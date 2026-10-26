#!/bin/bash
#
# Run ./build.sh inside an Ubuntu 18.04 container (podman or docker).
#
# The SDK scripts can't handle spaces in the checkout path and need 2018-era
# host compilers; mounting the tree at /work in an old Ubuntu avoids both.
#
#   ./build-in-container.sh            full build (same as ./build.sh)
#   ./build-in-container.sh kernel     any build.sh target
#   ./build-in-container.sh shell      interactive shell in the build env
#
set -e

TOP_DIR=$(cd "$(dirname "$0")"; pwd)
# Tag follows the Containerfile, so changing the build deps rebuilds the image
IMAGE=retronsq-build:$(cksum < "$TOP_DIR/docker/Containerfile" | cut -d ' ' -f 1)

# Don't relabel the whole tree for SELinux (Fedora); harmless elsewhere
if command -v podman > /dev/null; then
	ENGINE=podman
	# Keep files owned by you
	RUN_OPTS="--userns=keep-id --security-opt label=disable"
elif command -v docker > /dev/null; then
	ENGINE=docker
	RUN_OPTS="--user $(id -u):$(id -g) --security-opt label=disable"
else
	echo "Install podman or docker first." >&2
	exit 1
fi

# Two builds sharing buildroot/output corrupt each other's packages
if command -v flock > /dev/null; then
	exec 9> "$TOP_DIR/.build.lock"
	if ! flock -n 9; then
		echo "Another build is already running in $TOP_DIR." >&2
		exit 1
	fi
fi

if ! $ENGINE image exists $IMAGE 2> /dev/null && \
		! $ENGINE image inspect $IMAGE > /dev/null 2>&1; then
	$ENGINE build -t $IMAGE -f "$TOP_DIR/docker/Containerfile" "$TOP_DIR/docker"
fi

if [ "$1" = shell ]; then
	CMD=(bash)
else
	CMD=(./build.sh "$@")
fi

TTY=
[ -t 0 ] && TTY=-it

exec $ENGINE run --rm $TTY $RUN_OPTS \
	-e HOME=/tmp \
	-v "$TOP_DIR":/work -w /work \
	$IMAGE "${CMD[@]}"

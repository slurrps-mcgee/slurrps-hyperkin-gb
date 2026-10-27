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
#   ./build-in-container.sh test [GROUP...]   run tests/run-all.sh
#
# The firmware version comes from the VERSION file; set RETRONSQ_VERSION to
# override it. CONTAINER_ENGINE=docker|podman picks the engine.
#
set -e

TOP_DIR=$(cd "$(dirname "$0")"; pwd)
# Tag follows the Containerfile, so changing the build deps rebuilds the image.
# Tests use the "test" stage: the build image plus SDL and Xvfb.
TAG=$(cksum < "$TOP_DIR/docker/Containerfile" | cut -d ' ' -f 1)
if [ "$1" = test ]; then
	STAGE=test
else
	STAGE=build
fi
IMAGE=retronsq-$STAGE:$TAG

# Don't relabel the whole tree for SELinux (Fedora); harmless elsewhere
if [ -z "$CONTAINER_ENGINE" ]; then
	command -v podman > /dev/null && CONTAINER_ENGINE=podman || CONTAINER_ENGINE=docker
fi
if [ "$CONTAINER_ENGINE" = podman ] && command -v podman > /dev/null; then
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

# Two builds sharing buildroot/output corrupt each other's packages (tests
# don't touch it)
if [ $STAGE = build ] && command -v flock > /dev/null; then
	exec 9> "$TOP_DIR/.build.lock"
	if ! flock -n 9; then
		echo "Another build is already running in $TOP_DIR." >&2
		exit 1
	fi
fi

if ! $ENGINE image exists $IMAGE 2> /dev/null && \
		! $ENGINE image inspect $IMAGE > /dev/null 2>&1; then
	$ENGINE build --target $STAGE -t $IMAGE -f "$TOP_DIR/docker/Containerfile" "$TOP_DIR/docker"
fi

if [ "$1" = shell ]; then
	CMD=(bash)
elif [ "$1" = test ]; then
	shift
	CMD=(tests/run-all.sh "$@")
else
	CMD=(./build.sh "$@")
fi

TTY=
[ -t 0 ] && TTY=-it

exec $ENGINE run --rm $TTY $RUN_OPTS \
	-e HOME=/tmp \
	${RETRONSQ_VERSION:+-e RETRONSQ_VERSION="$RETRONSQ_VERSION"} \
	-v "$TOP_DIR":/work -w /work \
	$IMAGE "${CMD[@]}"

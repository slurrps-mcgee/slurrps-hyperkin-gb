#!/bin/bash
#
# Publish the firmware you built as a GitHub release.
#
#   ./build-in-container.sh            build first (the version is VERSION)
#   ./release.sh [-n notes.md] [-y]    then publish it as release v<VERSION>
#
#   -n FILE   release notes shown above GitHub's change list
#   -y        don't ask before publishing
#
# It checks that the images in rockdev/ are for VERSION and newer than every
# firmware source, that the tree is committed and pushed (the release tag
# points at that commit), and that the release doesn't exist yet. It then
# uploads the SD card zip, update.img and SHA256SUMS.txt. 0.x versions are
# marked as pre-releases ("beta"). Needs the GitHub CLI (gh auth login).
#
set -e

TOP_DIR=$(cd "$(dirname "$0")"; pwd)
cd "$TOP_DIR"

NOTES_FILE=
YES=
while getopts "n:y" opt; do
	case $opt in
	n) NOTES_FILE=$OPTARG ;;
	y) YES=1 ;;
	*) sed -n '3,15p' "$0"; exit 1 ;;
	esac
done

fail() { echo "release: $*" >&2; exit 1; }

command -v gh > /dev/null || fail "install the GitHub CLI (gh) first"
gh auth status > /dev/null 2>&1 || fail "log in first: gh auth login"
[ -z "$NOTES_FILE" ] || [ -f "$NOTES_FILE" ] || fail "no notes file $NOTES_FILE"

VERSION=$(tr -d ' \n' < VERSION)
echo "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$' ||
	fail "VERSION must look like 1.2.0 or 1.2.0-beta1, got '$VERSION'"
TAG=v$VERSION

# The images: built for this version, and not older than the sources
SD=rockdev/RetronSQ-CFW-$TAG-sdcard.zip
UPDATE=rockdev/update.img
[ -f "$SD" ] || fail "no $SD; build first: ./build-in-container.sh"
[ -f "$UPDATE" ] || fail "no $UPDATE; build first: ./build-in-container.sh"
NEWER=$(git ls-files -z -- buildroot/package buildroot/configs device kernel \
		u-boot build.sh VERSION |
	xargs -0 -r find 2> /dev/null -maxdepth 0 -newer "$UPDATE" | head -5)
[ -z "$NEWER" ] || fail "these changed after the last build; build again:
$NEWER"

# The release tag points at HEAD, so it must be what was built and on GitHub
[ -z "$(git status --porcelain --untracked-files=no)" ] ||
	fail "uncommitted changes; commit (and build) first"
git fetch -q origin
[ -n "$(git branch -r --contains HEAD)" ] || fail "HEAD isn't pushed; git push first"
REPO=$(gh repo view --json nameWithOwner -q .nameWithOwner)
gh release view "$TAG" --repo "$REPO" > /dev/null 2>&1 && fail "release $TAG already exists"

DIST=$(mktemp -d)
trap 'rm -rf "$DIST"' EXIT
cp "$SD" "$DIST/"
cp -L "$UPDATE" "$DIST/RetronSQ-CFW-$TAG-update.img"
(cd "$DIST" && sha256sum * > SHA256SUMS.txt)

{
	if [ -n "$NOTES_FILE" ]; then cat "$NOTES_FILE"; echo; fi
	echo "## Install"
	echo
	echo "1. Unzip \`RetronSQ-CFW-$TAG-sdcard.zip\`."
	echo "2. Write the \`.img\` to the console's microSD card (balenaEtcher,"
	echo "   GNOME Disks *Restore Disk Image*, or \`dd\`)."
	echo "3. Boot the console. On the first boot the games partition grows to"
	echo "   fill the card."
	echo
	echo "Then put the card in a PC: ROMs go in \`roms/gba\`, \`roms/gbc\`, \`roms/gb\`"
	echo "on the **RETRONSQ** drive. See the README for everything else."
	echo
	echo "\`RetronSQ-CFW-$TAG-update.img\` is the same firmware for Rockchip's"
	echo "RKDevTool / upgrade_tool."
} > "$DIST/notes.md"

TITLE="Retron SQ CFW $TAG"
FLAGS=()
case "$VERSION" in 0.*) TITLE="$TITLE beta"; FLAGS+=(--prerelease) ;; esac

echo "Release $TAG on $REPO, commit $(git rev-parse --short HEAD)${FLAGS:+ (pre-release)}:"
(cd "$DIST" && cat SHA256SUMS.txt)
if [ -z "$YES" ]; then
	read -r -p "Publish? [y/N] " ANSWER
	case "$ANSWER" in y|Y|yes) ;; *) echo "Not published."; exit 1 ;; esac
fi

gh release create "$TAG" "$DIST/$(basename "$SD")" \
	"$DIST/RetronSQ-CFW-$TAG-update.img" "$DIST/SHA256SUMS.txt" \
	--repo "$REPO" \
	--target "$(git rev-parse HEAD)" \
	--title "$TITLE" \
	--notes-file "$DIST/notes.md" \
	--generate-notes \
	"${FLAGS[@]}"

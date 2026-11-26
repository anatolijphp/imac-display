#!/usr/bin/env bash
# Builds the bootable installer image. Result: out/imac-display-installer-<date>-x86_64.iso
# Runs on a Mac or on Linux (also in GitHub Actions): anywhere Docker runs.
#
# Arch's image builder only runs on Arch Linux, so the build happens in a throwaway x86_64
# Arch container. Docker is a build tool only; nothing of it ends up in the image.
#
# The image installs the complete iMac side in one go: Arch Linux, the receiver (imacdisplayd,
# built here from receiver/), drive sharing, and the service that starts it at boot.
#
# Usage: linux/iso/build.sh
#   PUBKEY=path/to/key.pub   SSH public key allowed to log in as root (default ~/.ssh/id_ed25519.pub;
#                            PUBKEY=none or a missing file: no remote login, console only)
#   WITH_CLAUDE=1            also put Claude Code on the image (for developing on the iMac); default: no
#   CLAUDE_VERSION=x.y.z     with WITH_CLAUDE=1: pin the version (default: latest)
set -euo pipefail
cd "$(dirname "$0")"
REPO="$(cd ../.. && pwd)"
PUBKEY="${PUBKEY:-$HOME/.ssh/id_ed25519.pub}"

mkdir -p cache "$REPO/out"

if [ "${WITH_CLAUDE:-0}" = 1 ]; then
  ./fetch-claude.sh ${CLAUDE_VERSION:+"$CLAUDE_VERSION"}
fi

rm -f cache/authorized_keys
if [ "$PUBKEY" != none ] && [ -s "$PUBKEY" ]; then
  cp "$PUBKEY" cache/authorized_keys
else
  echo "build: no SSH public key ($PUBKEY): the installed iMac will have no remote login" >&2
fi

# The repo is baked into the image as a git bundle of what is committed.
if [ -n "$(git -C "$REPO" status --porcelain)" ]; then
  echo "build: WARNING: uncommitted changes will not be in the image's copy of the repo" >&2
fi
rm -f cache/imac-display.bundle
git -C "$REPO" bundle create "$PWD/cache/imac-display.bundle" --all

docker build --platform linux/amd64 -t imac-iso-builder .

docker run --rm --privileged --platform linux/amd64 \
  -v "$REPO":/src:ro \
  -v "$PWD/cache":/cache:ro \
  -v "$REPO/out":/out \
  -e WITH_CLAUDE="${WITH_CLAUDE:-0}" \
  imac-iso-builder /src/linux/iso/container-build.sh

echo "build: image is in $REPO/out"
ls -lh "$REPO"/out/*.iso

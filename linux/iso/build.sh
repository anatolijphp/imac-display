#!/usr/bin/env bash
# Builds the bootable live image on a Mac. Result: out/imac-display-live-<date>-x86_64.iso
#
# Arch's image builder only runs on Linux, so the build happens in a throwaway x86_64
# Arch container. Docker is a build tool only; nothing of it ends up in the image.
#
# Usage: linux/iso/build.sh
#   PUBKEY=path/to/key.pub   SSH public key allowed to log in as root (default ~/.ssh/id_ed25519.pub)
#   CLAUDE_VERSION=x.y.z     pin the Claude Code version (default: latest)
set -euo pipefail
cd "$(dirname "$0")"
REPO="$(cd ../.. && pwd)"
PUBKEY="${PUBKEY:-$HOME/.ssh/id_ed25519.pub}"

mkdir -p cache "$REPO/out"

./fetch-claude.sh ${CLAUDE_VERSION:+"$CLAUDE_VERSION"}

if [ ! -f "$PUBKEY" ]; then
  echo "build: SSH public key not found: $PUBKEY" >&2
  exit 1
fi
cp "$PUBKEY" cache/authorized_keys

docker build --platform linux/amd64 -t imac-iso-builder .

docker run --rm --privileged --platform linux/amd64 \
  -v "$REPO":/src:ro \
  -v "$PWD/cache":/cache:ro \
  -v "$REPO/out":/out \
  imac-iso-builder /src/linux/iso/container-build.sh

echo "build: image is in $REPO/out"
ls -lh "$REPO"/out/*.iso

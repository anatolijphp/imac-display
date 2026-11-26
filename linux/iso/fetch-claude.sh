#!/usr/bin/env bash
# Downloads the official Claude Code Linux x64 binary into cache/ and verifies its
# SHA-256 against Anthropic's release manifest. Same source and check as
# https://claude.ai/install.sh, without running the binary (the host is not Linux).
#
# Usage: fetch-claude.sh [VERSION]     (default: latest)
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p cache

BASE="https://downloads.claude.ai/claude-code-releases"
PLATFORM="linux-x64"
VERSION="${1:-$(curl -fsSL "$BASE/latest")}"
if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+ ]]; then
  echo "fetch-claude: unexpected version string: $VERSION" >&2
  exit 1
fi

SUM="$(curl -fsSL "$BASE/$VERSION/manifest.json" \
  | python3 -c 'import json,sys; print(json.load(sys.stdin)["platforms"][sys.argv[1]]["checksum"])' "$PLATFORM")"
if [[ ! "$SUM" =~ ^[a-f0-9]{64}$ ]]; then
  echo "fetch-claude: no checksum for $PLATFORM in manifest" >&2
  exit 1
fi

sha() { shasum -a 256 "$1" | cut -d' ' -f1; }

if [ -f cache/claude ] && [ "$(sha cache/claude)" = "$SUM" ]; then
  echo "fetch-claude: $VERSION already in cache"
else
  echo "fetch-claude: downloading Claude Code $VERSION ($PLATFORM)"
  curl -fsSL -o cache/claude.part "$BASE/$VERSION/$PLATFORM/claude"
  if [ "$(sha cache/claude.part)" != "$SUM" ]; then
    echo "fetch-claude: checksum mismatch, download discarded" >&2
    rm -f cache/claude.part
    exit 1
  fi
  mv cache/claude.part cache/claude
fi
chmod +x cache/claude
echo "$VERSION" > cache/claude.version
echo "fetch-claude: ok, sha256 $SUM"

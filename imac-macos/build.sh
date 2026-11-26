#!/bin/bash
# Builds "Restart in Linux.app", the small app for macOS ON THE iMAC that restarts it into Linux.
# The app is two shell scripts in an app bundle: nothing is compiled, so it runs on any macOS
# the iMac can have (10.11 or later). Needs a Mac (plutil, ditto).
#
#   imac-macos/build.sh        ->  out/build.noindex/Restart in Linux.app
#                                  out/Restart in Linux.zip
set -euo pipefail
cd "$(dirname "$0")"
OUT=../out
APP="$OUT/build.noindex/Restart in Linux.app"

bash -n app/Contents/MacOS/restart-in-linux
bash -n app/Contents/Resources/restart-in-linux-helper
plutil -lint app/Contents/Info.plist >/dev/null

rm -rf "$APP"
mkdir -p "$OUT/build.noindex"
cp -R app "$APP"
chmod 755 "$APP/Contents/MacOS/restart-in-linux" "$APP/Contents/Resources/restart-in-linux-helper"
xattr -cr "$APP" 2>/dev/null || true

# A .zip made by ditto keeps the scripts executable.
rm -f "$OUT/Restart in Linux.zip"
ditto -c -k --keepParent "$APP" "$OUT/Restart in Linux.zip"
echo "built: $(cd "$OUT" && pwd)/build.noindex/Restart in Linux.app"
echo "       $(cd "$OUT" && pwd)/Restart in Linux.zip"

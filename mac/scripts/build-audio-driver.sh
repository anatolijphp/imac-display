#!/bin/bash
# Builds the "iMac Speakers" audio driver (out/iMacDisplayAudio.driver) with the Command Line Tools,
# signs it, and runs its self-test. Installing it is a separate step: install-audio-driver.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="../out"
DRIVER="$OUT/iMacDisplayAudio.driver"
rm -rf "$DRIVER"
mkdir -p "$DRIVER/Contents/MacOS"
clang -Wall -Wextra -Wno-unused-parameter -O2 -arch arm64 -arch x86_64 -mmacosx-version-min=14.0 -bundle \
    -o "$DRIVER/Contents/MacOS/iMacDisplayAudio" AudioDriver/iMacDisplayAudio.c -framework CoreAudio -framework CoreFoundation
cp AudioDriver/Info.plist "$DRIVER/Contents/Info.plist"
codesign --force --sign "${SIGN_IDENTITY:--}" --identifier local.imac-display.audio "$DRIVER"
codesign --verify --strict "$DRIVER"
clang -Wall -O1 -o "$OUT/test-audio-driver" AudioDriver/test-driver.c -framework CoreAudio -framework CoreFoundation
"$OUT/test-audio-driver" "$DRIVER/Contents/MacOS/iMacDisplayAudio"
echo "built: $(cd "$OUT" && pwd)/iMacDisplayAudio.driver"

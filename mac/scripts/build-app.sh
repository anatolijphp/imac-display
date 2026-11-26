#!/bin/bash
# Builds "iMac Display.app" with the Command Line Tools only (no Xcode) and signs it.
#
#   mac/scripts/build-app.sh                     ad-hoc signature (runs on this Mac only)
#   SIGN_IDENTITY="My Cert" mac/scripts/build-app.sh   sign with a certificate from the keychain
#
# With an ad-hoc signature macOS ties the Screen Recording permission to this exact build, so it
# asks again after every rebuild. A certificate (even a self-signed one) keeps the permission.
set -euo pipefail
cd "$(dirname "$0")/.."
swift build -c release
APP="../out/iMac Display.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp .build/release/imd "$APP/Contents/MacOS/iMacDisplay"
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key><string>local.imac-display.mac</string>
    <key>CFBundleName</key><string>iMac Display</string>
    <key>CFBundleDisplayName</key><string>iMac Display</string>
    <key>CFBundleExecutable</key><string>iMacDisplay</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>LSMinimumSystemVersion</key><string>14.0</string>
    <key>LSUIElement</key><true/>
    <key>NSLocalNetworkUsageDescription</key><string>iMac Display finds and talks to your iMac over the Thunderbolt cable.</string>
    <key>NSBonjourServices</key><array><string>_imacdisplay._tcp</string></array>
</dict>
</plist>
PLIST
codesign --force --sign "${SIGN_IDENTITY:--}" --identifier local.imac-display.mac "$APP"
codesign --verify --strict --verbose=2 "$APP"
echo "built: $(cd "$(dirname "$APP")" && pwd)/$(basename "$APP")"

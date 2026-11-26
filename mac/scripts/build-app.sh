#!/bin/bash
# Builds "iMac Display.app": the one thing to install on the MacBook. Command Line Tools only (no Xcode).
# The app carries the "iMac Speakers" sound device inside and installs it itself when asked.
#
#   mac/scripts/build-app.sh                            signs with the local identity from
#                                                       make-signing-identity.sh if there is one, else ad hoc
#   SIGN_IDENTITY="My Cert" mac/scripts/build-app.sh    sign with a certificate from the login keychain
#
# Result: out/build.noindex/iMac Display.app and out/iMac Display.dmg (the app plus a shortcut to
# Applications). The folder name ends in .noindex so that Spotlight and Launchpad do not show this
# build next to the installed copy in /Applications.
#
# With an ad-hoc signature macOS ties its permissions (Screen Recording, Accessibility, Microphone)
# to this exact build and asks again after every rebuild. A certificate keeps them:
# run mac/scripts/make-signing-identity.sh once.
set -euo pipefail
cd "$(dirname "$0")/.."
SIGN="${SIGN_IDENTITY:--}"
SIGN_ARGS=()
LOCAL_KEYCHAIN="$(cd .. && pwd)/out/signing/imac-display.keychain-db"
if [ -z "${SIGN_IDENTITY:-}" ] && [ -f "$LOCAL_KEYCHAIN" ]; then
    security unlock-keychain -p "$(cat "$(dirname "$LOCAL_KEYCHAIN")/keychain-pass")" "$LOCAL_KEYCHAIN"
    SIGN="iMac Display Local Signing"
    SIGN_ARGS=(--keychain "$LOCAL_KEYCHAIN")
fi
scripts/build-audio-driver.sh
swift build -c release
mkdir -p ../out/build.noindex
APP="../out/build.noindex/iMac Display.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp .build/release/iMacDisplayApp "$APP/Contents/MacOS/iMacDisplay"
cp -R ../out/iMacDisplayAudio.driver "$APP/Contents/Resources/"
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
    <key>CFBundleShortVersionString</key><string>0.5</string>
    <key>CFBundleVersion</key><string>5</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>LSMinimumSystemVersion</key><string>14.0</string>
    <key>LSUIElement</key><true/>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSLocalNetworkUsageDescription</key><string>iMac Display finds and talks to your iMac over the Thunderbolt cable.</string>
    <key>NSBonjourServices</key><array><string>_imacdisplay._tcp</string></array>
    <key>NSMicrophoneUsageDescription</key><string>iMac Display records only from its own “iMac Speakers” device, to send that sound to your iMac. It does not use a microphone.</string>
</dict>
</plist>
PLIST
# The sound device stays ad-hoc signed: that is what macOS's audio service has loaded so far.
codesign --force --sign - --identifier local.imac-display.audio "$APP/Contents/Resources/iMacDisplayAudio.driver"
codesign --force ${SIGN_ARGS[@]+"${SIGN_ARGS[@]}"} --sign "$SIGN" --identifier local.imac-display.mac "$APP"
codesign --verify --strict --verbose=2 "$APP" || echo "note: a self-signed certificate does not pass --verify; the app still runs"
codesign -d -r- "$APP" 2>&1 | tail -1
DMG="../out/iMac Display.dmg"
STAGE="$(mktemp -d)"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
rm -f "$DMG"
hdiutil create -quiet -volname "iMac Display" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
rm -rf "$STAGE"
echo "built: $(cd ../out && pwd)/build.noindex/iMac Display.app"
echo "       $(cd ../out && pwd)/iMac Display.dmg"

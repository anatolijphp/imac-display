#!/bin/bash
# Makes a signing identity for this Mac only, so that macOS keeps the app's permissions (Screen
# Recording, Accessibility, Microphone) from one build to the next. With an ad-hoc signature every
# rebuild counts as a new app and macOS asks again.
#
# It is a self-signed certificate in its own keychain file under out/signing (never the login
# keychain, nothing added to the system's trust settings). build-app.sh uses it when it exists.
# Run once; delete out/signing to go back to ad-hoc signing.
set -euo pipefail
cd "$(dirname "$0")/../.."
D="$PWD/out/signing"
K="$D/imac-display.keychain-db"
[ -f "$K" ] && { echo "already there: $K"; exit 0; }
mkdir -p "$D"
cd "$D"
/usr/bin/openssl rand -hex 16 > keychain-pass
chmod 600 keychain-pass
/usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 3650 \
    -subj "/CN=iMac Display Local Signing" -addext "keyUsage=critical,digitalSignature" \
    -addext "extendedKeyUsage=critical,codeSigning" -addext "basicConstraints=critical,CA:false" 2>/dev/null
/usr/bin/openssl pkcs12 -export -inkey key.pem -in cert.pem -out id.p12 -passout pass:imd -name "iMac Display Local Signing"
P="$(cat keychain-pass)"
security create-keychain -p "$P" "$K"
security set-keychain-settings "$K"
security unlock-keychain -p "$P" "$K"
security import id.p12 -k "$K" -P imd -T /usr/bin/codesign
security set-key-partition-list -S apple-tool:,apple: -s -k "$P" "$K" >/dev/null
rm -f key.pem id.p12
echo "made: $K"

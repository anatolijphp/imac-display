#!/bin/bash
# Installs (or with "remove": removes) the "iMac Speakers" audio driver. macOS asks for an
# administrator password, because audio drivers live in /Library. Sound on this Mac stops for a
# second or two while the audio service restarts.
#
#   mac/scripts/install-audio-driver.sh           install or update
#   mac/scripts/install-audio-driver.sh remove    uninstall
set -euo pipefail
cd "$(dirname "$0")/.."
SRC="$(cd ../out && pwd)/iMacDisplayAudio.driver"
DST="/Library/Audio/Plug-Ins/HAL/iMacDisplayAudio.driver"
RESTART="launchctl kickstart -k system/com.apple.audio.coreaudiod || killall coreaudiod"
if [ "${1:-}" = "remove" ]; then
    CMD="rm -rf '$DST'; $RESTART"
    WHAT="remove the iMac Speakers audio driver"
else
    [ -d "$SRC" ] || { echo "build it first: mac/scripts/build-audio-driver.sh" >&2; exit 1; }
    CMD="mkdir -p /Library/Audio/Plug-Ins/HAL && rm -rf '$DST' && cp -R '$SRC' '$DST' && chown -R root:wheel '$DST' && { $RESTART; }"
    WHAT="install the iMac Speakers audio driver"
fi
if [ "$(id -u)" = 0 ]; then
    sh -c "$CMD"
else
    # A graphical password prompt, so this also works where sudo cannot ask.
    osascript -e "do shell script \"$CMD\" with prompt \"iMac Display wants to $WHAT.\" with administrator privileges" >/dev/null
fi
sleep 2
if [ "${1:-}" = "remove" ]; then echo "removed"; else
    system_profiler SPAudioDataType 2>/dev/null | grep -q "iMac Speakers" && echo "installed: \"iMac Speakers\" is now in the sound output list" \
        || { echo "installed the files, but \"iMac Speakers\" did not appear. Check: log show --last 1m --predicate 'process == \"coreaudiod\"'" >&2; exit 1; }
fi

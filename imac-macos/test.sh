#!/bin/bash
# Tests "Restart in Linux.app" as far as a Mac that is not the iMac can: with a disk image that
# looks like the Linux boot partition, in dry-run mode. Nothing is changed and nothing restarts.
# Not covered here (only the iMac can show it): the real bless, the password dialog, the restart.
set -uo pipefail
cd "$(dirname "$0")"
APP="../out/build.noindex/Restart in Linux.app"
[ -d "$APP" ] || ./build.sh >/dev/null
MAIN="$APP/Contents/MacOS/restart-in-linux"
HELPER="$APP/Contents/Resources/restart-in-linux-helper"
WORK="$(mktemp -d)"
DISK=""
failed=0
check() { # NAME EXPECTED-PATTERN ACTUAL
    if echo "$3" | grep -Eq "$2"; then echo "ok    $1"; else echo "FAIL  $1: got: $3"; failed=1; fi
}
detach() { [ -n "$DISK" ] && hdiutil detach "$DISK" -force >/dev/null 2>&1; DISK=""; }
trap 'detach; rm -rf "$WORK"' EXIT

if diskutil list | grep -q LINUXBOOT; then echo "a volume named LINUXBOOT is already attached; not testing next to it"; exit 1; fi

check "no partition: helper" "No partition named LINUXBOOT" "$(RIL_DRY_RUN=1 "$HELPER" 2>&1)"
check "no partition: app" "no partition named LINUXBOOT" "$(RIL_DRY_RUN=1 RIL_ANSWER=Restart "$MAIN" 2>&1)"

# image NAME WITH-LOADER: a FAT volume labelled LINUXBOOT, attached but not mounted (as on the iMac).
image() {
    hdiutil create -quiet -size 40m -fs MS-DOS -volname LINUXBOOT -layout GPTSPUD "$WORK/$1.dmg"
    if [ "$2" = yes ]; then
        local dev mnt
        dev="$(hdiutil attach -nobrowse "$WORK/$1.dmg" 2>/dev/null | awk '/LINUXBOOT/ {print $1}')"
        mnt="$(diskutil info "$dev" | sed -n 's/^ *Mount Point: *//p')"
        mkdir -p "$mnt/EFI/BOOT" "$mnt/loader"
        echo fake > "$mnt/EFI/BOOT/BOOTX64.EFI"
        echo "timeout 3" > "$mnt/loader/loader.conf"
        hdiutil detach "$dev" >/dev/null 2>&1
    fi
    DISK="$(hdiutil attach -nomount "$WORK/$1.dmg" 2>/dev/null | awk 'NR==1 {print $1}')"
}

image empty no
check "partition without Linux" "does not hold the Linux boot loader" "$(RIL_DRY_RUN=1 "$HELPER" 2>&1)"
detach

image linux yes
check "find" "^disk[0-9]+s[0-9]+$" "$("$HELPER" --find 2>&1)"
check "helper" "would run: bless --mount /Volumes/LINUXBOOT --file /Volumes/LINUXBOOT/EFI/BOOT/BOOTX64.EFI --setBoot --nextonly --shortform" "$(RIL_DRY_RUN=1 "$HELPER" 2>&1)"
check "helper unmounts what it mounted" "^$" "$(diskutil info "$("$HELPER" --find)" | sed -n 's/^ *Mount Point: *//p')"
check "app, Restart" "would run: bless.*would restart" "$(RIL_DRY_RUN=1 RIL_ANSWER=Restart "$MAIN" 2>&1 | tr '\n' ' ')"
check "app, Never Ask Again" "(would install|does not read /etc/sudoers.d).*would run: bless.*would restart" "$(RIL_DRY_RUN=1 RIL_ANSWER='Restart, Never Ask Again' "$MAIN" 2>&1 | tr '\n' ' ')"
check "app, Cancel" "^$" "$(RIL_DRY_RUN=1 RIL_ANSWER=Cancel "$MAIN" 2>&1)"
check "refuses without root" "must run as root" "$("$HELPER" 2>&1)"
check "refuses arguments" "usage:" "$("$HELPER" /Volumes/Other 2>&1)"
detach

# The sudo rule the helper writes must pass visudo's own check.
rule="$WORK/rule"
echo '#501 ALL=(root) NOPASSWD: /Library/PrivilegedHelperTools/local.imac-display.restart-in-linux ""' > "$rule"
check "sudo rule syntax" "parsed OK" "$(visudo -cf "$rule" 2>&1)"

[ $failed = 0 ] && echo "all passed" || { echo "FAILED"; exit 1; }

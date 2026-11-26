#!/usr/bin/env bash
# Writes the bootable installer USB stick on macOS. ERASES THE WHOLE STICK.
#
# The stick gets one FAT32 partition holding the contents of the installer ISO.
# The installer finds its boot medium by looking for /boot/<uuid>.uuid on any drive,
# so a plain copy of the ISO's files boots the same as the ISO itself. No sudo needed.
#
# Usage: tools/make-usb.sh diskN --erase diskN [path/to/image.iso]
#   The disk name is given twice on purpose, to make erasing the wrong disk harder.
#   Find it with: diskutil list external physical
#
# If the iMac's boot picker does not show the stick, write the ISO raw instead:
#   diskutil unmountDisk diskN && sudo dd if=out/<image>.iso of=/dev/rdiskN bs=4m
set -euo pipefail

die() { echo "make-usb: $*" >&2; exit 1; }

DISK="${1:-}"
[ "${2:-}" = "--erase" ] && [ -n "${3:-}" ] || die "usage: $0 diskN --erase diskN [image.iso]"
[ "$DISK" = "$3" ] || die "the two disk names differ ($DISK and $3); nothing was changed"
[[ "$DISK" =~ ^disk[0-9]+$ ]] || die "give the whole disk, for example disk4 (not disk4s1, not /dev/disk4)"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
ISO="${4:-$(ls -t "$REPO"/out/imac-display-installer-*.iso 2>/dev/null | head -1 || true)}"
[ -f "$ISO" ] || die "no image found; build it first with linux/iso/build.sh"

# The stick must hold the image plus 10% and 100 MB for FAT32 and partition overhead.
ISO_BYTES="$(stat -f %z "$ISO")"
MIN_BYTES=$(( ISO_BYTES + ISO_BYTES / 10 + 100 * 1000 * 1000 ))

# --- refuse anything that is not an external USB whole disk ---------------------------
INFO="$(diskutil info "$DISK")" || die "no such disk: $DISK"
field() { awk -F': *' -v k="^ *$1\$" '$1 ~ k {print $2; exit}' <<<"$INFO"; }
[ "$(field 'Whole')" = "Yes" ] || die "$DISK is not a whole disk"
[ "$(field 'Device Location')" = "External" ] || die "$DISK is not an external disk; refusing"
[ "$(field 'Protocol')" = "USB" ] || die "$DISK is not a USB device (protocol: $(field 'Protocol')); refusing"
BYTES="$(awk -F'[()]' '/Disk Size/ {print $2; exit}' <<<"$INFO" | awk '{print $1}')"
[ "${BYTES:-0}" -ge "$MIN_BYTES" ] || die "$DISK is too small: the image needs a stick of at least $((MIN_BYTES / 1000000)) MB"

echo "make-usb: erasing $DISK: $(field 'Device / Media Name'), $(field 'Disk Size' | cut -d'(' -f1)"
echo "make-usb: image $ISO"

# --- partition and copy -----------------------------------------------------------------
diskutil partitionDisk "$DISK" GPT "MS-DOS FAT32" IMACINST R

VOL=/Volumes/IMACINST
[ -d "$VOL" ] || die "the new volume did not mount"
mdutil -i off "$VOL" >/dev/null 2>&1 || true

echo "make-usb: copying the installer (several minutes on a USB 2.0 stick)"
bsdtar -x -f "$ISO" -C "$VOL"

# --- verify -----------------------------------------------------------------------------
echo "make-usb: verifying"
[ -f "$VOL/EFI/BOOT/BOOTx64.EFI" ] || die "boot loader missing on the stick"
ls "$VOL"/boot/*.uuid >/dev/null 2>&1 || die "boot-medium marker (boot/<uuid>.uuid) missing"
SFS="$VOL/arch/x86_64/airootfs.sfs"
[ -f "$SFS" ] || die "system image missing"
if [ -f "$VOL/arch/x86_64/airootfs.sha512" ]; then
  WANT="$(awk '{print $1}' "$VOL/arch/x86_64/airootfs.sha512")"
  GOT="$(shasum -a 512 "$SFS" | awk '{print $1}')"
  [ "$WANT" = "$GOT" ] || die "the system image is corrupt on the stick (checksum mismatch)"
  echo "make-usb: system image checksum ok"
fi

dot_clean -m "$VOL" 2>/dev/null || true
sync
diskutil eject "$DISK"
echo "make-usb: done. The stick can be unplugged."

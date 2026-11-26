#!/usr/bin/env bash
# Writes the bootable USB stick on macOS. ERASES THE WHOLE STICK.
#
#   Partition 1  ARCHISO   FAT32  the live system (contents of the ISO); the iMac boots from it
#   Partition 2  IMACDATA  FAT32  this repo as a git bundle; the live system converts it to
#                                 ext4 on first boot and keeps its saved data there
#
# Usage: tools/make-usb.sh diskN --erase diskN [path/to/image.iso]
#   The disk name is given twice on purpose, to make erasing the wrong disk harder.
#   Find it with: diskutil list external physical
set -euo pipefail

die() { echo "make-usb: $*" >&2; exit 1; }

DISK="${1:-}"
[ "${2:-}" = "--erase" ] && [ -n "${3:-}" ] || die "usage: $0 diskN --erase diskN [image.iso]"
[ "$DISK" = "$3" ] || die "the two disk names differ ($DISK and $3); nothing was changed"
[[ "$DISK" =~ ^disk[0-9]+$ ]] || die "give the whole disk, for example disk4 (not disk4s1, not /dev/disk4)"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
ISO="${4:-$(ls -t "$REPO"/out/imac-display-live-*.iso 2>/dev/null | head -1 || true)}"
[ -f "$ISO" ] || die "no image found; build it first with linux/iso/build.sh"

BOOT_GB=6
MIN_BYTES=$((14 * 1000 * 1000 * 1000))

# --- refuse anything that is not an external, removable, whole disk ----------------
INFO="$(diskutil info "$DISK")" || die "no such disk: $DISK"
field() { awk -F': *' -v k="^ *$1\$" '$1 ~ k {print $2; exit}' <<<"$INFO"; }
[ "$(field 'Whole')" = "Yes" ] || die "$DISK is not a whole disk"
[ "$(field 'Device Location')" = "External" ] || die "$DISK is not an external disk; refusing"
[ "$(field 'Protocol')" = "USB" ] || die "$DISK is not a USB device (protocol: $(field 'Protocol')); refusing"
BYTES="$(awk -F'[()]' '/Disk Size/ {print $2; exit}' <<<"$INFO" | awk '{print $1}')"
[ "${BYTES:-0}" -ge "$MIN_BYTES" ] || die "$DISK is smaller than 14 GB"

echo "make-usb: erasing $DISK: $(field 'Device / Media Name'), $(field 'Disk Size' | cut -d'(' -f1)"
echo "make-usb: image $ISO"

# --- partition -----------------------------------------------------------------------
diskutil partitionDisk "$DISK" GPT \
  "MS-DOS FAT32" ARCHISO "${BOOT_GB}G" \
  "MS-DOS FAT32" IMACDATA R

BOOT=/Volumes/ARCHISO
DATA=/Volumes/IMACDATA
[ -d "$BOOT" ] && [ -d "$DATA" ] || die "the new volumes did not mount"
mdutil -i off "$BOOT" "$DATA" >/dev/null 2>&1 || true

# --- live system: extract the ISO's file tree onto the FAT32 boot partition ---------------
# The live system finds its boot medium by looking for /boot/<uuid>.uuid on any drive,
# so a plain copy of the files boots the same as the ISO itself.
echo "make-usb: copying the live system (a few minutes)"
bsdtar -x -f "$ISO" -C "$BOOT"

# --- data partition: the repo as one bundle file, plus the marker the live system checks ---
echo "make-usb: writing the project repo"
git -C "$REPO" bundle create "$DATA/imac-display.bundle" --all
git -C "$REPO" bundle verify "$DATA/imac-display.bundle" >/dev/null
cat > "$DATA/IMACDATA.marker" <<EOF
iMac Display data partition. Created $(date -u +%Y-%m-%dT%H:%M:%SZ).
The live system converts this partition to ext4 on first boot.
EOF

# --- verify ---------------------------------------------------------------------------------
echo "make-usb: verifying"
[ -f "$BOOT/EFI/BOOT/BOOTx64.EFI" ] || die "boot loader missing on the stick"
ls "$BOOT"/boot/*.uuid >/dev/null 2>&1 || die "boot-medium marker (boot/<uuid>.uuid) missing"
SFS="$BOOT/arch/x86_64/airootfs.sfs"
[ -f "$SFS" ] || die "root filesystem image missing"
if [ -f "$BOOT/arch/x86_64/airootfs.sha512" ]; then
  WANT="$(awk '{print $1}' "$BOOT/arch/x86_64/airootfs.sha512")"
  GOT="$(shasum -a 512 "$SFS" | awk '{print $1}')"
  [ "$WANT" = "$GOT" ] || die "root filesystem image is corrupt on the stick (checksum mismatch)"
  echo "make-usb: root filesystem checksum ok"
fi

dot_clean -m "$BOOT" 2>/dev/null || true
sync
diskutil eject "$DISK"
echo "make-usb: done. The stick can be unplugged."

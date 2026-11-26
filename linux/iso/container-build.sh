#!/usr/bin/env bash
# Runs INSIDE the builder container (see build.sh). Starts from Arch's stock "releng"
# install-medium profile, layers our changes on top, and builds the installer ISO into /out.
#
# Mounts expected:  /src   the repo (read-only)
#                   /cache claude binary, authorized_keys and the repo bundle from the host (read-only)
#                   /out   where the ISO is written
set -euo pipefail

SRC=/src/linux/iso
CACHE=/cache
PROFILE=/build/profile
WORK=/work

rm -rf /build "$WORK"
mkdir -p /build
cp -r /usr/share/archiso/configs/releng "$PROFILE"

# --- packages -----------------------------------------------------------------
grep -vE '^\s*(#|$)' "$SRC/packages.extra" >> "$PROFILE/packages.x86_64"
grep -vE '^\s*(#|$)' "$PROFILE/packages.x86_64" | sort -u > "$PROFILE/packages.tmp"
mv "$PROFILE/packages.tmp" "$PROFILE/packages.x86_64"

# pacman's download sandbox does not work under Rosetta x86 emulation. This pacman.conf
# is only used to build the image; the live system keeps Arch's stock configuration.
sed -i 's/^\[options\]/[options]\nDisableSandbox/' "$PROFILE/pacman.conf"

# --- files added to the live system -------------------------------------------
ROOTFS="$PROFILE/airootfs"
cp -r "$SRC/airootfs/." "$ROOTFS/"
install -Dm755 "$CACHE/claude" "$ROOTFS/usr/local/bin/claude"
install -Dm600 "$CACHE/authorized_keys" "$ROOTFS/root/.ssh/authorized_keys"
# The project repo travels inside the image; imac-install clones it onto the iMac.
install -Dm644 "$CACHE/imac-display.bundle" "$ROOTFS/usr/share/imac-display/imac-display.bundle"

WANTS="$ROOTFS/etc/systemd/system/multi-user.target.wants"
mkdir -p "$WANTS"
ln -sf /etc/systemd/system/imac-wifi-select.service "$WANTS/imac-wifi-select.service"

# --- boot menu ------------------------------------------------------------------
LOADER="$PROFILE/efiboot/loader"
ENTRY="$LOADER/entries/01-archiso-linux.conf"
sed -i -e 's/^title .*/title    iMac Display installer (Arch Linux)/' \
       -e '/^options /s/$/ fbcon=font:TER16x32/' "$ENTRY"
sed -e 's/^title .*/title    iMac Display installer, older Radeon GPU (R9 M380 or M390)/' \
    -e 's/^sort-key .*/sort-key 01a/' \
    -e '/^options /s/$/ amdgpu.si_support=1 radeon.si_support=0 amdgpu.cik_support=1 radeon.cik_support=0/' \
    "$ENTRY" > "$LOADER/entries/01a-imac-oldgpu.conf"
sed -e 's/^title .*/title    iMac Display installer, safe graphics (if the screen stays black)/' \
    -e 's/^sort-key .*/sort-key 01b/' \
    -e '/^options /s/$/ nomodeset/' \
    "$ENTRY" > "$LOADER/entries/01b-imac-safe-graphics.conf"
sed -i -e 's/^timeout .*/timeout 8/' -e 's/^beep on/beep off/' "$LOADER/loader.conf"

# --- image settings ---------------------------------------------------------------
cat >> "$PROFILE/profiledef.sh" <<'EOF'

# --- iMac Display overrides ---
iso_name="imac-display-installer"
iso_application="iMac Display installer"
# Intel Macs boot through UEFI; no BIOS boot path needed.
bootmodes=('uefi.systemd-boot')
# zstd: much faster to build under emulation than xz, and faster to boot.
airootfs_image_tool_options=('-comp' 'zstd' '-Xcompression-level' '15' '-b' '1M')
file_permissions+=(
  ["/root/.ssh"]="0:0:700"
  ["/root/.ssh/authorized_keys"]="0:0:600"
  ["/usr/local/bin/claude"]="0:0:755"
  ["/usr/local/bin/imac-install"]="0:0:755"
  ["/usr/local/bin/imac-wifi-select"]="0:0:755"
  ["/usr/local/bin/m0-check"]="0:0:755"
)
EOF

# --- build -----------------------------------------------------------------------
mkarchiso -v -w "$WORK" -o /out "$PROFILE"

# --- checks ----------------------------------------------------------------------
echo
echo "===== verification ====="
ROOT="$WORK/x86_64/airootfs"
ISO="$(ls -t /out/imac-display-installer-*.iso | head -1)"
echo "ISO: $ISO ($(du -h "$ISO" | cut -f1))"
echo "-- boot files in the ISO"
bsdtar -tf "$ISO" | grep -iE 'BOOTx64\.EFI$|vmlinuz-linux$|initramfs-linux\.img$|airootfs\.sfs$|loader/entries/.*\.conf$|\.uuid$' || true
echo "-- kernel"
arch-chroot "$ROOT" pacman -Q linux linux-headers 2>/dev/null || true
echo "-- wl module built by dkms (optional)"
find "$ROOT/usr/lib/modules" -name 'wl.ko*' 2>/dev/null || true
echo "-- our files"
ls -la "$ROOT/usr/local/bin/" "$ROOT/root/.ssh/" "$ROOT/usr/share/imac-display/" 2>/dev/null || true
echo "-- claude --version inside the image"
arch-chroot "$ROOT" /usr/local/bin/claude --version || echo "WARN: claude did not run in the chroot (may be the emulator, check on real hardware)"
echo "===== done ====="

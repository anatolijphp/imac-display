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
# Claude Code only when asked for (WITH_CLAUDE=1): it is a development tool, not part of the product.
if [ "${WITH_CLAUDE:-0}" = 1 ] && [ -f "$CACHE/claude" ]; then
  install -Dm755 "$CACHE/claude" "$ROOTFS/usr/local/bin/claude"
fi
if [ -s "$CACHE/authorized_keys" ]; then
  install -Dm600 "$CACHE/authorized_keys" "$ROOTFS/root/.ssh/authorized_keys"
fi
# The project repo travels inside the image; imac-install clones it onto the iMac.
if [ -f "$CACHE/imac-display.bundle" ]; then
  install -Dm644 "$CACHE/imac-display.bundle" "$ROOTFS/usr/share/imac-display/imac-display.bundle"
fi

# --- the receiver ---------------------------------------------------------------
# Built here, against the same Arch packages the image gets, so the installer can put a
# finished system on the iMac with no compiling and no internet there.
echo "===== building the receiver ====="
pacman -Syu --noconfirm --needed base-devel cmake pkgconf git libevdev libdrm cairo mesa libglvnd ffmpeg alsa-lib \
    nlohmann-json fuse3 bzip2 zlib >/dev/null
cmake -S /src/receiver -B "$WORK/receiver" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
make -C "$WORK/receiver" -j"$(nproc)"
install -Dm755 "$WORK/receiver/imacdisplayd" "$ROOTFS/usr/local/bin/imacdisplayd"
# Read-only APFS for sharing the iMac's own macOS volume: third-party source at a pinned, reviewed
# commit (linux/apfs-fuse/build.sh builds it and installs into this container's /usr/local/bin).
bash /src/linux/apfs-fuse/build.sh >/dev/null
install -Dm755 /usr/local/bin/apfs-fuse "$ROOTFS/usr/local/bin/apfs-fuse"
install -Dm755 /usr/local/bin/apfsutil "$ROOTFS/usr/local/bin/apfsutil"
# The appliance's configuration (service, DNS-SD, drive-sharing user, ...) is not active on the
# installer stick itself, which must stay a plain console: imac-install copies it onto the iMac
# and switches the receiver on there.
mkdir -p "$ROOTFS/usr/share/imac-display/rootfs"
cp -r /src/linux/rootfs/. "$ROOTFS/usr/share/imac-display/rootfs/"

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
  ["/usr/local/bin/imacdisplayd"]="0:0:755"
  ["/usr/local/bin/apfs-fuse"]="0:0:755"
  ["/usr/local/bin/apfsutil"]="0:0:755"
  ["/usr/local/bin/imac-install"]="0:0:755"
  ["/usr/local/bin/imac-wifi-select"]="0:0:755"
  ["/usr/local/bin/m0-check"]="0:0:755"
)
EOF

# Files that are only sometimes there: permissions for a missing file would stop the build.
if [ -f "$ROOTFS/root/.ssh/authorized_keys" ]; then
  echo 'file_permissions+=(["/root/.ssh"]="0:0:700" ["/root/.ssh/authorized_keys"]="0:0:600")' >> "$PROFILE/profiledef.sh"
fi
if [ -f "$ROOTFS/usr/local/bin/claude" ]; then
  echo 'file_permissions+=(["/usr/local/bin/claude"]="0:0:755")' >> "$PROFILE/profiledef.sh"
fi

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
echo "-- the receiver finds all its libraries inside the image"
if arch-chroot "$ROOT" ldd /usr/local/bin/imacdisplayd /usr/local/bin/apfs-fuse | grep "not found"; then
  echo "ERROR: the receiver was built against libraries the image does not have" >&2
  exit 1
fi
if [ -x "$ROOT/usr/local/bin/claude" ]; then
  echo "-- claude --version inside the image"
  arch-chroot "$ROOT" /usr/local/bin/claude --version || echo "WARN: claude did not run in the chroot (may be the emulator, check on real hardware)"
fi
echo "===== done ====="

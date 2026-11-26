#!/usr/bin/env bash
# End-to-end test of the installer in a QEMU virtual machine on the Mac.
# x86 is emulated in software, so it is slow (tens of minutes), but it runs the real
# image, the real imac-install and then boots what was installed.
#
#   prepare   build a virtual "iMac" disk with a macOS-like layout (made by diskutil,
#             the same tool Disk Utility uses) and a virtual USB stick written the
#             same way tools/make-usb.sh writes the real one
#   install   boot the stick, check that imac-install refuses without consent, then install
#   boot      boot the installed system from the disk and check it
#   verify    confirm the stand-in macOS partitions are byte-for-byte unchanged
#   all       everything in order
#
# The VM is driven over SSH with a throwaway key. The image only trusts the user's own
# key, so the test key is typed into the VM's console through the QEMU monitor.
#
# Needs: brew install qemu. Usage: tools/test-installer-vm.sh all [image.iso]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
W="$REPO/out/vmtest"
ISO="${2:-$(ls -t "$REPO"/out/imac-display-installer-*.iso 2>/dev/null | head -1 || true)}"
QEMU_SHARE="$(brew --prefix qemu)/share/qemu"
SSH_PORT=2222
HTTP_PORT=8765
KEY="$W/test_key"
STICK_BYTES=3880452096          # the size of the real 4 GB stick this was first written to

say() { echo "vmtest: $*"; }
die() { echo "vmtest: FAIL: $*" >&2; exit 1; }

attach() {  # raw image [extra hdiutil args] -> /dev/diskN
  local img="$1"; shift
  hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$@" "$img" | head -1 | awk '{print $1}'
}

part_hashes() {  # hashes of the partitions imac-install must never touch
  local dev n
  dev="$(attach "$W/disk.raw" -readonly)"
  for n in 1 2; do
    echo "s$n $(shasum -a 256 "/dev/r${dev#/dev/}s$n" | awk '{print $1}')"
  done
  hdiutil detach "$dev" >/dev/null
}

vssh() {
  ssh -p "$SSH_PORT" -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
      -o LogLevel=ERROR -o BatchMode=yes -o ConnectTimeout=8 -o ServerAliveInterval=15 \
      root@127.0.0.1 "$@"
}

vm_running() { [ -f "$W/qemu.pid" ] && kill -0 "$(cat "$W/qemu.pid")" 2>/dev/null; }

vm_start() {  # "stick" or "disk"
  local extra=()
  if [ "$1" = "stick" ]; then
    extra=(-drive "if=none,id=stick,format=raw,readonly=on,file=$W/stick.raw"
           -device "usb-storage,drive=stick,bootindex=0")
  fi
  rm -f "$W/mon.sock" "$W/qemu.pid"
  qemu-system-x86_64 -machine q35 -accel tcg,thread=multi -cpu max -smp 4 -m 4096 \
    -drive "if=pflash,format=raw,readonly=on,file=$QEMU_SHARE/edk2-x86_64-code.fd" \
    -drive "if=pflash,format=raw,file=$W/vars.fd" \
    -drive "if=none,id=hd,format=raw,file=$W/disk.raw" -device "ide-hd,drive=hd,bus=ide.0,bootindex=1" \
    -device qemu-xhci ${extra[@]+"${extra[@]}"} \
    -nic "user,model=e1000e,hostfwd=tcp:127.0.0.1:$SSH_PORT-:22" \
    -display none -vga std \
    -monitor "unix:$W/mon.sock,server,nowait" \
    -daemonize -pidfile "$W/qemu.pid"
  say "VM started from the $1"
}

vm_wait_off() {  # seconds
  local i
  for i in $(seq 1 "$1"); do vm_running || return 0; sleep 1; done
  kill "$(cat "$W/qemu.pid")" 2>/dev/null || true
  die "the VM did not power off within $1 s"
}

vm_screenshot() {  # name
  echo "screendump $W/$1.png -f png" | nc -U "$W/mon.sock" >/dev/null 2>&1 || true
  sleep 1
}

# Types a string on the VM's console through the QEMU monitor.
vm_type() {
  local s="$1" i ch key
  {
    for ((i = 0; i < ${#s}; i++)); do
      ch="${s:i:1}"
      case "$ch" in
        [a-z0-9]) key="$ch" ;;
        [A-Z]) key="shift-$(tr 'A-Z' 'a-z' <<<"$ch")" ;;
        ' ') key=spc ;;  '-') key=minus ;;  '_') key=shift-minus ;;
        '.') key=dot ;;  '/') key=slash ;;  ':') key=shift-semicolon ;;
        '>') key=shift-dot ;;  $'\n') key=ret ;;
        *) echo "vm_type: no key for '$ch'" >&2; continue ;;
      esac
      echo "sendkey $key"
      sleep 0.12
    done
  } | nc -U "$W/mon.sock" >/dev/null
}

# Waits for the console to come up, then types a command that fetches the test key.
vm_ssh_in() {  # seconds to wait before the first try
  local i
  say "waiting $1 s for the VM to boot"
  sleep "$1"
  for i in $(seq 1 30); do
    vm_running || die "the VM exited while booting"
    vm_type $'curl -s 10.0.2.2:'"$HTTP_PORT"$'/k>>.ssh/authorized_keys\n'
    sleep 12
    if vssh true 2>/dev/null; then say "SSH is up"; return 0; fi
  done
  vm_screenshot boot-failed
  die "no SSH after boot; screenshot: $W/boot-failed.png"
}

http_start() {
  mkdir -p "$W/www"; cp "$KEY.pub" "$W/www/k"
  python3 -m http.server "$HTTP_PORT" --bind 127.0.0.1 --directory "$W/www" >/dev/null 2>&1 &
  echo $! > "$W/http.pid"
}
http_stop() { [ -f "$W/http.pid" ] && kill "$(cat "$W/http.pid")" 2>/dev/null || true; rm -f "$W/http.pid"; }

# ------------------------------------------------------------------------------------
prepare() {
  [ -f "$ISO" ] || die "no installer image found; build it with linux/iso/build.sh"
  vm_running && die "a test VM is still running (pid $(cat "$W/qemu.pid"))"
  rm -rf "$W"; mkdir -p "$W"
  ssh-keygen -q -t ed25519 -N "" -f "$KEY" -C imac-vmtest
  cp "$QEMU_SHARE/edk2-i386-vars.fd" "$W/vars.fd"

  say "building the virtual iMac disk"
  qemu-img create -q -f raw "$W/disk.raw" 32G
  local dev
  dev="$(attach "$W/disk.raw")"
  diskutil partitionDisk "$dev" GPT JHFS+ VMTESTMAC 4g "MS-DOS FAT32" LINUXBOOT 1g "MS-DOS FAT32" LINUX R >/dev/null
  echo "this stands in for the user's macOS data" > /Volumes/VMTESTMAC/canary.txt
  dd if=/dev/urandom of=/Volumes/VMTESTMAC/random.bin bs=1m count=32 2>/dev/null
  diskutil list "$dev"
  hdiutil detach "$dev" >/dev/null
  part_hashes > "$W/hashes.before"

  say "writing the virtual USB stick (same method as tools/make-usb.sh)"
  qemu-img create -q -f raw "$W/stick.raw" "$STICK_BYTES"
  dev="$(attach "$W/stick.raw")"
  diskutil partitionDisk "$dev" GPT "MS-DOS FAT32" IMACINST R >/dev/null
  bsdtar -x -f "$ISO" -C /Volumes/IMACINST
  df -h /Volumes/IMACINST | tail -1
  hdiutil detach "$dev" >/dev/null
  say "prepared in $W"
}

install() {
  trap 'http_stop' EXIT
  http_start
  vm_start stick
  vm_ssh_in 75
  say "--- installer system"
  vssh 'cat /etc/hostname; lsblk -o NAME,SIZE,FSTYPE,LABEL,TRAN,PARTTYPENAME'

  # INSTALLER_OVERRIDE=<file> tests a changed imac-install without rebuilding the image.
  if [ -n "${INSTALLER_OVERRIDE:-}" ]; then
    say "--- using imac-install from $INSTALLER_OVERRIDE"
    vssh 'cat > /usr/local/bin/imac-install && chmod 755 /usr/local/bin/imac-install' < "$INSTALLER_OVERRIDE"
  fi

  say "--- imac-install must stop when the answer is not INSTALL"
  local before after
  before="$(vssh 'blkid -o value -s UUID "$(blkid -L LINUX)"')"
  if vssh 'printf "yes\n" | imac-install' > "$W/refuse.log" 2>&1; then
    cat "$W/refuse.log"; die "imac-install went ahead without INSTALL"
  fi
  tail -2 "$W/refuse.log"
  after="$(vssh 'blkid -o value -s UUID "$(blkid -L LINUX)"')"
  [ -n "$before" ] && [ "$before" = "$after" ] || die "LINUX was changed although the install was refused"

  say "--- installing (slow under emulation)"
  vssh 'printf "INSTALL\n" | imac-install' 2>&1 | tr '\r' '\n' | grep -v '^ *[0-9,]* *[0-9]*%' | tee "$W/install.log"
  grep -q "Linux is installed" "$W/install.log" || die "imac-install did not finish"
  say "--- partition types after the install"
  vssh 'lsblk -o NAME,SIZE,FSTYPE,LABEL,PARTTYPENAME /dev/sda' | tee "$W/types.log"
  grep -q "LINUXBOOT .*EFI System" "$W/types.log" || die "LINUXBOOT is not typed EFI System"
  grep -q "LINUX .*Linux filesystem" "$W/types.log" || die "LINUX is not typed Linux filesystem"
  vssh poweroff || true
  vm_wait_off 180
  say "install phase done"
}

boot() {
  trap 'http_stop' EXIT
  http_start
  vm_start disk
  vm_ssh_in 90
  say "--- installed system"
  vssh '
    echo "hostname:   $(cat /etc/hostname)"
    echo "kernel:     $(uname -r)"
    echo "cmdline:    $(cat /proc/cmdline)"
    echo "root:       $(findmnt -no SOURCE,FSTYPE /)"
    echo "boot:       $(findmnt -no SOURCE,FSTYPE /boot)"
    echo "state:      $(systemctl is-system-running)"
    echo "failed units:"; systemctl --failed --no-legend --plain | sed "s/^/  /"
    echo "claude:     $(/usr/local/bin/claude --version 2>&1)"
    echo "repo:       $(git -C /root/imac-display log --oneline 2>&1 | head -1)"
    echo "repo docs:  $(ls /root/imac-display/docs | tr "\n" " ")"
    echo "sshd:       $(/usr/bin/sshd -T 2>/dev/null | grep -E "^(passwordauthentication|permitrootlogin)" | tr "\n" " ")"
    echo "pacman key: $(pacman-key --list-keys 2>/dev/null | grep -c "^pub") public keys"
    echo "live leftovers: $(ls /etc/mkinitcpio.conf.d/archiso.conf /usr/local/bin/imac-install /root/.zlogin 2>/dev/null | tr "\n" " ")"
    echo "boot entries: $(ls /boot/loader/entries | tr "\n" " ")"
  ' | tee "$W/boot.log"
  grep -q "^hostname:   imac-display" "$W/boot.log" || die "wrong hostname on the installed system"
  grep -q "^root: .*ext4" "$W/boot.log" || die "root is not on ext4"
  grep -q "^claude:     [0-9]" "$W/boot.log" || die "claude does not run on the installed system"
  grep -q "^repo:       [0-9a-f]\{7\}" "$W/boot.log" || die "project repo missing on the installed system"
  vssh poweroff || true
  vm_wait_off 180
  say "boot phase done"
}

verify() {
  part_hashes > "$W/hashes.after"
  if diff "$W/hashes.before" "$W/hashes.after" >/dev/null; then
    say "the EFI and stand-in macOS partitions are byte-for-byte unchanged"
  else
    diff "$W/hashes.before" "$W/hashes.after" || true
    die "a partition outside LINUX/LINUXBOOT was modified"
  fi
  local dev
  dev="$(attach "$W/disk.raw" -readonly)"
  diskutil list "$dev"
  hdiutil detach "$dev" >/dev/null
}

case "${1:-}" in
  prepare|install|boot|verify) "$1" ;;
  all) prepare; install; boot; verify; say "ALL CHECKS PASSED" ;;
  *) echo "usage: $0 prepare|install|boot|verify|all [image.iso]" >&2; exit 1 ;;
esac

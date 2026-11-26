# Restart in Display Mode (for macOS on the iMac)

A small app for the **iMac's own macOS**. Launch it and the iMac restarts in display mode: the Linux that makes it a display for the MacBook. Display mode starts **once**: restart the iMac again (the Restart button on its waiting screen) and macOS is back, because macOS stays the default system.

The app itself says "display mode" everywhere, not "Linux". Its file names and bundle identifier still say `restart-in-linux`; an installed helper and macOS's permission for the app are tied to them.

It replaces "restart, hold Option, choose EFI Boot". It is not needed on the MacBook and does nothing there.

## Use

1. Copy `Restart in Display Mode.app` to the iMac (a USB stick, AirDrop, or the download from GitHub) and put it in Applications.
2. First start: the app is not signed by an Apple developer, so right-click it and choose **Open** (on macOS 15 or later: System Settings → Privacy & Security → **Open Anyway**).
3. It asks, with three buttons:

| Button | What happens |
|---|---|
| **Restart** | macOS asks for an administrator password, then the iMac restarts in display mode. The same question next time. |
| **Restart, Never Ask Again** | The same, and a small helper is installed. From then on, launching the app restarts the iMac straight away: no question, no password. |
| **Cancel** | Nothing. |

Apps with unsaved documents can still stop the restart, as with Apple menu → Restart. Display mode then starts at the next restart, whenever that is.

The first time, macOS may also ask whether the app may control "System Events"; that is the restart request. If it is refused, the app tells you to restart from the Apple menu yourself.

## What it changes

- Each launch: one firmware setting, "start from the `LINUXBOOT` partition at the next start only" (`bless --setBoot --nextonly`). The default startup disk is not changed.
- Only with "Never Ask Again", two files:
  - `/Library/PrivilegedHelperTools/local.imac-display.restart-in-linux`: a root-owned copy of `restart-in-linux-helper`
  - `/etc/sudoers.d/imac-display-restart-in-linux`: lets your user run that one script, without arguments, without a password

To go back to being asked, delete both:

    sudo rm /Library/PrivilegedHelperTools/local.imac-display.restart-in-linux /etc/sudoers.d/imac-display-restart-in-linux

The helper takes no paths from its caller. It looks for a FAT partition labelled `LINUXBOOT` (an internal one first) that holds `EFI/BOOT/BOOTX64.EFI` and systemd-boot's `loader/loader.conf`, which is what `imac-install` creates, and refuses anything else.

## Requirements

- An Intel Mac with Linux installed by `imac-install`.
- macOS 10.11 or later. The app is two shell scripts, nothing compiled.
- **System Integrity Protection:** with it switched on, macOS may refuse to set a startup system that is not Apple's or Windows; the app then shows macOS's message. On the project's iMac it is switched off (firmware value `csr-active-config` = `0x0fff`, read from Linux on 2026-11-26), so this is not expected there.

## Build and test

    imac-macos/build.sh    # out/build.noindex/Restart in Display Mode.app and out/Restart in Display Mode.zip
    imac-macos/test.sh     # dry run against a disk image that looks like the boot partition

**State (2026-11-26):** the tests pass on the MacBook (finding the partition, refusing a partition without Linux, the command it would run, the bundle starting from Finder's launcher). **Not yet run on the iMac:** the real `bless`, the password dialog, the restart itself, and whether the firmware then starts Linux. Log: `~/Library/Logs/Restart in Display Mode.log`.

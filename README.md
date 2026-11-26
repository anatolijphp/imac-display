# iMac Display

Use an **iMac Retina 5K, 27-inch, Late 2015** (iMac17,1) as a display, speakers, keyboard/mouse dock and USB hub for a MacBook, over one Thunderbolt cable.

That iMac has no Target Display Mode and no video input, so this is done in software:

- the **iMac** dual-boots a minimal Linux that only runs a receiver (macOS stays on its drive, untouched);
- the **MacBook** runs one app, **iMac Display**, which adds an extra display and sends picture and sound to the iMac.

| | What you get |
|---|---|
| Display | An extra display on the MacBook; its resolution is set in macOS's Display settings like any monitor |
| Sound | "iMac Speakers" appears as a sound output while connected, and the Mac's sound moves to it |
| Keyboard and mouse | The ones plugged into the iMac control the MacBook |
| USB drives | Drives plugged into the iMac show up in Finder on the MacBook |

Status: works over Wi-Fi for testing. The Thunderbolt link, which is the intended one, has not been tested yet. See `docs/` for measurements and open points.

## Get the builds

Every push to `main` builds both halves with GitHub Actions (**Actions → Build → latest run → Artifacts**); tags like `v0.4` also get a release with the same two files.

| File | For | What it is |
|---|---|---|
| `imac-display-installer-<date>-x86_64.iso` | the iMac | Bootable installer: Arch Linux plus the receiver, installed in one go, no internet needed |
| `iMac Display.dmg` | the MacBook | The app, with its sound device inside |
| `Restart in Linux.zip` | the iMac's macOS (optional) | A small app: launch it in macOS on the iMac and the iMac restarts into Linux once, instead of holding Option at power-on. See [`imac-macos/README.md`](imac-macos/README.md) |

## Install

**iMac** (full guide: [`docs/INSTALL.md`](docs/INSTALL.md))

1. In macOS on the iMac, make a backup and add two partitions in Disk Utility: `LINUXBOOT` (1 GB) and `LINUX` (20 GB or more), both "MS-DOS (FAT)".
2. Write the `.iso` to a USB stick (2 GB or more): `tools/make-usb.sh diskN --erase diskN image.iso` on a Mac, or any tool that writes disk images.
3. Start the iMac holding **Option**, choose **EFI Boot**, type `imac-install`, then `INSTALL` to confirm. It only touches the two partitions named above.
4. Restart holding **Option** → **EFI Boot**. The iMac shows "Waiting for your Mac".
5. Optional: put **Restart in Linux** in the iMac's macOS Applications folder. Launching it does step 4 for you; restarting from Linux brings macOS back.

**MacBook**

1. Open the `.dmg` and drag **iMac Display** to Applications.
2. The builds are not notarized by Apple, so the first start needs: right-click the app → **Open**, or System Settings → Privacy & Security → **Open Anyway**.
3. Allow what it asks for (Screen Recording, Accessibility, and Microphone for its own sound device) and let it install the "iMac Speakers" sound device.
4. Connect the Thunderbolt cable (MacBook → Apple Thunderbolt 3-to-2 adapter → Thunderbolt 2 cable → iMac).

## Build it yourself

    linux/iso/build.sh          # the installer image; needs Docker (macOS or Linux)
    mac/scripts/build-app.sh    # the app; needs a Mac with the Command Line Tools
    imac-macos/build.sh         # "Restart in Linux" for the iMac's macOS; needs a Mac

Options are described at the top of each script.

## Layout

| Path | What |
|---|---|
| `mac/` | The MacBook app, its sound driver, and a development test tool |
| `receiver/` | `imacdisplayd`, the program the iMac runs |
| `imac-macos/` | "Restart in Linux", a small app for the iMac's own macOS |
| `linux/` | The iMac's system: installer image definition (`linux/iso/`) and the receiver's system files (`linux/rootfs/`) |
| `PROTOCOL.md` | The wire format between the two |
| `docs/` | Install guide, hardware measurements, notes between the two development sessions |
| `tools/` | Writing the USB stick, testing the installer in a virtual machine |

The app uses a private macOS API for the extra display (`CGVirtualDisplay`); an macOS update can break it.

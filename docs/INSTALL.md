# Installing Linux on the iMac

For the person doing the install. It puts Arch Linux, Claude Code and this project on the iMac's internal drive next to macOS. macOS stays the default system; Linux starts only when you hold Option at power-on.

You need the installer USB stick (made on the MacBook with `tools/make-usb.sh`) and a wired USB keyboard for the iMac. A Magic Keyboard connected by its Lightning cable works.

## 1. In macOS on the iMac: back up and make room

Only macOS can safely shrink its own disk, so this part is done by hand.

1. Make a Time Machine backup.
2. Open **Disk Utility**. In the menu choose **View > Show All Devices**.
3. Select the internal drive (the top entry in the sidebar), click **Partition**, then **+**, then **Add Partition**.
4. Create two partitions, both with format **MS-DOS (FAT)**:

   | Name | Size |
   |---|---|
   | `LINUXBOOT` | 1 GB |
   | `LINUX` | 60 GB or more (20 GB is the minimum) |

5. Click **Apply** and wait until it finishes.

The names matter: the installer looks for exactly `LINUXBOOT` and `LINUX` and writes to nothing else. On a Fusion Drive the partitions land on the hard-disk part, which is fine.

## 2. Start the installer

1. Shut the iMac down. Plug in the USB stick and the wired keyboard.
2. Switch it on while holding **Option**. Choose **EFI Boot**.
3. A text menu appears; wait or press Enter. If the screen then stays black, restart and choose the "safe graphics" entry.
4. At the prompt type:

   ```
   imac-install
   ```

5. It shows the drive layout and the two partitions it will erase. Check them, then type `INSTALL`.
6. When it says "Linux is installed", type `poweroff` and pull out the stick.

No internet is needed for the install.

## 3. First start of Linux

1. Switch the iMac on while holding **Option** and choose **EFI Boot**.
2. The iMac shows **"Waiting for your Mac"**. That is the finished system: nothing else needs installing.
3. On the MacBook, start **iMac Display** and connect the Thunderbolt cable.

For a text console on the iMac (to join Wi-Fi with `iwctl station wlan0 connect "NETWORK NAME"`, or to look at logs): after choosing **EFI Boot**, a menu appears for 3 seconds; choose **"console only (receiver off)"**. With an SSH key built into the installer (`PUBKEY`, see `linux/iso/build.sh`), `ssh root@imac-display.local` also works once Wi-Fi is joined.

Developing on the iMac with Claude Code: build the installer with `WITH_CLAUDE=1`, then `cd /root/imac-display && claude` and `follow docs/IMAC-BUILD.md`.

## Going back

- **To macOS:** restart without holding Option.
- **Removing Linux:** in Disk Utility, select each of the two partitions and click **-**. macOS gets the space back.

## If something goes wrong

| What you see | What to do |
|---|---|
| The stick is not offered in the Option menu | On the MacBook, write the image raw instead: see the `dd` line at the top of `tools/make-usb.sh`. |
| "no partition named LINUX found" | Step 1 was not done, or the names differ. Check them in Disk Utility. |
| The installer stops with FAILED | macOS was not touched. The message names the cause; the log is `/tmp/imac-install.log`. Run `imac-install` again after fixing it. |
| macOS shows "The disk you attached was not readable" | Click **Ignore**. Never click Initialise. Tell the iMac's Claude; it means the partition type label was not set. |
| Wi-Fi interface is not called `wlan0` | `iwctl station list` shows the real name. |

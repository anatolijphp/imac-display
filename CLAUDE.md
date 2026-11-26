# iMac Display

Turn an iMac Retina 5K, 27-inch, Late 2015 (iMac17,1) into a display, USB hub and sound device for a MacBook over one Thunderbolt cable. The iMac has no Target Display Mode and no video input, so this is done in software: the iMac dual-boots a minimal Linux that only runs a receiver, and the MacBook runs one custom app that streams to it.

## Which machine am I on?

- **On the MacBook** (macOS, Apple Silicon): since 2026-11-27 this is the only session, and it owns everything. It works on the iMac itself over SSH (`ssh root@imac-display.local`, the iMac's copy of this repo is `/root/imac-display`): edits here, pushes to the iMac's `macbook` branch, merges there (`git merge --ff-only macbook`), builds the receiver there (`receiver/README.md`) and installs it. The user decided this: no second session, no relaying, no handoff notes.
- **On the iMac** (Linux, hostname `imac-display`): there is no separate session any more. `docs/IMAC-BUILD.md` and `docs/handoff.md` are history from when there was one.

## Decisions already made (do not reopen without the user)

- **Link: Thunderbolt only.** One cable: MacBook USB-C port → Apple Thunderbolt 3–to–2 adapter → Thunderbolt 2 cable → iMac. No Ethernet or Wi-Fi fallback for the display link. Wi-Fi on the iMac is for setup and package downloads only. One exception, asked for by the user: a Thunderbolt / Wi-Fi switch on the iMac's waiting screen, for testing over Wi-Fi; Thunderbolt stays the default.
- **Transport: Thunderbolt networking**, not PCIe. Two computers cannot open a PCIe tunnel to each other; Thunderbolt's host-to-host mode gives an IP link (`thunderbolt0` on Linux, `bridge0` "Thunderbolt Bridge" on macOS, link-local addresses).
- **iMac: dual boot.** macOS stays on the internal drive and stays the default system. Linux lives in two partitions the user made in Disk Utility, `LINUX` and `LINUXBOOT`, and is installed from a USB stick with `imac-install` (`docs/INSTALL.md`).
- **Linux: Arch, minimal, no desktop.** Kernel pinned once the 5K panel works.
- **MacBook: exactly one custom app**, `iMac Display.app`. No third-party helper apps, no separate installers: anything the app needs (the "iMac Speakers" sound device) is inside it and installed by it.
- **Sound:** MacBook output → iMac speakers and 3.5 mm jack; iMac microphone → MacBook input.
- **USB:** keyboard and mouse (forwarded as input events), flash drives and disks (shared as network volumes over the cable), printers and scanners (AirPrint/AirScan re-publishing). Arbitrary USB pass-through is gated on an Apple-granted entitlement and is the last milestone.

## Hardware

| | iMac | MacBook |
|---|---|---|
| Model | iMac17,1, Intel Skylake, Radeon R9 M380 (details: `docs/hardware-validation.md`) | MacBook Pro (Mac17,2), Apple M5, macOS 27.2 |
| Thunderbolt | 2 × Thunderbolt 2 (Falcon Ridge) | 3 × Thunderbolt/USB4 |
| Panel | 5120×2880, driven internally as two DisplayPort tiles | |

## Who owns what

| Path | Owner |
|---|---|
| Everything | the one session on the MacBook |
| `docs/handoff.md` | closed; kept as the record of the two-session time |

`PROTOCOL.md` is the contract between the two halves (the MacBook app and `imacdisplayd`).

Working on the running iMac: the user is usually looking at that screen. Stopping `imacdisplayd` drops their picture, so do it rarely, say so, and put it back. Before rebuilding the boot image (`mkinitcpio -P`), keep the previous one (`/boot/initramfs-linux-prev.img`). **Never restart the iMac with `kexec`:** the graphics chip does not initialise after it (`amdgpu ... Fatal error during GPU init`, tried 2026-11-27), the screen stays black and only a normal restart, which lands in macOS, brings it back.

## Working rules

- Small commits with clear messages. Pull before starting work. The shared remote is GitHub (`origin`); a push to `main` there starts the build pipeline (`.github/workflows/build.yml`: installer image and app).
- Write down what you measured, not what you expect. Unverified claims are marked as such.
- Third-party code (kernel patches, driver forks) is read before it is built or loaded.
- **On the iMac's internal drive, only the `LINUX` and `LINUXBOOT` partitions are ours.** Nothing else on that drive is written to, resized or reformatted, and the firmware's boot settings are left alone. One exception, asked for by the user: `Restart in Display Mode.app` (`imac-macos/`), which the user launches in the iMac's macOS, sets "start Linux at the next start only". The default system stays macOS.

## Milestones

| | Side | What |
|---|---|---|
| M0 | iMac | Hardware gate: Thunderbolt networking, 5K panel, video decode, sound. Go or no-go. |
| M1 | MacBook | Spikes: virtual display, screen capture, low-latency encode, build and sign without Xcode. |
| M2 | iMac | Turn the installed system into an appliance: kernel pin, 5K patch, boot straight into the receiver. |
| M3 | Both | Video end to end: cable in → display appears. |
| M4 | Both | Sound out and microphone. |
| M5 | Both | Keyboard and mouse. |
| M6 | Both | Flash drives and disks. |
| M7 | Both | Native 5K (4 tiles) and polish. |
| M8 | iMac | Printers and scanners. |
| M9 | Both | Any other USB device (needs Apple's entitlement). |

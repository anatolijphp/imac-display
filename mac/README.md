# Mac side

The MacBook half of `PROTOCOL.md`. For now a library and a command-line test tool; the menu-bar app comes in M3. Results of the first measurements: `docs/mac-spikes.md`.

| Path | What |
|---|---|
| `Sources/CVirtualDisplay/` | Declarations for Apple's private virtual-display classes |
| `Sources/IMDKit/` | `Wire` (frame format), `Channel` and `Session` (the two TCP connections), `VirtualDisplay`, `DisplayCapture` (screen and sound), `H264Encoder`, `TileSplitter`, `InputInjector` (the iMac's keyboard and mouse), `StorageMounter` (the iMac's USB drives) |
| `Sources/imd/` | `imd`: `vdisplay`, `pipeline`, `hello` (run it without arguments for the options) |
| `AudioDriver/` | The "iMac Speakers" audio driver (a Core Audio server plug-in in C) and its self-test |
| `Sources/IMDKit/SpeakerDevice.swift` | The app's end of that driver: records what was played into "iMac Speakers", follows its volume and mute |
| `scripts/build-audio-driver.sh`, `scripts/install-audio-driver.sh` | Build, sign and self-test the driver; install or remove it (asks for an administrator password) |
| `scripts/build-app.sh` | Builds and signs `out/iMac Display.app` |
| `test/fake_imac.py` | A stand-in for the iMac's receiver |

Build: `swift build -c release` (Command Line Tools are enough).

Permissions, for the terminal the tool runs in (or for the app, when run as an app), both under System Settings > Privacy & Security:

- **Screen & System Audio Recording**: screen capture and sound.
- **Accessibility**: so the iMac's keyboard and mouse can control this Mac. Without it they do nothing.
- **Microphone**: only with the audio driver installed. macOS asks because the app records from the driver's device; it never opens a real microphone for this.

## Sound: "iMac Speakers"

    scripts/build-audio-driver.sh
    scripts/install-audio-driver.sh          # administrator password; `remove` uninstalls

The driver adds an output device called **iMac Speakers**. Pick it in the sound menu (or per app, where an app lets you choose) and that sound goes to the iMac; everything else keeps playing on the MacBook. The volume keys then change the iMac's volume. The driver also adds a hidden device, "iMac Speakers (tap)", which only the app uses.

Without the driver, `imd pipeline` falls back to copying everything the Mac plays (`--audio-source screen`), with no way to choose.

Known limits: the device is in the list even when no iMac is connected (sound sent to it then goes nowhere), and there is no "iMac Microphone" device yet.

Testing against the real iMac over Wi-Fi (testing only, the product uses the Thunderbolt cable): start the receiver there with `--iface wlan0`, then

    .build/release/imd hello --host imac-display.local
    .build/release/imd pipeline --host imac-display.local --lodpi --points 1920x1080 --mbps 10 --capture-fps 30 --no-scene --quiet --seconds 0

That last one is a usable session: an extra 1920×1080 display, the iMac's keyboard, mouse and USB drives, and sound once the iMac can play it. It runs until Ctrl-C. (30 pictures per second until the receiver's problems at 60 are fixed, see `docs/handoff.md`.)

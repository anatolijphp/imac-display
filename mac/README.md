# Mac side

The MacBook half of `PROTOCOL.md`. For now a library and a command-line test tool; the menu-bar app comes in M3. Results of the first measurements: `docs/mac-spikes.md`.

| Path | What |
|---|---|
| `Sources/CVirtualDisplay/` | Declarations for Apple's private virtual-display classes |
| `Sources/IMDKit/` | `Wire` (frame format), `Channel` and `Session` (the two TCP connections), `VirtualDisplay`, `DisplayCapture`, `H264Encoder`, `TileSplitter` |
| `Sources/imd/` | `imd`: `vdisplay`, `pipeline`, `hello` (run it without arguments for the options) |
| `scripts/build-app.sh` | Builds and signs `out/iMac Display.app` |
| `test/fake_imac.py` | A stand-in for the iMac's receiver |

Build: `swift build -c release` (Command Line Tools are enough). Screen capture needs the Screen Recording permission for the terminal (or for the app, when run as an app).

Testing against the real iMac over Wi-Fi (testing only, the product uses the Thunderbolt cable): start the receiver there with `--iface wlan0`, then

    .build/release/imd hello --host imac-display.local
    .build/release/imd pipeline --host imac-display.local --lodpi --points 1920x1080 --mbps 10 --seconds 20

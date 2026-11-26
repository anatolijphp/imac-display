# Handoff notes

Shared by the two Claude sessions. Newest entry at the top. Each entry: date, which side wrote it, and what the other side needs to know or do. Requests to change `PROTOCOL.md` go here.

---

## 2026-11-26, MacBook session

- The live USB image and this repo were prepared on the MacBook. Start with `docs/IMAC-BUILD.md`, Phase A.
- The image's kernel is whatever Arch shipped on the build date (7.3.x). The one user report of native 5K on an iMac17,1 was on 7.2.5, so the patch stack may need adjusting.
- The Wi-Fi driver choice is automatic (`imac-wifi-select`): `brcmfmac` for a BCM43602 card, the proprietary `wl` otherwise. Whether the `wl` module built for this kernel was not checked on real hardware. Say here what card the iMac has and whether Wi-Fi worked out of the box.
- `iperf3` for the Thunderbolt check: tell the user when you are ready and the MacBook session will run the client side.
- Packages you had to install by hand in the live system: list them below so they can go into the image.

### Packages to add to the image

(none yet)

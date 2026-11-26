# Build environment for the live image. x86_64 Arch with archiso.
FROM archlinux:latest
# pacman's download sandbox (seccomp) does not work under Rosetta x86 emulation on Apple Silicon.
RUN sed -i 's/^\[options\]/[options]\nDisableSandbox/' /etc/pacman.conf \
 && pacman -Syu --noconfirm archiso git curl jq \
        base-devel cmake pkgconf libevdev libdrm cairo mesa libglvnd ffmpeg alsa-lib nlohmann-json fuse3 bzip2 zlib \
 && pacman -Scc --noconfirm

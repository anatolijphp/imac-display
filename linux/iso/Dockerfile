# Build environment for the live image. x86_64 Arch with archiso.
FROM archlinux:latest
# pacman's download sandbox (seccomp) does not work under Rosetta x86 emulation on Apple Silicon.
RUN sed -i 's/^\[options\]/[options]\nDisableSandbox/' /etc/pacman.conf \
 && pacman -Syu --noconfirm archiso git curl jq \
 && pacman -Scc --noconfirm

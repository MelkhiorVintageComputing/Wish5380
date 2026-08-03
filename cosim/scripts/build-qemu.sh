#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Configure and build only what the co-simulation needs: one target, no GUI,
# no extras.  Re-running is cheap - ninja rebuilds what changed, which is what
# makes iterating on the card practical.
source "$(dirname "$0")/common.sh"

[ -d "$QEMU_SRC" ] || die "run fetch-qemu.sh first"

if [ ! -f "$QEMU_BUILD/build.ninja" ]; then
    say "configuring"
    mkdir -p "$QEMU_BUILD"
    (cd "$QEMU_BUILD" && "$QEMU_SRC/configure" \
        --prefix="$QEMU_PREFIX" \
        --target-list=i386-softmmu \
        --disable-docs --disable-guest-agent --disable-sdl --disable-gtk \
        --disable-vnc --disable-spice --disable-tools --disable-werror \
        --disable-tpm --disable-libssh --disable-vde --disable-curl \
        --disable-slirp-smbd --without-default-features \
        --enable-system --enable-fdt=internal)
fi

say "building"
ninja -C "$QEMU_BUILD"
say "installing into $QEMU_PREFIX"
ninja -C "$QEMU_BUILD" install >/dev/null
say "$(qemu_bin) --version"
"$(qemu_bin)" --version | head -1

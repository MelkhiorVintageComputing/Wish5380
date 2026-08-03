#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Build the guest: an i386 Linux with the NCR 5380 driver in it, and a card
# image with a filesystem the guest can mount.
#
# The kernel is i386 because CONFIG_ISA exists only there - arch/x86/Kconfig
# puts `config ISA` inside `if X86_32` - so an ISA card cannot be probed by an
# x86-64 guest at all.
#
# Nothing about the driver is modified.  What is chosen is the configuration:
# CONFIG_SCSI_GENERIC_NCR5380 built in rather than a module, because there is
# no initramfs and anything needed to reach the root filesystem has to be in
# the kernel.
source "$(dirname "$0")/common.sh"

tarball="$DOWNLOADS/$LINUX_TARBALL"
if [ ! -f "$tarball" ]; then
    say "downloading $LINUX_TARBALL"
    curl -fSL --progress-bar -o "$tarball.part" "$LINUX_URL"
    mv "$tarball.part" "$tarball"
fi

if [ ! -d "$LINUX_SRC" ]; then
    say "unpacking the kernel"
    mkdir -p "$LINUX_SRC"
    tar -xf "$tarball" -C "$LINUX_SRC" --strip-components=1
fi

mkdir -p "$LINUX_BUILD"
if [ ! -f "$LINUX_BUILD/.config" ]; then
    say "configuring"
    make -C "$LINUX_SRC" O="$LINUX_BUILD" ARCH=i386 i386_defconfig >/dev/null
    "$LINUX_SRC/scripts/config" --file "$LINUX_BUILD/.config" \
        --enable ISA \
        --enable SCSI \
        --enable BLK_DEV_SD \
        --enable SCSI_LOWLEVEL \
        --enable SCSI_GENERIC_NCR5380 \
        --enable EXT2_FS \
        --enable SERIAL_8250 \
        --enable SERIAL_8250_CONSOLE \
        --disable RANDOMIZE_BASE
    make -C "$LINUX_SRC" O="$LINUX_BUILD" ARCH=i386 olddefconfig >/dev/null
    grep -q "^CONFIG_SCSI_GENERIC_NCR5380=y" "$LINUX_BUILD/.config" ||
        die "the driver did not survive olddefconfig; check CONFIG_ISA"
    echo "  CONFIG_SCSI_GENERIC_NCR5380=y"
fi

say "building the kernel (this is the long part)"
make -C "$LINUX_SRC" O="$LINUX_BUILD" ARCH=i386 -j"$(nproc)" bzImage >/dev/null
[ -f "$KERNEL" ] || die "no bzImage"
say "kernel: $KERNEL"

say "building the guest's init"
root="$WORK/guest-root"
rm -rf "$root"
mkdir -p "$root/dev" "$root/proc" "$root/sys"
gcc -m32 -static -O2 -o "$root/init" "$COSIM_DIR/guest/init.c"

say "making the card image"
printf 'the host put this here before the guest booted\n' > "$root/hello.txt"
rm -f "$CARD_IMAGE"
# One filesystem filling the whole card, and no partition table: the guest
# mounts /dev/sda itself, so there is one less thing between the RTL and the
# verdict.  The block size is 1024 because the card is small.
mke2fs -q -t ext2 -b 1024 -d "$root" -F "$CARD_IMAGE" "$((CARD_BLOCKS / 2))k"
say "card image: $CARD_IMAGE, $((CARD_BLOCKS * 512)) bytes"

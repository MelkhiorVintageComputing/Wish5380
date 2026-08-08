# SPDX-License-Identifier: MIT
# Shared settings for the co-simulation scripts.  Sourced, not run.

set -euo pipefail

COSIM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT_DIR="$(cd "$COSIM_DIR/.." && pwd)"

# Everything downloaded, built or generated lands here.  None of it is in git:
# these scripts are what put it back.
WORK="${WORK:-$ROOT_DIR/work}"
DOWNLOADS="$WORK/downloads"

# One QEMU, two machines.  The ISA card and the Sun-3 board model the same
# part behind different glue and share the library that carries it, so they
# are built from one tree by build-sun3-qemu.sh - which is why there is no
# fetch-qemu.sh and no release tarball here any more.
QEMU_SRC="$WORK/sun3-qemu"
QEMU_BUILD="$QEMU_SRC/build"

LINUX_SRC="$WORK/linux-src"
LINUX_BUILD="$WORK/linux-build"
IMAGES="$WORK/images"
LIBDIR="$WORK/lib"

# A longterm kernel, built for i386 because CONFIG_ISA exists only there:
# arch/x86/Kconfig puts `config ISA` inside `if X86_32`, so an ISA card cannot
# be probed by an x86-64 guest at all.  That is why this is not the host's own
# kernel.
LINUX_VERSION="6.6.63"
LINUX_TARBALL="linux-$LINUX_VERSION.tar.xz"
LINUX_URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/$LINUX_TARBALL"

KERNEL="$LINUX_BUILD/arch/x86/boot/bzImage"
CARD_IMAGE="$IMAGES/card.img"
CARD_BLOCKS="${CARD_BLOCKS:-4096}"          # two mebibytes, in 512-byte blocks
RTL_LIB="$LIBDIR/libwish5380rtl.so"

# Where the card sits.  0x350 with IRQ 5 is one of the addresses Linux's
# g_NCR5380 probes by default, and nothing on the PC machine wants either.
CARD_PORT="${CARD_PORT:-0x350}"
CARD_IRQ="${CARD_IRQ:-5}"

qemu_bin() {
    if [ -x "$QEMU_BUILD/qemu-system-i386" ]; then
        echo "$QEMU_BUILD/qemu-system-i386"
    else
        command -v qemu-system-i386
    fi
}

say()  { printf '\033[36m==\033[0m %s\n' "$*"; }
warn() { printf '\033[33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31m!!\033[0m %s\n' "$*" >&2; exit 1; }

mkdir -p "$DOWNLOADS" "$IMAGES"

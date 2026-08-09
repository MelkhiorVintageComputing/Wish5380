#!/bin/bash
# Build the QEMU the co-simulation uses: the Sun-3/60 with its onboard SCSI
# board, and the i386 PC with the ISA card, from one tree.
#
# Two machines, one build.  Both boards model the same NCR 5380 behind
# different glue and share the library that carries it, so a second QEMU to
# hold the second board bought nothing and cost a file that had to exist
# twice.  Hence one clone, one patchset, and two binaries.
#
# The Sun-3 machine model is not ours: it lives in a separate fork, together
# with the PROM images and the conventions for recording changes to it.  This
# script clones that fork's already-patched tree into work/, applies the two
# patch series kept under cosim/patches/, and builds - so nothing in the fork
# is touched and no QEMU sources are kept here.
#
# The series are applied in order and the order is the point.  qemu/ is the
# software 5380 and the two boards that carry it, and stands on its own;
# qemu-rtl/ then teaches both boards to load the Verilated chip instead.  Only
# the first could ever go to qemu-devel, which is why it is a series and not a
# subset.
#
# Usage: cosim/scripts/build-sun3-qemu.sh [-f] [-d] [-s]
#   -f  start over: delete work/sun3-qemu first
#   -d  --enable-debug (much slower to boot, but debuggable)
#   -s  software chip only: apply cosim/patches/qemu/ and stop there, which
#       is how "the first series stands on its own" gets tested rather than
#       asserted.  Needs -f if the tree already has both.
#
# Environment:
#   SUN3_FORK  the fork's checkout (default ~/qemu-sun3)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FORK="${SUN3_FORK:-$HOME/qemu-sun3}"
WORK="$ROOT/work/sun3-qemu"
PATCHES="$ROOT/cosim/patches/qemu"
PATCHES_RTL="$ROOT/cosim/patches/qemu-rtl"

FRESH=0
SW_ONLY=0
DEBUG=()
while getopts "fdsh" opt; do
    case "$opt" in
        f) FRESH=1 ;;
        d) DEBUG=(--enable-debug) ;;
        s) SW_ONLY=1 ;;
        h) sed -n '2,28p' "$0"; exit 0 ;;
        *) exit 1 ;;
    esac
done

if [ ! -d "$FORK/qemu/.git" ]; then
    echo "error: $FORK/qemu is not a git checkout." >&2
    echo "Run '$FORK/scripts/setup-qemu.sh' there first - it recreates the" >&2
    echo "tree from QEMU-BASE and applies the Sun-3 series." >&2
    exit 1
fi

if [ "$FRESH" = 1 ]; then
    rm -rf "$WORK"
fi

if [ ! -d "$WORK/.git" ]; then
    echo "== cloning $FORK/qemu -> $WORK"
    # --no-hardlinks so nothing we do can reach back into the fork's objects
    git clone --no-hardlinks "$FORK/qemu" "$WORK"
    git -C "$WORK" checkout -b scsi

    # One directory at a time, and never one glob over both: a single glob
    # would sort by filename and interleave the two series, which do not
    # commute - qemu-rtl/ rewrites files qemu/ creates.
    if compgen -G "$PATCHES/*.patch" > /dev/null; then
        echo "== applying $(ls "$PATCHES"/*.patch | wc -l) patches (software chip)"
        git -C "$WORK" am "$PATCHES"/*.patch
    else
        echo "== no patches in $PATCHES yet; building the fork as it stands"
    fi

    if [ "$SW_ONLY" = 1 ]; then
        echo "== -s given: stopping before $PATCHES_RTL"
    elif compgen -G "$PATCHES_RTL/*.patch" > /dev/null; then
        echo "== applying $(ls "$PATCHES_RTL"/*.patch | wc -l) patches (Verilated chip)"
        git -C "$WORK" am "$PATCHES_RTL"/*.patch
    fi
fi

if [ ! -f "$WORK/build/config-host.mak" ]; then
    echo "== configuring"
    mkdir -p "$WORK/build"
    (cd "$WORK/build" && ../configure \
        --target-list=m68k-softmmu,i386-softmmu \
        --disable-docs --disable-werror \
        "${DEBUG[@]}")
fi

echo "== building"
(cd "$WORK/build" && ninja qemu-system-m68k qemu-system-i386)

echo
echo "built $WORK/build/qemu-system-m68k  - run with cosim/scripts/run-sun3.py"
echo "built $WORK/build/qemu-system-i386  - run with cosim/scripts/run-cosim.py"

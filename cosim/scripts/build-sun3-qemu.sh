#!/bin/bash
# Build a QEMU with the Sun-3/60 machine and our onboard SCSI board.
#
# The Sun-3 machine model is not ours: it lives in a separate fork, together
# with the PROM images and the conventions for recording changes to it.  This
# script clones that fork's already-patched tree into work/, applies the
# patchset kept in cosim/patches/sun3/, and builds - so nothing in the fork is
# touched and no QEMU sources are kept here.
#
# Usage: cosim/scripts/build-sun3-qemu.sh [-f] [-d]
#   -f  start over: delete work/sun3-qemu first
#   -d  --enable-debug (much slower to boot, but debuggable)
#
# Environment:
#   SUN3_FORK  the fork's checkout (default ~/qemu-sun3)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FORK="${SUN3_FORK:-$HOME/qemu-sun3}"
WORK="$ROOT/work/sun3-qemu"
PATCHES="$ROOT/cosim/patches/sun3"

FRESH=0
DEBUG=()
while getopts "fdh" opt; do
    case "$opt" in
        f) FRESH=1 ;;
        d) DEBUG=(--enable-debug) ;;
        h) sed -n '2,14p' "$0"; exit 0 ;;
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

    if compgen -G "$PATCHES/*.patch" > /dev/null; then
        echo "== applying $(ls "$PATCHES"/*.patch | wc -l) patches"
        git -C "$WORK" am "$PATCHES"/*.patch
    else
        echo "== no patches in $PATCHES yet; building the fork as it stands"
    fi
fi

if [ ! -f "$WORK/build/config-host.mak" ]; then
    echo "== configuring"
    mkdir -p "$WORK/build"
    (cd "$WORK/build" && ../configure \
        --target-list=m68k-softmmu --disable-docs --disable-werror \
        "${DEBUG[@]}")
fi

echo "== building"
(cd "$WORK/build" && ninja qemu-system-m68k)

echo
echo "built $WORK/build/qemu-system-m68k"
echo "run it with: cosim/scripts/run-sun3.py"

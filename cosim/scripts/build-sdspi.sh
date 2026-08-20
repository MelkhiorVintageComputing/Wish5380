#!/bin/bash
# Fetch ZipCPU's sdspi, whose bench/cpp/sdspisim.cpp is an SD card in SPI mode
# written by someone else.
#
# It is the outside opinion the SD side of this design otherwise has none of:
# tb/cpp/sd_card.cpp was written alongside src/blk_sd.sv, from one reading, and
# answers exactly the commands blk_sd sends.  A misreading in both cancels out.
# SDSPISIM was written against a different controller and is a superset - it
# knows CMD1, CMD10, CMD13 and ACMD51 as well as everything we send.
#
# It is GPLv3-or-later, so it is treated exactly as Hatari is: cloned into
# work/, never committed, and nothing in src/ or tb/ may depend on it.  The
# binary that links it is built under work/ and is not distributed.
#
# Usage: cosim/scripts/build-sdspi.sh [-f]
#   -f  start over: delete work/sdspi-src first
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
WORK="${WORK:-$ROOT/work}"
SRC="$WORK/sdspi-src"

# Pinned, because a patch series - or a set of expectations - against a moving
# target is one that stops applying without saying so.
UPSTREAM="https://github.com/ZipCPU/sdspi"
COMMIT="dfb16c8"

FRESH=0
while getopts "fh" opt; do
    case "$opt" in
        f) FRESH=1 ;;
        h) sed -n '2,17p' "$0"; exit 0 ;;
        *) exit 1 ;;
    esac
done

[ "$FRESH" = 1 ] && rm -rf "$SRC"

if [ ! -d "$SRC/.git" ]; then
    echo "== cloning $UPSTREAM -> $SRC"
    mkdir -p "$WORK"
    git clone --quiet "$UPSTREAM" "$SRC"
fi

echo "== checking out $COMMIT"
git -C "$SRC" checkout --quiet --detach "$COMMIT"

# No patches so far, and that is a result rather than an omission: blk_sd
# computes a real CRC7 for every command, so the one thing that would have
# needed patching out - SDSPISIM asserting on a bad command CRC, with no CMD59
# to turn checking off - never fires.  doc/sd.md records how that came about.
PATCHES="$ROOT/cosim/patches/sdspi"
if compgen -G "$PATCHES/*.patch" > /dev/null 2>&1; then
    echo "== applying $(ls "$PATCHES"/*.patch | wc -l) patches"
    git -C "$SRC" apply --whitespace=nowarn "$PATCHES"/*.patch
fi

echo
echo "ready: $SRC/bench/cpp/sdspisim.cpp"
echo "build the harness with  make -C cosim/sdcheck check"

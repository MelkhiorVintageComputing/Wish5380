#!/bin/bash
# Build a Hatari with an Atari TT whose SCSI is a Verilated wish5380, and
# fetch the EmuTOS that drives it.
#
# Neither is ours, so neither is kept here: the sources are cloned and
# downloaded into work/ and the one change to Hatari lives in
# cosim/patches/hatari/.  Hatari is GPL-2+ and EmuTOS is GPL-2+, which is why
# nothing of either enters src/ or tb/.
#
# Usage: cosim/scripts/build-hatari.sh [-f]
#   -f  start over: delete work/hatari-src and work/hatari-build first
#
# Environment:
#   HATARI_REF  the commit or tag to build (default: the tree's default branch)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SRC="$ROOT/work/hatari-src"
BUILD="$ROOT/work/hatari-build"
PATCHES="$ROOT/cosim/patches/hatari"
EMUTOS="$ROOT/work/emutos"
EMUTOS_VER=1.4

FRESH=0
while getopts "fh" opt; do
    case "$opt" in
        f) FRESH=1 ;;
        h) sed -n '2,14p' "$0"; exit 0 ;;
        *) exit 1 ;;
    esac
done

if [ "$FRESH" = 1 ]; then
    rm -rf "$SRC" "$BUILD"
fi

if [ ! -d "$SRC/.git" ]; then
    git clone https://github.com/hatari/hatari.git "$SRC"
fi

git -C "$SRC" fetch --all --tags
git -C "$SRC" checkout -q "${HATARI_REF:-origin/HEAD}"
git -C "$SRC" reset -q --hard

# The patch is applied to a checkout that is reset above, so this is always a
# clean application and never a second one on top of itself.
git -C "$SRC" apply --whitespace=nowarn "$PATCHES"/*.patch

cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DENABLE_TRACING=1
cmake --build "$BUILD" -j"$(nproc)"

# EmuTOS: a free TOS, which is the whole reason this machine is reachable and
# the Macintosh is not.  cosim/README.md argues that at length.
if [ ! -f "$EMUTOS/emutos-512k-$EMUTOS_VER/etos512us.img" ]; then
    mkdir -p "$EMUTOS"
    zip="$ROOT/work/downloads/emutos-512k-$EMUTOS_VER.zip"
    mkdir -p "$(dirname "$zip")"
    [ -f "$zip" ] || curl -L -o "$zip" \
        "https://sourceforge.net/projects/emutos/files/emutos/$EMUTOS_VER/emutos-512k-$EMUTOS_VER.zip/download"
    unzip -o -q "$zip" -d "$EMUTOS"
fi

echo
echo "hatari:  $BUILD/src/hatari"
echo "EmuTOS:  $EMUTOS/emutos-512k-$EMUTOS_VER/etos512us.img"
echo "next:    cosim/scripts/run-tt.py --help"

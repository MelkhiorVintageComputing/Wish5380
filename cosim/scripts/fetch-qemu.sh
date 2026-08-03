#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Fetch the QEMU source and apply whatever patches this repository carries.
# The source is not in git; the patches are.  A pristine copy is kept beside
# the working tree so a changed patch can be regenerated with diff -ruN.
source "$(dirname "$0")/common.sh"

tarball="$DOWNLOADS/$QEMU_TARBALL"
if [ ! -f "$tarball" ]; then
    say "downloading $QEMU_TARBALL"
    curl -fSL --progress-bar -o "$tarball.part" "$QEMU_URL"
    mv "$tarball.part" "$tarball"
fi

say "verifying the download"
have=$(sha512sum "$tarball" | awk '{print $1}')
[ "$have" = "$QEMU_TARBALL_SHA512" ] || die "sha512 mismatch on $QEMU_TARBALL: got $have"
echo "  sha512 matches the pin in common.sh"

if [ -d "$QEMU_SRC" ]; then
    say "source tree already unpacked; delete $QEMU_SRC to start over"
    exit 0
fi

say "unpacking"
mkdir -p "$QEMU_SRC" "$QEMU_PRISTINE"
tar -xf "$tarball" -C "$QEMU_SRC" --strip-components=1
tar -xf "$tarball" -C "$QEMU_PRISTINE" --strip-components=1

shopt -s nullglob
for p in "$COSIM_DIR"/patches/*.patch; do
    say "applying $(basename "$p")"
    patch -d "$QEMU_SRC" -p1 --no-backup-if-mismatch < "$p" || die "patch failed"
done
say "done"

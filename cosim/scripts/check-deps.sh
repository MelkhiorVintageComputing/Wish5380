#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Report what the co-simulation needs and does not have.  Installs nothing:
# what to do about a missing package is the reader's business, not a script's.
source "$(dirname "$0")/common.sh"

missing=0
need() {
    printf '  %-24s ' "$1"
    if eval "$2" >/dev/null 2>&1; then echo ok; else echo "MISSING - $3"; missing=1; fi
}

say "for the shared library"
need verilator       "command -v verilator"  "verilator"
need "a C++ compiler" "command -v c++"       "g++ or clang"

say "for QEMU"
need meson           "command -v meson"      "meson"
need ninja           "command -v ninja"      "ninja-build"
need pkg-config      "command -v pkg-config" "pkg-config"
need "glib headers"  "pkg-config --exists glib-2.0" "libglib2.0-dev"
need "pixman headers" "pkg-config --exists pixman-1" "libpixman-1-dev"
need python3         "command -v python3"    "python3"

say "for the guest kernel"
need "gcc -m32"      "echo 'int main(){return 0;}' > /tmp/.wd.c && gcc -m32 -c -o /tmp/.wd.o /tmp/.wd.c" \
                     "gcc-multilib, or libc6-dev-i386"
need bison           "command -v bison"      "bison"
need flex            "command -v flex"       "flex"
need bc              "command -v bc"         "bc"
need "libelf headers" "ls /usr/include/libelf.h" "libelf-dev"
need "ssl headers"   "ls /usr/include/openssl/opensslv.h" "libssl-dev"

say "for the card image"
need mke2fs          "command -v mke2fs"     "e2fsprogs"
need sfdisk          "command -v sfdisk"     "util-linux"

if [ "$missing" != 0 ]; then
    die "something is missing; nothing was installed"
fi
say "everything is here"

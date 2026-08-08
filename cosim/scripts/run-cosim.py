#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Boot the guest against the Verilated core and say whether it worked.

The guest is an unmodified i386 Linux.  Its `g_NCR5380` finds the card at the
port the command line names, its `NCR5380.c` arbitrates and selects, and the
SCSI midlayer reads the disk - all of it through `wish5380_sd` behind QEMU's
ISA bus.

The verdict is the serial log plus the card image afterwards, because the two
answer different questions: the log says the driver attached and read what the
host had put there, and the image says what the guest wrote came back out.
"""

import argparse
import os
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
WORK = pathlib.Path(os.environ.get("WORK", ROOT / "work"))

KERNEL = WORK / "linux-build" / "arch" / "x86" / "boot" / "bzImage"
CARD = WORK / "images" / "card.img"
LIB = WORK / "lib" / "libwish5380rtl.so"
# The same tree that carries the Sun-3 machine: one QEMU builds both boards.
QEMU = WORK / "sun3-qemu" / "build" / "qemu-system-i386"

PORT = 0x350
IRQ = 5
BLOCKS = 4096


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=600,
                    help="seconds to allow the guest, which is slow: every "
                         "register access runs the Verilated core")
    ap.add_argument("--trace", action="store_true",
                    help="make the library log every register access")
    ap.add_argument("--log", default=str(WORK / "cosim.log"))
    ap.add_argument("--keep", action="store_true",
                    help="keep whatever the guest wrote to the card")
    args = ap.parse_args()

    for p in (KERNEL, CARD, LIB, QEMU):
        if not p.exists():
            print(f"missing {p}", file=sys.stderr)
            print("run build-sun3-qemu.sh and build-guest.sh first",
                  file=sys.stderr)
            return 2

    card = CARD
    if not args.keep:
        # A run should not change what the next one starts from.
        card = WORK / "images" / "card-run.img"
        card.write_bytes(CARD.read_bytes())

    before = card.read_bytes()

    cmdline = " ".join([
        "console=ttyS0,115200",
        "root=/dev/sda", "rootfstype=ext2", "rw", "init=/init",
        # g_NCR5380's own parameters: a plain 5380 at this port, which is
        # `board=0` - the generic ISA card, registers one byte apart.
        f"g_NCR5380.ncr_addr={PORT}",
        f"g_NCR5380.ncr_irq={IRQ}",
        "g_NCR5380.ncr_5380=1",
        "panic=5", "rootwait",
    ])

    cmd = [
        str(QEMU), "-M", "pc", "-m", "128", "-nographic", "-no-reboot",
        "-kernel", str(KERNEL), "-append", cmdline,
        "-device", (f"ncr5380-isa,iobase={PORT},irq={IRQ},"
                    f"rtl={LIB},image={card},blocks={BLOCKS}"),
    ]

    env = dict(os.environ)
    if args.trace:
        env["WISH_RTL_TRACE"] = "1"

    print("booting the guest against the RTL; this takes a while")
    try:
        out = subprocess.run(cmd, env=env, capture_output=True, text=True,
                             timeout=args.timeout)
        log = out.stdout + out.stderr
    except subprocess.TimeoutExpired as e:
        log = (e.stdout or b"").decode("utf-8", "replace") + \
              (e.stderr or b"").decode("utf-8", "replace")
        pathlib.Path(args.log).write_text(log)
        print(f"the guest never finished; log in {args.log}", file=sys.stderr)
        return 1

    pathlib.Path(args.log).write_text(log)

    failures = []

    def want(pattern: str, what: str):
        m = re.search(pattern, log)
        if m:
            print(f"  ok    {what}: {m.group(0).strip()}")
        else:
            failures.append(what)
            print(f"  FAIL  {what}")

    print("\nwhat the guest said:")
    want(r"scsi host\d+: Generic NCR5380", "the driver attached to the card")
    want(r"scsi \d+:\d+:\d+:\d+: Direct-Access\s+DOLBEAU.*WISH5380",
         "INQUIRY named our target")
    want(r"\[sda\] \d+ 512-byte logical blocks", "READ CAPACITY was believed")
    want(r"EXT2-fs .*mounted|VFS: Mounted root \(ext2", "the root filesystem mounted")
    want(r"WISH5380-COSIM: init running", "the guest's init started")
    want(r"WISH5380-COSIM: read back 'the host put this here",
         "the guest read the host's file off the card")
    want(r"WISH5380-COSIM: wrote a file back", "the guest wrote a file")
    want(r"WISH5380-COSIM: done", "the guest finished")

    if b"the guest wrote this through the RTL" in card.read_bytes():
        print("  ok    what the guest wrote reached the card image")
    else:
        failures.append("what the guest wrote reached the card image")
        print("  FAIL  what the guest wrote reached the card image")

    if before == card.read_bytes():
        failures.append("the card image changed at all")
        print("  FAIL  the card image is byte for byte what it was")

    print()
    if failures:
        print(f"{len(failures)} failure(s); full log in {args.log}")
        return 1
    print(f"all good; full log in {args.log}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

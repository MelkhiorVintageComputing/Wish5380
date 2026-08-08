#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Boot the guest against the card and say whether it worked.

The guest is an unmodified i386 Linux.  Its `g_NCR5380` finds the card at the
port the command line names, its `NCR5380.c` arbitrates and selects, and the
SCSI midlayer reads the disk.

Two chips can sit behind that port and the driver cannot tell them apart,
which is the point of `--core`:

    --core rtl   the Verilated wish5380 in libwish5380rtl.so, with the SD card
                 model behind it and the raw image behind that.  Slow, and the
                 only one that says anything about the RTL.
    --core sw    QEMU's own hw/scsi/ncr5380.c, with an ordinary scsi-hd behind
                 it.  Fast, and what says whether the *driver* is happy.

They differ in what INQUIRY answers, because the target is a different piece
of software in each - the RTL has its own scsi_targ, the software chip has
QEMU's scsi-disk.  Everything else the guest does should look the same.

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
    ap.add_argument("--core", choices=("rtl", "sw"), default="rtl",
                    help="which 5380 sits behind the port (default rtl)")
    args = ap.parse_args()

    needed = [KERNEL, CARD, QEMU] + ([LIB] if args.core == "rtl" else [])
    for p in needed:
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
    ]
    if args.core == "rtl":
        cmd += ["-device", (f"ncr5380-isa,iobase={PORT},irq={IRQ},core=rtl,"
                            f"rtl={LIB},image={card},blocks={BLOCKS}")]
    else:
        # The PC machine's default drive type is IF_IDE and it does not accept
        # if=scsi at all, so the disk is named and attached by hand.
        cmd += [
            "-device", f"ncr5380-isa,iobase={PORT},irq={IRQ},core=sw",
            "-drive", f"if=none,id=sd0,file={card},format=raw",
            "-device", "scsi-hd,drive=sd0",
        ]

    env = dict(os.environ)
    if args.trace:
        env["WISH_RTL_TRACE"] = "1"

    if args.core == "rtl":
        print("booting the guest against the RTL; this takes a while")
    else:
        print("booting the guest against the software 5380")
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
    # The two cores have different targets behind them, so the name differs
    # and the point is that everything either side of it does not.
    vendor = r"DOLBEAU\s+WISH5380" if args.core == "rtl" else r"QEMU\s+QEMU"
    want(r"scsi \d+:\d+:\d+:\d+: Direct-Access\s+" + vendor,
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

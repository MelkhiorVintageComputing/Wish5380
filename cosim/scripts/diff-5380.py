#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Drive both NCR 5380 models with identical stimulus and compare them.

There are two implementations of this part in the tree now.  One is the
SystemVerilog in `src/`, reached through `libwish5380rtl.so`; the other is
`hw/scsi/ncr5380.c`, written in C for QEMU.  Neither is derived from the
other - both were written from
`doc/NCR5380_design_manual_Mar86.pdf` and from the four driver families in
`doc/drivers/` - so where they agree, the datasheet has been read the same way
twice, and where they disagree, one of them has read it wrong.

SunOS is what made this worth building.  It survives four to seven "I/O
request timeout" reports on the Verilated core and reaches a login prompt; on
the software core it stalls in `rc` and produces two thousand of them.  The
two chips differ somewhere, and a guest is much too coarse an instrument to
say where.

## How

Both cards go in one QEMU, at different I/O ports, and this script speaks the
qtest protocol to it over stdin.  Every access is issued to both and every
read is compared, so the stimulus is identical by construction rather than by
two scripts being kept in step.  Nothing else is running in the guest at all -
`-accel qtest` means the CPU never executes an instruction - so a divergence
is the chips and nothing else.

## What it is allowed to compare, and why that is a real question

The two cores have different things behind them: the Verilated one has
`scsi_targ.sv` and an SD card, the software one has whatever QEMU's SCSI bus
was given.  A target that answered differently would diverge for reasons that
have nothing to do with the chip.

So the rule here is that **no target answers on either side**.  The software
card is given no drive at all.  The Verilated card always has a `scsi_targ` on
its internal fabric at ID 0, which cannot be removed - so this never selects
ID 0, and never lets a random ODR write set bit 0.  Every ID this does select
is empty on both sides, and an empty ID behaves identically: nothing asserts
BSY.

What is left is the whole of the register port and most of the bus engine:
the read-back asymmetries, the one refused write, the strobes, the interrupt
latch and what empties it, arbitration, phase match, the SCSI reset, and DRQ.
What is *not* covered is the DMA handshake proper - DACK cycles and End of
Process - because the ISA card is the only board with a register window this
can drive, and an ISA 5380 card has no DMA controller in front of it.  That
half is exercised by the Sun-3 co-simulation, on both cores, and by the
fifteen `dma_` tests in `tb/`.

## Reading a failure

A divergence prints the access that diverged and the twenty before it, which
is usually enough: these are register sequences, and the write that set the
state up is rarely far away.
"""

import argparse
import os
import pathlib
import random
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
WORK = pathlib.Path(os.environ.get("WORK", ROOT / "work"))

QEMU = WORK / "sun3-qemu" / "build" / "qemu-system-i386"
LIB = WORK / "lib" / "libwish5380rtl.so"

SW_PORT = 0x350
RTL_PORT = 0x360

# The eight registers, by the names the datasheet gives them (p. 10).
R_CSD, R_ODR = 0, 0
R_ICR = 1
R_MR = 2
R_TCR = 3
R_CSB, R_SER = 4, 4
R_BSR, R_SDS = 5, 5
R_IDR, R_SDTR = 6, 6
R_RPI, R_SDIR = 7, 7

REG_NAME_R = ["CSD", "ICR", "MR", "TCR", "CSB", "BSR", "IDR", "RPI"]
REG_NAME_W = ["ODR", "ICR", "MR", "TCR", "SER", "SDS", "SDTR", "SDIR"]

ICR_RST, ICR_TEST, ICR_ACK = 0x80, 0x40, 0x10
ICR_BSY, ICR_SEL, ICR_ATN, ICR_DATA = 0x08, 0x04, 0x02, 0x01
ICR_AIP, ICR_LA = 0x40, 0x20

MR_TARGET, MR_PAR_CHK, MR_PAR_INTR = 0x40, 0x20, 0x10
MR_EOP_INTR, MR_MON_BSY, MR_DMA, MR_ARB = 0x08, 0x04, 0x02, 0x01

BSR_END_DMA, BSR_DRQ, BSR_PAR_ERR, BSR_IRQ = 0x80, 0x40, 0x20, 0x10
BSR_PHASE_MATCH, BSR_BUSY_ERR, BSR_ATN, BSR_ACK = 0x08, 0x04, 0x02, 0x01

CSB_RST, CSB_BSY, CSB_REQ, CSB_MSG = 0x80, 0x40, 0x20, 0x10
CSB_CD, CSB_IO, CSB_SEL, CSB_DBP = 0x08, 0x04, 0x02, 0x01

TRAIL = 20


class Qtest:
    """The qtest protocol, which is a line of text each way."""

    def __init__(self, proc):
        self.proc = proc

    def cmd(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        reply = self.proc.stdout.readline()
        if not reply:
            raise RuntimeError("qemu went away")
        reply = reply.strip()
        if reply.startswith("FAIL"):
            raise RuntimeError(f"{line!r} -> {reply}")
        return reply

    def outb(self, addr, val):
        self.cmd(f"outb 0x{addr:x} 0x{val:02x}")

    def inb(self, addr):
        return int(self.cmd(f"inb 0x{addr:x}").split()[-1], 16) & 0xFF


class Pair:
    """Both chips, one stimulus, every read compared."""

    def __init__(self, qt):
        self.qt = qt
        self.trail = []
        self.diffs = []
        self.reads = 0
        self.writes = 0

    def _note(self, text):
        self.trail.append(text)
        if len(self.trail) > TRAIL:
            self.trail.pop(0)

    def w(self, reg, val):
        self.writes += 1
        self._note(f"  w {REG_NAME_W[reg]}({reg}) <- 0x{val:02x}")
        self.qt.outb(SW_PORT + reg, val)
        self.qt.outb(RTL_PORT + reg, val)

    def r(self, reg, what=""):
        self.reads += 1
        sw = self.qt.inb(SW_PORT + reg)
        rtl = self.qt.inb(RTL_PORT + reg)
        mark = "" if sw == rtl else "   <<< DIVERGED"
        self._note(f"  r {REG_NAME_R[reg]}({reg}) -> sw 0x{sw:02x} "
                   f"rtl 0x{rtl:02x}{mark}")
        if sw != rtl:
            self.diffs.append((what or REG_NAME_R[reg], reg, sw, rtl,
                               list(self.trail)))
        return sw, rtl

    def quiesce(self):
        """A known state both chips agree on: an SCSI reset, then empty the
        interrupt latch that the reset deliberately spares (p. 23)."""
        self.w(R_ICR, ICR_RST)
        self.w(R_ICR, 0x00)
        self.r(R_RPI, "quiesce")
        self.trail.clear()


# --------------------------------------------------------------------------
# The scripted checks.  Each is named for what it pins, and each cites the
# page it comes from.
# --------------------------------------------------------------------------

def check_reset_state(p):
    """Every register, straight after a bus reset (p. 23)."""
    p.quiesce()
    for reg in range(8):
        p.r(reg, f"reset state of register {reg}")


def check_icr_reads_back_differently(p):
    """Initiator Command bits 6 and 5 are AIP and LOST ARBITRATION on read
    where TEST MODE and DIFF ENBL were written (p. 12)."""
    p.quiesce()
    for val in (0x00, 0x1f, 0x60, 0x7f, 0x1a, 0x15):
        p.w(R_ICR, val)
        p.r(R_ICR, f"ICR read back after writing 0x{val:02x}")
    p.w(R_ICR, 0x00)


def check_tcr_upper_bits_read_zero(p):
    """Target Command bits 7 and 6:4 are unimplemented on the NMOS part."""
    p.quiesce()
    for val in (0xff, 0xf0, 0x0f, 0xa5):
        p.w(R_TCR, val)
        p.r(R_TCR, f"TCR read back after writing 0x{val:02x}")


def check_mode_dma_needs_busy(p):
    """"BSY must be active in order to set the DMA Mode bit" (p. 14) - the
    one write the chip refuses rather than merely ignores."""
    p.quiesce()
    # BSY false: the bit does not land, everything else does.
    p.w(R_MR, MR_DMA | MR_PAR_CHK)
    p.r(R_MR, "MR after writing DMA with BSY false")
    # Now assert BSY ourselves.  The chip sees its own drive, because the
    # fabric is a wired-OR that includes it.
    p.w(R_ICR, ICR_BSY)
    p.r(R_CSB, "CSB with our own BSY asserted")
    p.w(R_MR, MR_DMA | MR_PAR_CHK)
    p.r(R_MR, "MR after writing DMA with BSY true")
    p.w(R_MR, 0x00)
    p.w(R_ICR, 0x00)


def check_register_seven_clears_three_bits(p):
    """Reading register 7 empties PARITY ERROR, IRQ and BUSY ERROR and not
    END OF DMA (pp. 16-17)."""
    p.quiesce()
    # MONITOR BUSY with BSY false is the cheapest way to raise both BUSY
    # ERROR and the interrupt.
    p.w(R_MR, MR_MON_BSY)
    p.r(R_BSR, "BSR with MONITOR BUSY and no BSY")
    p.r(R_RPI, "the acknowledge")
    p.r(R_BSR, "BSR after acknowledging")
    p.w(R_MR, 0x00)
    p.r(R_BSR, "BSR once MONITOR BUSY is off")
    p.r(R_RPI, "acknowledge again")
    p.r(R_BSR, "BSR after that")


def check_busy_error_is_a_level(p):
    """"set whenever the MONITOR BUSY bit is true and BSY is false" (p. 16).
    Acknowledging empties the latch and the condition refills it, which is
    why NCR5380_intr writes the Mode Register before it reads register 7."""
    p.quiesce()
    p.w(R_MR, MR_MON_BSY)
    for i in range(3):
        p.r(R_BSR, f"BSR, round {i}")
        p.r(R_RPI, f"acknowledge, round {i}")
    # And with BSY asserted it stays away.
    p.w(R_ICR, ICR_BSY)
    p.r(R_RPI, "acknowledge with BSY held")
    p.r(R_BSR, "BSR with BSY held")
    p.w(R_ICR, 0x00)
    p.w(R_MR, 0x00)


def check_arbitration(p):
    """The chip reports AIP and LOST ARBITRATION and nothing else (p. 12);
    "AIP will remain active until the ARBITRATE bit is reset"."""
    p.quiesce()
    p.w(R_ODR, 0x80)            # ID 7, the initiator's own
    p.r(R_ICR, "ICR before arbitrating")
    p.w(R_MR, MR_ARB)
    p.r(R_ICR, "ICR once arbitrating")
    p.r(R_CSD, "the data bus during arbitration")
    p.r(R_CSB, "CSB during arbitration")
    p.w(R_MR, 0x00)
    p.r(R_ICR, "ICR once ARBITRATE is cleared")
    p.r(R_CSD, "the data bus afterwards")


def check_data_bus_needs_assert_data(p):
    """The Output Data Register reaches the bus by itself during arbitration,
    and afterwards only with ASSERT DATA BUS and a matching phase (p. 12)."""
    p.quiesce()
    p.w(R_ODR, 0x5a)
    p.r(R_CSD, "the bus with nothing asserted")
    p.w(R_ICR, ICR_DATA)
    p.r(R_CSD, "the bus with ASSERT DATA BUS")
    p.r(R_CSB, "CSB, for the parity bit")
    p.w(R_ICR, 0x00)
    p.r(R_CSD, "the bus once ASSERT DATA BUS goes")


def check_phase_match_as_target(p):
    """Phase match is {MSG,C/D,I/O} against Target Command bits 2:0 (p. 16),
    and a target drives those lines itself (p. 13)."""
    p.quiesce()
    p.w(R_MR, MR_TARGET)
    for phase in range(8):
        p.w(R_TCR, phase)
        p.r(R_CSB, f"CSB as target in phase {phase}")
        p.r(R_BSR, f"phase match as target in phase {phase}")
    p.w(R_MR, 0x00)
    # As an initiator the chip drives none of them, so the bus reads free and
    # only phase 0 matches.
    for phase in range(8):
        p.w(R_TCR, phase)
        p.r(R_BSR, f"phase match as initiator in phase {phase}")
    p.w(R_TCR, 0x00)


def check_scsi_reset_spares_two_things(p):
    """An SCSI reset clears every register "except for the IRQ interrupt
    latch and the ASSERT RST bit" (p. 23)."""
    p.quiesce()
    p.w(R_MR, MR_PAR_CHK | MR_EOP_INTR)
    p.w(R_TCR, 0x07)
    p.w(R_SER, 0x40)
    p.w(R_ODR, 0x33)
    p.r(R_MR, "MR before the reset")
    p.w(R_ICR, ICR_RST)
    p.r(R_ICR, "ICR during the reset: ASSERT RST survives")
    p.r(R_MR, "MR after the reset")
    p.r(R_TCR, "TCR after the reset")
    p.r(R_BSR, "BSR after the reset: the interrupt latch survives")
    p.r(R_CSB, "CSB with RST on the bus")
    p.w(R_ICR, 0x00)
    p.r(R_BSR, "BSR once RST is released")
    p.r(R_RPI, "the acknowledge")
    p.r(R_BSR, "BSR after acknowledging")


def check_test_mode_floats_everything(p):
    """TEST MODE floats every output driver, "effectively removing the
    NCR 5380 from the circuit" (p. 12)."""
    p.quiesce()
    p.w(R_ICR, ICR_BSY | ICR_SEL)
    p.r(R_CSB, "CSB with BSY and SEL asserted")
    p.w(R_ICR, ICR_BSY | ICR_SEL | ICR_TEST)
    p.r(R_CSB, "CSB with TEST MODE as well")
    p.w(R_ICR, 0x00)
    p.r(R_CSB, "CSB once everything is released")


def check_drq_follows_dma_mode(p):
    """The three start registers arm a transfer whatever is written to them
    (p. 16), and DRQ dies with the DMA MODE bit (p. 25)."""
    p.quiesce()
    # DMA mode needs BSY, so hold it ourselves.
    p.w(R_ICR, ICR_BSY)
    p.w(R_MR, MR_DMA)
    p.r(R_MR, "MR with DMA armed")
    p.r(R_BSR, "BSR before any start register")
    p.w(R_SDS, 0x00)            # Start DMA Send
    p.r(R_BSR, "BSR after Start DMA Send")
    p.w(R_MR, 0x00)             # halting a transfer "at any time" (p. 25)
    p.r(R_BSR, "BSR once DMA MODE is cleared")
    p.w(R_ICR, 0x00)

    # Initiator receive, which asks for nothing until a byte arrives.
    p.quiesce()
    p.w(R_ICR, ICR_BSY)
    p.w(R_MR, MR_DMA)
    p.w(R_SDIR, 0x00)           # Start DMA Initiator Receive
    p.r(R_BSR, "BSR after Start DMA Initiator Receive")
    p.w(R_MR, 0x00)
    p.w(R_ICR, 0x00)


def check_selection_of_an_empty_id(p):
    """A full arbitrate-and-select at an ID nothing answers to, which is the
    sequence NCR5380_select walks and the one an empty bus must survive.

    Target 0 is never used: the Verilated card always carries a scsi_targ
    there and the software card carries nothing, so it is the one ID where
    the two are legitimately different."""
    p.quiesce()
    own = 0x80                  # this initiator is ID 7, as every driver is
    for target in (1, 2, 5, 6):
        p.quiesce()
        p.w(R_TCR, 0x00)        # or the chip will not drive the bus (p. 12)
        p.w(R_ODR, own)
        p.w(R_MR, MR_ARB)
        p.r(R_ICR, f"AIP arbitrating for target {target}")
        p.r(R_CSD, f"the data bus arbitrating for target {target}")
        p.w(R_ICR, ICR_SEL | ICR_BSY)
        p.w(R_ODR, own | (1 << target))
        p.w(R_ICR, ICR_BSY | ICR_DATA | ICR_ATN | ICR_SEL)
        p.w(R_MR, 0x00)
        p.w(R_SER, 0x00)
        p.r(R_ICR, f"ICR selecting target {target}")
        p.w(R_ICR, ICR_DATA | ICR_ATN | ICR_SEL)
        p.r(R_CSB, f"CSB selecting target {target}")
        p.r(R_CSD, f"the data bus selecting target {target}")
        p.r(R_BSR, f"BSR selecting target {target}")
        # Nothing is there, so BSY never appears and a driver times out.
        for _ in range(4):
            p.r(R_CSB, f"CSB waiting for target {target}")
        p.w(R_ICR, 0x00)
        p.r(R_CSB, f"CSB after giving up on target {target}")


CHECKS = [
    check_reset_state,
    check_icr_reads_back_differently,
    check_tcr_upper_bits_read_zero,
    check_mode_dma_needs_busy,
    check_register_seven_clears_three_bits,
    check_busy_error_is_a_level,
    check_arbitration,
    check_data_bus_needs_assert_data,
    check_phase_match_as_target,
    check_scsi_reset_spares_two_things,
    check_test_mode_floats_everything,
    check_drq_follows_dma_mode,
    check_selection_of_an_empty_id,
]


def random_walk(p, steps, seed):
    """The part the scripted checks cannot reach: whatever nobody thought of.

    Writes are drawn from the five registers that hold something and the three
    that are strobes; reads from all eight.  Two constraints, both to keep the
    "no target answers" rule true: bit 0 never reaches the Output Data
    Register, so the Verilated card's target at ID 0 is never selected; and a
    quiesce every so often, so that one divergence does not colour the rest of
    the run.
    """
    rnd = random.Random(seed)
    for i in range(steps):
        if i % 64 == 0:
            p.quiesce()
        op = rnd.randrange(10)
        if op < 4:
            reg = rnd.randrange(8)
            p.r(reg, f"random walk step {i}")
        else:
            reg = rnd.randrange(8)
            val = rnd.randrange(256)
            if reg == R_ODR:
                val &= ~0x01    # never address the Verilated card's target
            p.w(reg, val)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qemu", default=str(QEMU))
    ap.add_argument("--rtl", default=str(LIB))
    ap.add_argument("--steps", type=int, default=4000,
                    help="random walk length (0 disables it)")
    ap.add_argument("--seed", type=int, default=20260808)
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="print every access, not only the divergences")
    args = ap.parse_args()

    for what, path in (("QEMU", args.qemu), ("RTL library", args.rtl)):
        if not os.path.exists(path):
            print(f"missing {what}: {path}", file=sys.stderr)
            print("run cosim/scripts/build-sun3-qemu.sh and make -C cosim/rtl",
                  file=sys.stderr)
            return 2

    cmd = [
        args.qemu, "-M", "pc", "-m", "64", "-accel", "qtest",
        "-display", "none", "-serial", "none", "-monitor", "none",
        "-qtest", "stdio",
        "-device", f"ncr5380-isa,iobase=0x{SW_PORT:x},irq=5,core=sw",
        "-device", (f"ncr5380-isa,iobase=0x{RTL_PORT:x},irq=7,core=rtl,"
                    f"rtl={args.rtl}"),
    ]

    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True, bufsize=1)
    qt = Qtest(proc)
    p = Pair(qt)

    try:
        print(f"software 5380 at 0x{SW_PORT:03x}, Verilated at "
              f"0x{RTL_PORT:03x}\n")
        for check in CHECKS:
            before = len(p.diffs)
            check(p)
            name = check.__name__.removeprefix("check_")
            n = len(p.diffs) - before
            print(f"  {'FAIL' if n else 'ok  '}  {name}"
                  + (f"   ({n} divergence(s))" if n else ""))
        if args.steps:
            before = len(p.diffs)
            random_walk(p, args.steps, args.seed)
            n = len(p.diffs) - before
            print(f"  {'FAIL' if n else 'ok  '}  random_walk "
                  f"({args.steps} steps, seed {args.seed})"
                  + (f"   ({n} divergence(s))" if n else ""))
    finally:
        try:
            proc.stdin.close()
        except BrokenPipeError:
            pass
        proc.terminate()
        proc.wait(timeout=30)

    print(f"\n{p.writes} writes, {p.reads} reads compared")

    if not p.diffs:
        print("the two chips agree everywhere this can see")
        return 0

    # One report per distinct (register, sw, rtl), because a walk that goes
    # wrong tends to go wrong the same way many times over.
    seen = {}
    for what, reg, sw, rtl, trail in p.diffs:
        seen.setdefault((reg, sw, rtl), (what, trail, 0))
        w, t, n = seen[(reg, sw, rtl)]
        seen[(reg, sw, rtl)] = (w, t, n + 1)

    print(f"\n{len(p.diffs)} divergence(s), {len(seen)} distinct:\n")
    for (reg, sw, rtl), (what, trail, n) in seen.items():
        print(f"--- {REG_NAME_R[reg]} (register {reg}): "
              f"software 0x{sw:02x}, Verilated 0x{rtl:02x}"
              f"   x{n}\n    at: {what}")
        for line in trail:
            print(f"    {line}")
        print()
    return 1


if __name__ == "__main__":
    sys.exit(main())

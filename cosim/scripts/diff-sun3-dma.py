#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare the two 5380 models across the DMA handshake, on the Sun-3 board.

`diff-5380.py` compares the register port and the bus engine, and after the
bugs it found the two models agree there over tens of thousands of reads.  What
it cannot reach is the DMA handshake - DACK cycles and End of Process - because
the ISA card is the only board with a register window it can drive, and an ISA
5380 card has no DMA controller in front of it.

The open fault is on 8192-byte DMA transfers.  So the one half never compared
was the one half still under suspicion, and this is that half.

## How this one differs

The Sun-3 machine carries exactly one `si`, so the two chips cannot share a
QEMU the way the two ISA cards do.  There are two machines instead, driven
from one script over two qtest connections, which is very nearly as good: the
stimulus is still written once, and `-accel qtest` means neither guest CPU
executes an instruction, so nothing else is running to drift.

Getting at DVMA is the awkward part.  The board masters the bus through the
Sun-3 MMU, and with no guest there are no page tables, so every DMA access
would fail translation and both sides would agree on a bus error and prove
nothing.  The MMU's segment and page maps are ordinary MMIO on this machine -
0x90000000 and 0xA0000000 - so this builds a mapping by hand: DVMA address D
is virtual 0x0F000000 + D in context 0, and the page table entry points it at
real memory this script has already written through qtest.

## What can be compared without a target, and why that is still the point

The same rule as the register harness: no target answers on either side, so
that a difference between `scsi_targ.sv` and QEMU's `scsi-disk.c` cannot be
mistaken for a difference between the chips.

That bounds a transfer to one byte, and the bound is worth understanding
rather than working around.  A Start DMA Send raises DRQ before the chip has
anywhere to put the byte - the Output Data Register is its one byte of buffer,
and filling it early is exactly what lets END OF DMA be set "while the SCSI
transfer may still be in progress" (p. 20).  So the board's DMA controller
reads a byte out of memory and hands it over, and then the chip waits for a
REQ that no one will send.  One byte, and it is the *interesting* byte: with
the FIFO count at one it is also the last byte, which is where End of Process
is asserted and where END OF DMA is set.

So this covers the whole acknowledge path - DRQ, the DACK write, the FIFO
packing and its residue register, the byte count, the UDC's chain and count,
DMA ACTIVE - and the EOP boundary that only ever happens once per transfer.
What it does not cover is a transfer that runs, which needs a device to
handshake with; `cosim/README.md` records that as what is left.
"""

import argparse
import os
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
WORK = pathlib.Path(os.environ.get("WORK", ROOT / "work"))
FORK = pathlib.Path(os.environ.get("SUN3_FORK", os.path.expanduser("~/qemu-sun3")))

QEMU = WORK / "sun3-qemu" / "build" / "qemu-system-m68k"
LIB = WORK / "lib" / "libwish5380rtl.so"
PROM = FORK / "roms" / "sun3_60_v3.0.1.bin"

SI = 0x0FF40000                 # the board, obio 0x140000

# The 5380's eight registers, then Sun's own.
R_CSD = R_ODR = 0
R_ICR = 1
R_MR = 2
R_TCR = 3
R_CSB = R_SER = 4
R_BSR = R_SDS = 5
R_IDR = R_SDTR = 6
R_RPI = R_SDIR = 7

UDC_DATA, UDC_ADDR = 0x10, 0x12
FIFO_DATA, FIFO_COUNT, CSR = 0x14, 0x16, 0x18

ICR_RST, ICR_ACK, ICR_BSY, ICR_SEL, ICR_ATN, ICR_DATA = \
    0x80, 0x10, 0x08, 0x04, 0x02, 0x01
MR_TARGET, MR_MON_BSY, MR_DMA, MR_ARB = 0x40, 0x04, 0x02, 0x01
BSR_END_DMA, BSR_DRQ, BSR_IRQ, BSR_PHASE_MATCH = 0x80, 0x40, 0x10, 0x08

CSR_DMA_ACTIVE, CSR_DMA_BUS_ERR, CSR_FIFO_EMPTY = 0x8000, 0x2000, 0x0400
CSR_SBC_IP, CSR_SEND, CSR_INTR_EN = 0x0200, 0x0008, 0x0004
CSR_FIFO_RES, CSR_SCSI_RES = 0x0002, 0x0001

# Am9516 register pointers and commands, as sun3-si.c decodes them.
UDC_ADR_COMMAND, UDC_ADR_CAR_HIGH, UDC_ADR_CAR_LOW = 0x2e, 0x26, 0x22
UDC_CMD_STRT_CHN = 0xa0
RSEL_CUR_ARA, RSEL_CUR_COUNT = 1 << 9, 1 << 7

# The Sun-3 MMU, which on this machine is ordinary MMIO.
MMU_CONTEXT, MMU_SEGMENT, MMU_PAGE = 0x80000000, 0x90000000, 0xA0000000
PTE_VALID, PTE_WRITE, PTE_SYSTEM = 1 << 31, 1 << 30, 1 << 29
PTE_REF, PTE_MOD = 1 << 25, 1 << 24
PAGE_SIZE = 0x2000

# Where the transfer lives.  DVMA 0xF00000 is where the guests put theirs, and
# it is one 128 KB segment, so one PMEG covers everything this needs.
DVMA_BASE = 0xF00000
PHYS_BASE = 0x00200000          # 2 MB into RAM, where nothing else is
PMEG = 0x10

CHAIN_DVMA = DVMA_BASE          # the Am9516 reload table
BUF_DVMA = DVMA_BASE + 0x2000   # the data, one page along

TRAIL = 24


class Qtest:
    def __init__(self, proc, name):
        self.proc = proc
        self.name = name

    def cmd(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        reply = self.proc.stdout.readline()
        if not reply:
            raise RuntimeError(f"{self.name}: qemu went away")
        reply = reply.strip()
        if reply.startswith("FAIL"):
            raise RuntimeError(f"{self.name}: {line!r} -> {reply}")
        return reply

    def readb(self, a):
        return int(self.cmd(f"readb 0x{a:x}").split()[-1], 16) & 0xFF

    def readw(self, a):
        return int(self.cmd(f"readw 0x{a:x}").split()[-1], 16) & 0xFFFF

    def writeb(self, a, v):
        self.cmd(f"writeb 0x{a:x} 0x{v:02x}")

    def writew(self, a, v):
        self.cmd(f"writew 0x{a:x} 0x{v:04x}")

    def writel(self, a, v):
        self.cmd(f"writel 0x{a:x} 0x{v:08x}")

    def write_mem(self, a, data):
        self.cmd(f"write 0x{a:x} 0x{len(data):x} 0x{data.hex()}")

    def read_mem(self, a, n):
        return bytes.fromhex(self.cmd(f"read 0x{a:x} 0x{n:x}").split()[-1][2:])


class Pair:
    """Two machines, one stimulus, every read compared."""

    def __init__(self, sw, rtl):
        self.sw, self.rtl = sw, rtl
        self.trail = []
        self.diffs = []
        self.vacuous = []
        self.reads = 0

    def _note(self, s):
        self.trail.append(s)
        if len(self.trail) > TRAIL:
            self.trail.pop(0)

    # ---- the board ------------------------------------------------------
    def wb(self, off, val):
        self._note(f"  w  [{off:02x}] <- 0x{val:02x}")
        self.sw.writeb(SI + off, val)
        self.rtl.writeb(SI + off, val)

    def ww(self, off, val):
        self._note(f"  w16[{off:02x}] <- 0x{val:04x}")
        self.sw.writew(SI + off, val)
        self.rtl.writew(SI + off, val)

    def rb(self, off, what=""):
        self.reads += 1
        a, b = self.sw.readb(SI + off), self.rtl.readb(SI + off)
        self._note(f"  r  [{off:02x}] -> sw 0x{a:02x} rtl 0x{b:02x}"
                   + ("" if a == b else "   <<< DIVERGED"))
        if a != b:
            self.diffs.append((what or f"register {off}", f"0x{a:02x}",
                               f"0x{b:02x}", list(self.trail)))
        return a

    def rw(self, off, what=""):
        self.reads += 1
        a, b = self.sw.readw(SI + off), self.rtl.readw(SI + off)
        self._note(f"  r16[{off:02x}] -> sw 0x{a:04x} rtl 0x{b:04x}"
                   + ("" if a == b else "   <<< DIVERGED"))
        if a != b:
            self.diffs.append((what or f"register {off}", f"0x{a:04x}",
                               f"0x{b:04x}", list(self.trail)))
        return a

    # ---- memory ---------------------------------------------------------
    def wmem(self, phys, data):
        self.sw.write_mem(phys, data)
        self.rtl.write_mem(phys, data)

    def rmem(self, phys, n, what):
        self.reads += 1
        a, b = self.sw.read_mem(phys, n), self.rtl.read_mem(phys, n)
        if a != b:
            where = next((i for i in range(min(len(a), len(b)))
                          if a[i] != b[i]), 0)
            self._note(f"  mem 0x{phys:x}+{n} DIVERGED at byte {where}")
            self.diffs.append((what, f"...{a[where]:02x}...",
                               f"...{b[where]:02x}...", list(self.trail)))
        return a

    def require(self, got, mask, what):
        """The two agreeing is worth nothing if neither of them did anything.
        This records what a check claims to have exercised, and complains if
        the software side shows no sign of it."""
        if (got & mask) != mask:
            self.vacuous.append(f"{what}: expected 0x{mask:x} in 0x{got:x}")

    def quiesce(self):
        """A known state: the board's own reset line, which resets the 5380
        and the UDC both, then empty the interrupt latch."""
        self.ww(CSR, 0)                                 # both resets asserted
        self.ww(CSR, CSR_SCSI_RES | CSR_FIFO_RES)       # and released
        self.rb(R_RPI, "quiesce")
        self.trail.clear()


def setup_mmu(qt, pages):
    """Build a context-0 DVMA mapping by hand, because there is no guest to
    have built one.  A DVMA address D is virtual 0x0F000000 + D."""
    qt.writeb(MMU_CONTEXT, 0)
    for i in range(pages):
        v = 0x0F000000 + DVMA_BASE + i * PAGE_SIZE
        phys = PHYS_BASE + i * PAGE_SIZE
        qt.writeb(MMU_SEGMENT + v, PMEG)
        qt.writel(MMU_PAGE + v,
                  PTE_VALID | PTE_WRITE | PTE_SYSTEM | PTE_REF | PTE_MOD
                  | (phys >> 13))


def chain_table(buf_dvma, words):
    """The Am9516 reload table the board's chain parser walks: a register
    selection word, then the fields it names.  Only the current address and
    the count are read, which is all Sun ever writes."""
    rsel = RSEL_CUR_ARA | RSEL_CUR_COUNT
    hi = ((buf_dvma >> 8) & 0xFF00) | 0x40      # A23-A16, then "memory, +1"
    lo = buf_dvma & 0xFFFF
    return b"".join(x.to_bytes(2, "big") for x in (rsel, hi, lo, words))


def arm_dma(p, count, send, buf=BUF_DVMA):
    """Everything a driver does between deciding to move bytes and the chip
    being told to: the chain into memory, the UDC pointed at it, the FIFO
    count, the direction, and finally the 5380."""
    p.wmem(PHYS_BASE, chain_table(buf, (count + 1) // 2))

    p.ww(UDC_ADDR, UDC_ADR_CAR_HIGH)
    p.ww(UDC_DATA, ((CHAIN_DVMA >> 8) & 0xFF00) | 0x40)
    p.ww(UDC_ADDR, UDC_ADR_CAR_LOW)
    p.ww(UDC_DATA, CHAIN_DVMA & 0xFFFF)
    p.ww(UDC_ADDR, UDC_ADR_COMMAND)
    p.ww(UDC_DATA, UDC_CMD_STRT_CHN)

    # The board refuses a FIFO count written during a DATA phase, and an idle
    # bus decodes as phase 0, which *is* DATA OUT - so the count has to be set
    # from some other phase.  Every driver does this anyway, during COMMAND or
    # MESSAGE IN; here the chip drives COMMAND at itself for the one access.
    p.wb(R_MR, MR_TARGET)
    p.wb(R_TCR, 0x02)                       # C/D true: COMMAND
    p.ww(FIFO_COUNT, count)
    p.wb(R_TCR, 0x00)
    p.wb(R_MR, 0x00)
    p.ww(CSR, CSR_SCSI_RES | CSR_FIFO_RES | CSR_INTR_EN
         | (CSR_SEND if send else 0))


# --------------------------------------------------------------------------
# The checks
# --------------------------------------------------------------------------

def check_board_reset_state(p):
    """The board and the chip after the CSR's two reset lines, which are
    active low and take the 5380 and the UDC down together."""
    p.quiesce()
    for off in range(8):
        p.rb(off, f"5380 register {off} after a board reset")
    p.rw(CSR, "CSR after a board reset")
    p.rw(FIFO_COUNT, "FIFO count after a board reset")
    p.rw(FIFO_DATA, "FIFO data after a board reset")


def check_chain_and_count(p):
    """The UDC's chain parse: an address pair whose high word carries A23-A16
    in its top byte, and a word count."""
    p.quiesce()
    arm_dma(p, 512, send=True)
    p.rw(CSR, "CSR once the chain is started")
    p.rw(FIFO_COUNT, "the FIFO count as armed")
    # Nothing has been asked for yet, because the chip has not been told.
    p.rb(R_BSR, "BSR before the chip is armed")


def check_fifo_count_refused_in_a_data_phase(p):
    """"The OBIO si IGNORES any attempt to set the FIFO count register after
    the SCSI bus goes into any DATA phase", which is why every driver sets it
    during COMMAND or MESSAGE IN."""
    p.quiesce()
    # Drive a DATA IN phase from the chip itself, as a target would.
    p.wb(R_MR, MR_TARGET)
    p.wb(R_TCR, 0x01)                       # I/O true: DATA IN
    p.rb(R_CSB, "CSB in a DATA phase")
    p.ww(FIFO_COUNT, 0x1234)
    p.rw(FIFO_COUNT, "the FIFO count written during a DATA phase")
    # Out of the data phase, and it lands.
    p.wb(R_TCR, 0x02)                       # C/D true: COMMAND
    p.rb(R_CSB, "CSB in COMMAND")
    p.ww(FIFO_COUNT, 0x1234)
    p.rw(FIFO_COUNT, "the FIFO count written outside a DATA phase")
    p.wb(R_MR, 0)
    p.wb(R_TCR, 0)


def check_drq_and_one_acknowledge(p):
    """The acknowledge path, one byte of it.

    A Start DMA Send raises DRQ before the chip has anywhere to put the byte,
    so the board's DMA controller reads one out of memory and hands it over
    without any device being involved.  Then the chip waits for a REQ that
    will not come, which is where this stops."""
    p.quiesce()
    p.wmem(PHYS_BASE + 0x2000, bytes(range(0x40, 0x50)) * 16)

    arm_dma(p, 512, send=True)
    p.wb(R_ICR, ICR_BSY)            # DMA mode is refused without BSY (p. 14)
    p.wb(R_MR, MR_DMA)
    p.rb(R_MR, "MR with DMA armed")
    p.rb(R_BSR, "BSR before Start DMA Send")

    p.wb(R_SDS, 0)                  # write register 5: Start DMA Send
    p.rb(R_BSR, "BSR after Start DMA Send")
    count = p.rw(FIFO_COUNT, "the FIFO count after one byte has moved")
    p.require(511 - count + 1, 1, "a byte actually moving")
    p.rw(FIFO_DATA, "the FIFO's leftover byte")
    p.rw(CSR, "CSR mid-transfer")

    # And it stays put: nothing is asking for the second byte.
    p.rw(FIFO_COUNT, "the FIFO count, still")
    p.rb(R_BSR, "BSR, still")

    p.wb(R_MR, 0)                   # halt it (p. 25)
    p.rb(R_BSR, "BSR once DMA MODE is cleared")
    p.wb(R_ICR, 0)


def check_end_of_process_sets_end_of_dma(p):
    """End of Process is asserted across the last acknowledge, and that is
    what sets END OF DMA (p. 16).  With the FIFO count at one, the first byte
    is also the last, so the whole EOP boundary happens in one transfer.

    This is the one thing here that cannot be reached from the ISA card at
    all: a CPU moving bytes has no End of Process pin, which is the whole
    difference between the Macintosh's pseudo-DMA and the Sun-3's Am9516."""
    p.quiesce()
    p.wmem(PHYS_BASE + 0x2000, b"\xa5" * 16)

    arm_dma(p, 1, send=True)
    p.wb(R_ICR, ICR_BSY)
    p.wb(R_MR, MR_DMA)
    p.wb(R_SDS, 0)

    bsr = p.rb(R_BSR, "BSR after the last byte, with EOP asserted across it")
    p.require(bsr, BSR_END_DMA, "END OF DMA after an EOP acknowledge")
    count = p.rw(FIFO_COUNT, "the FIFO count at the end")
    p.require(~count & 0xffff, 0xffff, "the FIFO count reaching zero")
    csr = p.rw(CSR, "CSR at the end: DMA ACTIVE should have gone")
    p.require(~csr & CSR_DMA_ACTIVE, CSR_DMA_ACTIVE, "DMA ACTIVE clearing")
    data = p.rw(FIFO_DATA, "the byte that moved")
    p.require(data, 0xa500, "the byte reaching the FIFO's residue register")

    # "reset when the DMA MODE bit is reset", and by nothing else - reading
    # register 7 does not clear it, which is why it is not in the three bits
    # that acknowledge does.
    p.rb(R_RPI, "the acknowledge")
    p.rb(R_BSR, "BSR after acknowledging: END OF DMA survives it")
    p.wb(R_MR, 0)
    p.rb(R_BSR, "BSR once DMA MODE is cleared")
    p.wb(R_ICR, 0)


def check_receive_asks_for_nothing(p):
    """A Start DMA Initiator Receive asks for nothing until a byte arrives:
    the chip latches on REQ and only then raises DRQ, so with no device the
    handshake never starts and the board must not move anything."""
    p.quiesce()
    arm_dma(p, 512, send=False)
    p.wb(R_ICR, ICR_BSY)
    p.wb(R_MR, MR_DMA)
    p.wb(R_SDIR, 0)                 # write register 7
    p.rb(R_BSR, "BSR after Start DMA Initiator Receive")
    p.rw(FIFO_COUNT, "the FIFO count, which must not have moved")
    p.rw(CSR, "CSR with a receive armed and nothing arriving")
    p.wb(R_MR, 0)
    p.wb(R_ICR, 0)


def check_a_send_that_is_never_armed(p):
    """The UDC is armed before the chip is told anything - every driver here
    starts the chain first and writes the Target Command Register after - so
    there is a window where DMA ACTIVE is set about a transfer that has not
    started.  Nothing may move in it."""
    p.quiesce()
    arm_dma(p, 64, send=True)
    for i in range(4):
        p.rw(CSR, f"CSR armed but not started, {i}")
        p.rw(FIFO_COUNT, f"the FIFO count armed but not started, {i}")
        p.rb(R_BSR, f"BSR armed but not started, {i}")


def check_dma_bit_needs_busy_on_this_board(p):
    """The one refused write, reached through the board's own register
    window rather than through an I/O port."""
    p.quiesce()
    p.wb(R_MR, MR_DMA)
    p.rb(R_MR, "MR written with DMA and no BSY")
    p.wb(R_ICR, ICR_BSY)
    p.wb(R_MR, MR_DMA)
    p.rb(R_MR, "MR written with DMA and BSY held")
    p.wb(R_MR, 0)
    p.wb(R_ICR, 0)


def check_word_access_is_two_register_cycles(p):
    """The chip is eight byte-wide registers on a 16-bit bus, so a word
    access is two register cycles, high byte first - and register 7 is a
    strobe, which makes a word read of 6 and 7 an acknowledge as well as a
    read of Input Data."""
    p.quiesce()
    p.wb(R_ICR, ICR_BSY)
    p.wb(R_MR, MR_MON_BSY)
    p.rw(R_MR, "a word read of Mode and Target Command")
    p.rw(R_CSB, "a word read of Current SCSI Bus Status and Bus and Status")
    p.wb(R_ICR, 0)
    p.rb(R_BSR, "BSR after the word reads")
    p.rw(R_IDR, "a word read of Input Data and Reset Parity/Interrupt")
    p.rb(R_BSR, "BSR after that word read, which acknowledged")
    p.wb(R_MR, 0)


CHECKS = [
    check_board_reset_state,
    check_dma_bit_needs_busy_on_this_board,
    check_word_access_is_two_register_cycles,
    check_chain_and_count,
    check_fifo_count_refused_in_a_data_phase,
    check_a_send_that_is_never_armed,
    check_drq_and_one_acknowledge,
    check_end_of_process_sets_end_of_dma,
    check_receive_asks_for_nothing,
]


def start(qemu, prom, core, rtl):
    machine = f"sun3,si-core={core}"
    if core == "rtl":
        machine += f",si-rtl={rtl}"
    cmd = [qemu, "-M", machine, "-m", "4M", "-bios", prom,
           "-accel", "qtest", "-qtest", "stdio",
           "-display", "none", "-serial", "none", "-monitor", "none"]
    return subprocess.Popen(cmd, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, bufsize=1)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qemu", default=str(QEMU))
    ap.add_argument("--rtl", default=str(LIB))
    ap.add_argument("--prom", default=str(PROM))
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="print every access, so that a check which passes "
                         "because nothing happened is visible as one")
    args = ap.parse_args()

    for what, path in (("QEMU", args.qemu), ("RTL library", args.rtl),
                       ("PROM", args.prom)):
        if not os.path.exists(path):
            print(f"missing {what}: {path}", file=sys.stderr)
            return 2

    sw_p = start(args.qemu, args.prom, "sw", args.rtl)
    rtl_p = start(args.qemu, args.prom, "rtl", args.rtl)
    sw, rtl = Qtest(sw_p, "sw"), Qtest(rtl_p, "rtl")
    p = Pair(sw, rtl)

    try:
        for qt in (sw, rtl):
            setup_mmu(qt, pages=8)
        print("software 5380 and Verilated 5380, one Sun-3 board each\n")
        for check in CHECKS:
            before = len(p.diffs)
            check(p)
            n = len(p.diffs) - before
            name = check.__name__.removeprefix("check_")
            print(f"  {'FAIL' if n else 'ok  '}  {name}"
                  + (f"   ({n} divergence(s))" if n else ""))
            if args.verbose:
                for line in p.trail:
                    print(f"      {line}")
    finally:
        for proc in (sw_p, rtl_p):
            try:
                proc.stdin.close()
            except BrokenPipeError:
                pass
            proc.terminate()
            proc.wait(timeout=30)

    print(f"\n{p.reads} reads compared")
    if p.vacuous:
        print("\nchecks that did not exercise what they claim:")
        for v in p.vacuous:
            print(f"  {v}")
        return 1
    if not p.diffs:
        print("the two chips agree everywhere this can see")
        return 0

    print(f"\n{len(p.diffs)} divergence(s):\n")
    for what, a, b, trail in p.diffs:
        print(f"--- {what}: software {a}, Verilated {b}")
        for line in trail:
            print(f"    {line}")
        print()
    return 1


if __name__ == "__main__":
    sys.exit(main())

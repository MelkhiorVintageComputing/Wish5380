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

    def clock_step(self, ns):
        self.cmd(f"clock_step {ns}")

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


# --------------------------------------------------------------------------
# A running transfer
#
# Everything above moves one byte, because with no target nothing will
# handshake.  This part gives both sides the *same disk*, so a whole transfer
# runs and the bytes that come back can be compared - and compared against the
# file itself, which is the opinion neither model gets a vote on.
#
# It cannot be lock-step.  The Verilated target takes real time to answer and
# QEMU's answers at once, so the two sides need different numbers of polls to
# reach the same place; comparing poll for poll would report timing as
# divergence.  Each side is driven independently to the same *state* instead,
# and what is compared is the data, the status, the message and the registers
# once the dust has settled.
# --------------------------------------------------------------------------

PH_DATA_OUT, PH_DATA_IN, PH_COMMAND, PH_STATUS = 0, 1, 2, 3
PH_MSG_OUT, PH_MSG_IN = 6, 7

CSB_BSY, CSB_REQ, CSB_PHASE = 0x40, 0x20, 0x1c
ICR_AIP = 0x40
INITIATOR_ID = 0x80             # ID 7, as every driver here is


class Timeout(Exception):
    pass


class Side:
    """One machine, driven the way si.c drives it."""

    def __init__(self, qt, name, tries=3000):
        self.qt = qt
        self.name = name
        self.tries = tries
        self.accesses = 0

    def wb(self, off, val):
        self.accesses += 1
        self.qt.writeb(SI + off, val)

    def rb(self, off):
        self.accesses += 1
        return self.qt.readb(SI + off)

    def ww(self, off, val):
        self.accesses += 1
        self.qt.writew(SI + off, val)

    def rw(self, off):
        self.accesses += 1
        return self.qt.readw(SI + off)

    def until(self, off, mask, want, what, tries=None, wide=False):
        tries = tries or self.tries
        for _ in range(tries):
            v = self.rw(off) if wide else self.rb(off)
            if (v & mask) == want:
                return v
            self.last = v
        raise Timeout(f"{self.name}: {what} (last 0x{self.last:x})\n"
                      f"        {self.dump()}")

    def dump(self):
        """What the board and the chip look like when something has stopped.
        The same registers si.c prints when it gives up, and in the same
        order, so the two can be read against each other."""
        csb, bsr = self.rb(R_CSB), self.rb(R_BSR)
        icr, mr, tcr = self.rb(R_ICR), self.rb(R_MR), self.rb(R_TCR)
        return (f"csr=0x{self.rw(CSR):04x} count=0x{self.rw(FIFO_COUNT):04x} "
                f"fifo=0x{self.rw(FIFO_DATA):04x} | csb=0x{csb:02x} "
                f"(phase {(csb & CSB_PHASE) >> 2}) bsr=0x{bsr:02x} "
                f"icr=0x{icr:02x} mr=0x{mr:02x} tcr=0x{tcr:02x}")

    def board_reset(self):
        self.ww(CSR, 0)
        self.ww(CSR, CSR_SCSI_RES | CSR_FIFO_RES)
        self.rb(R_RPI)

    # ---- the phases ------------------------------------------------------
    def select(self, target):
        """NCR5380_select, step for step."""
        self.wb(R_TCR, 0)               # or the chip will not drive the bus
        self.wb(R_ODR, INITIATOR_ID)
        self.wb(R_MR, MR_ARB)
        self.until(R_ICR, ICR_AIP, ICR_AIP, "arbitration never started")
        # The 2.2 us arbitration delay is the driver's (p. 18); here every
        # register access buys the Verilated core a microsecond of its own
        # time, which is the same thing by a different route.
        self.wb(R_ICR, ICR_SEL | ICR_BSY)
        self.wb(R_ODR, INITIATOR_ID | (1 << target))
        self.wb(R_ICR, ICR_BSY | ICR_DATA | ICR_ATN | ICR_SEL)
        self.wb(R_MR, 0)
        self.wb(R_SER, 0)
        self.wb(R_ICR, ICR_DATA | ICR_ATN | ICR_SEL)    # drop BSY
        self.until(R_CSB, CSB_BSY, CSB_BSY, f"target {target} never answered")
        self.wb(R_ICR, ICR_ATN)         # release SEL and the data bus

    def phase(self):
        return (self.rb(R_CSB) & CSB_PHASE) >> 2

    def pio_out(self, ph, data, keep_atn_until_last=False):
        self.wb(R_TCR, ph)
        for i, byte in enumerate(data):
            self.until(R_CSB, CSB_REQ, CSB_REQ, f"no REQ in phase {ph}")
            keep = ICR_ATN if (keep_atn_until_last and i + 1 < len(data)) else 0
            self.wb(R_ODR, byte)
            self.wb(R_ICR, keep | ICR_DATA)
            self.wb(R_ICR, keep | ICR_DATA | ICR_ACK)
            self.until(R_CSB, CSB_REQ, 0, f"REQ stuck in phase {ph}")
            self.wb(R_ICR, keep)

    def pio_in(self, ph, n):
        out = bytearray()
        self.wb(R_TCR, ph)
        for _ in range(n):
            self.until(R_CSB, CSB_REQ, CSB_REQ, f"no REQ in phase {ph}")
            out.append(self.rb(R_CSD))
            self.wb(R_ICR, ICR_ACK)
            self.until(R_CSB, CSB_REQ, 0, f"REQ stuck in phase {ph}")
            self.wb(R_ICR, 0)
        return bytes(out)

    def arm_udc(self, count, send, buf=BUF_DVMA):
        """The chain and the byte count.  The count has to be written from a
        phase that is not a DATA phase, which is why every driver sets it
        during COMMAND or MESSAGE IN - and why this is called before the CDB
        goes out rather than after."""
        self.qt.write_mem(PHYS_BASE, chain_table(buf, (count + 1) // 2))
        # The byte count goes in *before* the chain is started, and the order
        # matters: starting the chain is what makes the transfer live, and the
        # board judges a live transfer with a count of zero to be one that has
        # already finished.  A count left at zero by the transfer before would
        # therefore be declared complete on the spot, before the command had
        # even gone out - which is a hang, not an error, because the data
        # phase then never gets serviced.
        self.ww(FIFO_COUNT, count)
        got = self.rw(FIFO_COUNT)
        if got != count:
            # The board refuses this write during a DATA phase, and silently.
            # A harness that let that pass would arm a transfer of zero bytes
            # and then wait for it, which is a hang and not a result.
            raise Timeout(f"{self.name}: the FIFO count would not take "
                          f"0x{count:04x} (reads 0x{got:04x})\n"
                          f"        {self.dump()}")
        self.ww(UDC_ADDR, UDC_ADR_CAR_HIGH)
        self.ww(UDC_DATA, ((CHAIN_DVMA >> 8) & 0xFF00) | 0x40)
        self.ww(UDC_ADDR, UDC_ADR_CAR_LOW)
        self.ww(UDC_DATA, CHAIN_DVMA & 0xFFFF)
        self.ww(UDC_ADDR, UDC_ADR_COMMAND)
        self.ww(UDC_DATA, UDC_CMD_STRT_CHN)
        self.ww(CSR, CSR_SCSI_RES | CSR_FIFO_RES | CSR_INTR_EN
                | (CSR_SEND if send else 0))

    def dma_out(self):
        """Hand a DATA OUT phase to the UDC.

        The mirror of dma_in, with two differences that matter: the board has
        to be told the direction so that it reads memory rather than writing
        it, and the chip needs ASSERT DATA BUS, because as an initiator it only
        drives the data lines when the phase matches and it has been told to
        (p. 12)."""
        self.wb(R_TCR, PH_DATA_OUT)
        self.wb(R_ICR, ICR_DATA)
        self.wb(R_MR, MR_DMA | MR_MON_BSY)
        self.wb(R_SDS, 0)               # write register 5
        self.until(CSR, CSR_DMA_ACTIVE, 0, "DMA ACTIVE never cleared",
                   wide=True)
        self.wb(R_MR, 0)
        self.wb(R_ICR, 0)

    def dma_in(self):
        """Hand the data phase to the UDC and wait for it to stop asking."""
        self.wb(R_TCR, PH_DATA_IN)
        self.wb(R_ICR, 0)
        self.wb(R_MR, MR_DMA | MR_MON_BSY)
        self.wb(R_SDIR, 0)              # write register 7
        # SunOS waits for exactly this: si_cmdwait polls the CSR for DMA
        # ACTIVE to clear, and gives up with "DMA_ACTIVE still on".
        self.until(CSR, CSR_DMA_ACTIVE, 0, "DMA ACTIVE never cleared",
                   wide=True)
        self.wb(R_MR, 0)
        self.wb(R_ICR, 0)

    def wait_phase(self):
        """Wait for the target to ask for something, and say what for.

        A driver dispatches on the phase rather than assuming one, and here
        that is not pedantry: a command can go straight from COMMAND to STATUS
        with no data at all, which is exactly what a CHECK CONDITION does."""
        csb = self.until(R_CSB, CSB_REQ, CSB_REQ, "the target never asked")
        return (csb & CSB_PHASE) >> 2

    def step(self, what):
        if os.environ.get("WISH_DIFF_STEPS"):
            print(f"      [{self.name}] {what}", flush=True)

    def command(self, cdb, dma_count=None, send=False, data=None):
        """One command, dispatched on the phase the target asks for, the way
        si.c's phase loop does it."""
        self.step(f"select for cdb {cdb.hex()}")
        self.select(0)
        self.step("message out")
        self.pio_out(PH_MSG_OUT, bytes([0x80]))     # IDENTIFY, no disconnect
        if dma_count:
            self.step("arm udc")
            if data is not None:
                self.qt.write_mem(PHYS_BASE + 0x2000, data)
            # The FIFO count has to be set from a phase that is not a DATA
            # phase, so it goes in here, while the target is still in COMMAND.
            self.arm_udc(dma_count, send=send)
        self.step("command out")
        self.pio_out(PH_COMMAND, cdb)

        sense = None
        did_data = False
        while True:
            ph = self.wait_phase()
            self.step(f"phase {ph}")
            if ph == PH_DATA_OUT and dma_count and send:
                if did_data:
                    raise Timeout(f"{self.name}: the target is still in DATA "
                                  f"OUT after the transfer ended\n"
                                  f"        {self.dump()}")
                did_data = True
                self.dma_out()
            elif ph == PH_DATA_IN and dma_count:
                if did_data:
                    # The target still wants to give us data after the DMA has
                    # said it is finished.  Spinning here is how this used to
                    # hang; saying so is more use.
                    raise Timeout(f"{self.name}: the target is still in DATA "
                                  f"IN after the transfer ended\n"
                                  f"        {self.dump()}")
                did_data = True
                self.dma_in()
            elif ph == PH_DATA_IN:
                sense = self.pio_in(PH_DATA_IN, 18)
            elif ph == PH_STATUS:
                break
            else:
                raise Timeout(f"{self.name}: unexpected phase {ph}\n"
                              f"        {self.dump()}")
        status = self.pio_in(PH_STATUS, 1)[0]
        self.step(f"status 0x{status:02x}")
        message = self.pio_in(PH_MSG_IN, 1)[0]
        self.step(f"message 0x{message:02x}")
        self.until(R_CSB, CSB_BSY, 0, "the bus never went free")
        self.step("bus free")
        # The residual is what a driver reads to find out how much of a
        # transfer really happened, and it is the same number on both sides
        # whatever the two targets put in the buffer.
        residual = self.rw(FIFO_COUNT) if dma_count else 0
        # And what the board and the chip are left holding.  "Lost interrupt"
        # is the whole shape of the fault this is hunting, so the interrupt
        # latch and the two status bits the driver reads it through are part
        # of what a command's outcome means.
        state = (self.rb(R_BSR) & (BSR_END_DMA | BSR_IRQ | BSR_PHASE_MATCH),
                 self.rw(CSR) & (CSR_DMA_ACTIVE | CSR_DMA_BUS_ERR
                                 | CSR_FIFO_EMPTY | CSR_SBC_IP | 0x0100))
        return status, message, sense, residual, state

    def settle(self, tries=4):
        """Get the device past whatever it wants to complain about first.

        QEMU's scsi-disk reports a power-on UNIT ATTENTION on the first
        command after a reset, exactly as a real disk does and as SCSI
        requires; the RTL's scsi_targ does not implement unit attention at all
        (doc/target.md lists what it deliberately leaves out).  That is a
        difference between the two *targets* and not between the two chips, so
        it is drained here rather than compared - a TEST UNIT READY, and a
        REQUEST SENSE to clear the condition, until the device is happy.

        Every driver does this at probe time for the same reason."""
        for _ in range(tries):
            status, _, _, _, _ = self.command(bytes(6))     # TEST UNIT READY
            if status == 0:
                return
            self.command(bytes([0x03, 0, 0, 0, 18, 0]))     # REQUEST SENSE
        raise Timeout(f"{self.name}: the device never came ready")

    def read6(self, lba, blocks, count):
        return self.command(bytes([0x08, (lba >> 16) & 0x1f,
                                   (lba >> 8) & 0xff, lba & 0xff,
                                   blocks & 0xff, 0]), dma_count=count)

    def write6(self, lba, blocks, data):
        return self.command(bytes([0x0a, (lba >> 16) & 0x1f,
                                   (lba >> 8) & 0xff, lba & 0xff,
                                   blocks & 0xff, 0]),
                            dma_count=len(data), send=True, data=data)

    def inquiry(self, alloc):
        """Ask for more than a SCSI-1 disk will give.

        SunOS asks for 56 bytes and gets the standard 36, so the transfer ends
        on a phase change rather than at terminal count - which is the case the
        board's end-of-transfer rule exists for, and the one NetBSD never
        produces because it only ever asks for what it will get."""
        return self.command(bytes([0x12, 0, 0, 0, alloc, 0]),
                            dma_count=alloc)


class Model:
    """What the disk ought to contain.

    Reads are compared against this and not only against each other, because
    two models agreeing on the wrong bytes is a failure that agreeing alone
    cannot catch.  It starts as the file and takes every write this harness
    issues, so it stays right across the whole sequence."""

    def __init__(self, path):
        self.data = bytearray(open(path, "rb").read())

    def read(self, lba, blocks):
        return bytes(self.data[lba * 512:(lba + blocks) * 512])

    def write(self, lba, data):
        self.data[lba * 512:lba * 512 + len(data)] = data


def pattern(lba, blocks, salt):
    """Something to write that says where it was meant to go."""
    out = bytearray()
    for b in range(blocks):
        blk = bytearray(512)
        blk[0:4] = ((lba + b) ^ salt).to_bytes(4, "big")
        blk[4:8] = salt.to_bytes(4, "big")
        for i in range(8, 512):
            blk[i] = ((lba + b) * 13 + i * 7 + salt) & 0xFF
        out += blk
    return bytes(out)


# The mixture SunOS actually produces, which is what a single clean read never
# reproduced: reads and writes interleaved, sizes that are and are not the
# 8192 bytes the fault happens on, a transfer the target ends short, and a
# command that fails and has to be recovered from.
SEQUENCE = [
    ("tur",),
    ("read", 0, 16),
    ("read", 1, 1),
    ("write", 100, 16),
    ("read", 100, 16),
    ("inquiry", 56),
    ("write", 101, 2),
    ("read", 100, 17),
    ("read", 500, 16),
    ("write", 500, 16),
    ("read", 500, 16),
    ("read", 2, 4),
    ("write", 2, 4),
    ("inquiry", 36),
    ("read", 0, 16),
    ("write", 1000, 16),
    ("read", 1000, 16),
    ("read", 999, 18),
    ("read", 2040, 16),         # off the end: CHECK CONDITION from both
    ("tur",),
    ("read", 137, 16),
]


def quiet_transfer(sw, rtl, model, report, lba=300, blocks=16):
    """A transfer nobody is watching.

    Everything else here polls the CSR until the transfer ends, and polling is
    a register access, and a register access is what wakes the board up.  That
    hides an entire class of fault by construction - two of the three bugs
    found on the way here were missed wake-ups, where a transfer only made
    progress because the driver happened to touch something.

    A real driver does not poll.  SunOS arms the transfer and sleeps until the
    interrupt, which is the case this reproduces: arm it, then advance the
    clock without touching the board at all, and see whether it finished by
    itself.  The two cores get there differently - the Verilated one from its
    500 us pacing timer, the software one from the chip's DRQ and IRQ pins -
    and this is the only place that difference is visible.
    """
    count = blocks * 512
    ok = True
    got = []
    for side in (sw, rtl):
        side.qt.write_mem(PHYS_BASE + 0x2000, b"\x77" * count)
        side.select(0)
        side.pio_out(PH_MSG_OUT, bytes([0x80]))
        side.arm_udc(count, send=False)
        side.pio_out(PH_COMMAND, bytes([0x08, (lba >> 16) & 0x1f,
                                        (lba >> 8) & 0xff, lba & 0xff,
                                        blocks & 0xff, 0]))
        side.until(R_CSB, CSB_REQ, CSB_REQ, "the target never asked")
        side.wb(R_TCR, PH_DATA_IN)
        side.wb(R_ICR, 0)
        side.wb(R_MR, MR_DMA | MR_MON_BSY)
        side.wb(R_SDIR, 0)

        # From here nothing touches the board.  Give it a second of virtual
        # time in ten pieces, and let it finish on its own.
        for _ in range(10):
            side.qt.clock_step(100 * 1000 * 1000)

        csr = side.rw(CSR)
        got.append(csr & CSR_DMA_ACTIVE)
        if csr & CSR_DMA_ACTIVE:
            report.append(f"quiet transfer: {side.name} was still running "
                          f"after a second of virtual time with nobody "
                          f"polling it\n        {side.dump()}")
            ok = False
        # Put the bus back, however it went.
        side.wb(R_MR, 0)
        side.wb(R_ICR, 0)
        try:
            while side.wait_phase() != PH_STATUS:
                pass
            side.pio_in(PH_STATUS, 1)
            side.pio_in(PH_MSG_IN, 1)
            side.until(R_CSB, CSB_BSY, 0, "the bus never went free")
        except Timeout as e:
            report.append(f"quiet transfer: {e}")
            ok = False

    if ok:
        data = [side.qt.read_mem(PHYS_BASE + 0x2000, count)
                for side in (sw, rtl)]
        want = model.read(lba, blocks)
        for name, d in (("software", data[0]), ("Verilated", data[1])):
            if d != want:
                j = next(k for k in range(count) if d[k] != want[k])
                report.append(f"quiet transfer: {name} read 0x{d[j]:02x} at "
                              f"byte {j}, disk holds 0x{want[j]:02x}")
                ok = False
    return ok


def run_sequence(sw, rtl, model, report, step=None):
    """One script, both machines, compared after every command."""
    salt = 0
    for n, op in enumerate(SEQUENCE):
        kind = op[0]
        salt += 1
        what = f"{n:2d} {kind} {' '.join(str(x) for x in op[1:])}"
        if step:
            print(f"      {what}", flush=True)

        if kind == "tur":
            res = [side.command(bytes(6)) for side in (sw, rtl)]
            got = [(r[0], r[1], r[4]) for r in res]
            data = [None, None]
        elif kind == "inquiry":
            alloc = op[1]
            res = [side.inquiry(alloc) for side in (sw, rtl)]
            # Status and message only, and the residual deliberately not.
            #
            # Asked for 56 bytes, scsi_targ.sv hands over the 36 an INQUIRY
            # has and leaves 20 in the count; QEMU's scsi-disk pads to the
            # allocation length and leaves none.  The RTL is right - SCSI says
            # the target transfers the lesser of the allocation length and
            # what it has - but both are targets, and this harness compares
            # chips.
            #
            # It costs something worth naming: the short-transfer path, where
            # a transfer ends on a phase change rather than at terminal count,
            # is the one SunOS produces and NetBSD never does, and it can only
            # be exercised on the Verilated side.  The out-of-range read below
            # is the error case both targets do produce identically.
            got = [(r[0], r[1], r[4]) for r in res]
            data = [None, None]
        elif kind == "read":
            lba, blocks = op[1], op[2]
            data = []
            got = []
            for side in (sw, rtl):
                side.qt.write_mem(PHYS_BASE + 0x2000, b"\xee" * (blocks * 512))
                r = side.read6(lba, blocks, blocks * 512)
                got.append((r[0], r[1], r[3], r[4]))
                data.append(side.qt.read_mem(PHYS_BASE + 0x2000, blocks * 512))
            if got[0][0] != 0 or got[1][0] != 0:
                # A command that failed moved nothing, so there is nothing to
                # compare but the failure itself.
                data = [None, None]
        elif kind == "write":
            lba, blocks = op[1], op[2]
            buf = pattern(lba, blocks, salt)
            res = [side.write6(lba, blocks, buf) for side in (sw, rtl)]
            got = [(r[0], r[1], r[3], r[4]) for r in res]
            data = [None, None]
            if got[0] == got[1] and got[0][0] == 0:
                model.write(lba, buf)
        else:
            raise RuntimeError(kind)

        if got[0] != got[1]:
            report.append(f"{what}: software {got[0]}, Verilated {got[1]}")
            return False
        if data[0] is not None:
            want = model.read(op[1], op[2])
            if data[0] != data[1]:
                j = next(k for k in range(len(data[0]))
                         if data[0][k] != data[1][k])
                report.append(f"{what}: data differs at byte {j}, software "
                              f"0x{data[0][j]:02x}, Verilated "
                              f"0x{data[1][j]:02x}")
                return False
            if data[0] != want:
                j = next(k for k in range(len(want)) if data[0][k] != want[k])
                report.append(f"{what}: both read 0x{data[0][j]:02x} at byte "
                              f"{j} where the disk should hold "
                              f"0x{want[j]:02x}")
                return False
    return True


def running_transfer(sw, rtl, image, lba, blocks, report):
    """The same READ(6) on both, and the file as the third opinion."""
    count = blocks * 512
    with open(image, "rb") as f:
        f.seek(lba * 512)
        want = f.read(count)

    got = {}
    for side in (sw, rtl):
        # Poison the buffer first, so that a transfer which moves nothing
        # cannot pass by leaving the previous run's bytes lying there.
        side.qt.write_mem(PHYS_BASE + 0x2000, b"\xee" * count)
        before = side.accesses
        status, message, _, _, _ = side.read6(lba, blocks, count)
        data = side.qt.read_mem(PHYS_BASE + 0x2000, count)
        got[side.name] = (status, message, data, side.accesses - before)

    ok = True
    s_st, s_msg, s_data, s_n = got["sw"]
    r_st, r_msg, r_data, r_n = got["rtl"]

    def note(what, a, b):
        nonlocal ok
        ok = False
        report.append(f"{what}: software {a}, Verilated {b}")

    if s_st != r_st:
        note(f"READ(6) lba {lba} status", f"0x{s_st:02x}", f"0x{r_st:02x}")
    if s_msg != r_msg:
        note(f"READ(6) lba {lba} message", f"0x{s_msg:02x}", f"0x{r_msg:02x}")
    if s_data != r_data:
        i = next(j for j in range(count) if s_data[j] != r_data[j])
        note(f"READ(6) lba {lba} data at byte {i}",
             f"0x{s_data[i]:02x}", f"0x{r_data[i]:02x}")
    # And the file, which neither of them gets a vote on.
    for name, data in (("software", s_data), ("Verilated", r_data)):
        if data != want:
            i = next(j for j in range(count) if data[j] != want[j])
            ok = False
            report.append(f"READ(6) lba {lba}: {name} read 0x{data[i]:02x} at "
                          f"byte {i} where the file has 0x{want[i]:02x}")
    return ok, s_st, s_n, r_n


def make_image(path, blocks):
    """A disk whose every block says which block it is, so a transfer that
    lands at the wrong offset is obvious rather than merely wrong."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        for lba in range(blocks):
            b = bytearray(512)
            b[0:4] = lba.to_bytes(4, "big")
            for i in range(4, 512):
                b[i] = (lba * 7 + i * 31) & 0xFF
            f.write(bytes(b))


def start(qemu, prom, core, rtl, image=None):
    machine = f"sun3,si-core={core}"
    if core == "rtl":
        machine += f",si-rtl={rtl}"
        if image:
            machine += f",si-image={image}"
    cmd = [qemu, "-M", machine, "-m", "4M", "-bios", prom,
           "-accel", "qtest", "-qtest", "stdio",
           "-display", "none", "-serial", "none", "-monitor", "none"]
    extra = os.environ.get("WISH_DIFF_TRACE")
    if extra:
        for ev in extra.split(","):
            cmd += ["-trace", ev]
    if core == "sw" and image:
        # The machine's default drive type is IF_SCSI, so this lands on the
        # chip's bus at target 0 without being told to - the same ID the
        # Verilated card's own scsi_targ sits at.
        cmd += ["-drive", f"file={image},format=raw"]
    err = subprocess.DEVNULL
    if extra:
        err = open(f"/tmp/wish-diff-{core}.trace", "w")
    return subprocess.Popen(cmd, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE,
                            stderr=err, text=True, bufsize=1)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qemu", default=str(QEMU))
    ap.add_argument("--rtl", default=str(LIB))
    ap.add_argument("--prom", default=str(PROM))
    ap.add_argument("--image", default=str(WORK / "images" / "diff-disk.img"),
                    help="the disk both sides read, made if it is not there")
    ap.add_argument("--blocks", type=int, default=16,
                    help="blocks per READ(6): 16 is the 8192-byte transfer the "
                         "open fault happens on")
    ap.add_argument("--lbas", type=lambda v: [int(x) for x in v.split(",")],
                    default=[0, 1, 137],
                    help="which blocks to read, comma separated")
    ap.add_argument("--only", choices=("sw", "rtl"),
                    help="drive one side only, to find which of them "
                         "a hang belongs to")
    ap.add_argument("--tries", type=int, default=3000,
                    help="how many polls before a phase is called stuck")
    ap.add_argument("--no-seq", action="store_true",
                    help="skip the interleaved read/write sequence")
    ap.add_argument("--no-xfer", action="store_true",
                    help="skip the running transfer and its disk")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="print every access, so that a check which passes "
                         "because nothing happened is visible as one")
    args = ap.parse_args()

    for what, path in (("QEMU", args.qemu), ("RTL library", args.rtl),
                       ("PROM", args.prom)):
        if not os.path.exists(path):
            print(f"missing {what}: {path}", file=sys.stderr)
            return 2

    image = sw_img = rtl_img = None
    if not args.no_xfer:
        image = pathlib.Path(args.image)
        if not image.exists():
            print(f"making {image}")
            make_image(image, 2048)
        # A copy each.  The software side writes through QEMU's block layer
        # and the Verilated side into an SD card model it only flushes when it
        # exits, so one file between them would be two writers and a race.
        sw_img = image.with_name(image.stem + "-sw" + image.suffix)
        rtl_img = image.with_name(image.stem + "-rtl" + image.suffix)
        for dst in (sw_img, rtl_img):
            dst.write_bytes(image.read_bytes())

    sw_p = start(args.qemu, args.prom, "sw", args.rtl, sw_img)
    rtl_p = start(args.qemu, args.prom, "rtl", args.rtl, rtl_img)
    sw, rtl = Qtest(sw_p, "sw"), Qtest(rtl_p, "rtl")
    p = Pair(sw, rtl)
    xfer = []

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

        if image:
            sw_side = Side(sw, "sw", args.tries)
            rtl_side = Side(rtl, "rtl", args.tries)
            sides = [sw_side, rtl_side]
            if args.only:
                sides = [s for s in sides if s.name == args.only]
            for side in sides:
                side.board_reset()
                side.settle()
            for lba in args.lbas:
                try:
                    if args.only:
                        side = sides[0]
                        side.qt.write_mem(PHYS_BASE + 0x2000,
                                          b"\xee" * (args.blocks * 512))
                        st, msg, _, _, _ = side.read6(lba, args.blocks,
                                                args.blocks * 512)
                        print(f"  {side.name} lba {lba}: status 0x{st:02x} "
                              f"message 0x{msg:02x}", flush=True)
                        continue
                    ok, status, n_sw, n_rtl = running_transfer(
                        sw_side, rtl_side, image, lba, args.blocks, xfer)
                    print(f"  {'ok  ' if ok else 'FAIL'}  "
                          f"running_transfer lba {lba}, "
                          f"{args.blocks * 512} bytes"
                          + (f"   status 0x{status:02x}, "
                             f"{n_sw} accesses sw / {n_rtl} rtl"
                             if ok else ""))
                except Timeout as e:
                    print(f"  FAIL  running_transfer lba {lba}: {e}")
                    xfer.append(str(e))

            if not args.no_seq:
                model = Model(image)
                try:
                    ok = run_sequence(sw_side, rtl_side, model, xfer,
                                      os.environ.get("WISH_DIFF_STEPS"))
                    print(f"  {'ok  ' if ok else 'FAIL'}  sequence "
                          f"({len(SEQUENCE)} commands, reads and writes "
                          f"interleaved)", flush=True)
                except Timeout as e:
                    print(f"  FAIL  sequence: {e}", flush=True)
                    xfer.append(str(e))

                try:
                    ok = quiet_transfer(sw_side, rtl_side, model, xfer)
                    print(f"  {'ok  ' if ok else 'FAIL'}  quiet_transfer "
                          f"(8192 bytes, nobody polling)", flush=True)
                except Timeout as e:
                    print(f"  FAIL  quiet_transfer: {e}", flush=True)
                    xfer.append(str(e))
    finally:
        for proc in (sw_p, rtl_p):
            try:
                proc.stdin.close()
            except BrokenPipeError:
                pass
            proc.terminate()
            proc.wait(timeout=30)

    print(f"\n{p.reads} reads compared")
    if xfer:
        print("\nthe running transfer:")
        for line in xfer:
            print(f"  {line}")
        return 1
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

# Drivers for the real chip

These are unmodified copies of drivers for the NCR 5380, kept as reference
material.  They keep their original licences, which are in the file headers;
nothing in `src/` or `tb/` is derived from them.  They are here to be read.

| directory       | what it is                                                   |
|-----------------|--------------------------------------------------------------|
| `Linux/`        | `drivers/scsi/NCR5380.[ch]`, the shared core, plus the Mac, Atari, Sun 3 and generic-ISA board drivers that sit on it |
| `NetBSD/`       | `sys/dev/ic/ncr5380sbc.c` and its headers, the core shared by every NetBSD 5380 port, plus the Mac and Sun 3 attachments |
| `NetBSD-atari/` | NetBSD/atari's own `ncr5380.c`, a second, independent driver by a different author |

## Which source is the authority on what

**`doc/NCR5380_design_manual_Mar86.pdf` is the authority on bit positions and
register behaviour.**  Every bit of every register is named and described:
the register summary on p. 10, Initiator Command on pp. 11-12, Mode on p. 13,
Target Command on p. 14, Current SCSI Bus Status and Bus and Status on
pp. 15-16, the DMA strobes on pp. 16-17, and the register reference chart in
appendix A7 on p. 58.  It wins any disagreement.

The scan is clean and complete, which is the comfortable case.  The sibling
project Wish82586 had no datasheet at all and had to nominate a driver header
as the authority; do not carry that habit across.

**The drivers are the authority on sequencing.**  What order a driver writes
the registers in, how long it is prepared to wait, which bits it polls, and
what it assumes the chip does between two accesses are things the datasheet
does not say and the drivers do.

| what                                                       | where                              |
|------------------------------------------------------------|------------------------------------|
| arbitration: `MR_ARBITRATE`, wait for AIP, then `udelay(3)` for the 2.4 µs arbitration delay before LOST ARBITRATION is believed | `Linux/NCR5380.c` lines 997-1040 |
| the 1.2 µs bus clear plus bus settle it then waits, rounded up to `udelay(2)` | `Linux/NCR5380.c` lines 1042-1057 |
| the same sequence written by a different author             | `NetBSD/ncr5380sbc.c`, `ncr5380_sched` |
| selection, and what it does when the target never answers   | `Linux/NCR5380.c`, `NCR5380_select` |
| the REQ/ACK handshake in programmed I/O                     | `Linux/NCR5380.c`, `NCR5380_transfer_pio` |
| pseudo-DMA, and what it polls before each chunk             | `Linux/mac_scsi.c`, `macscsi_wait_for_drq` |
| reselection, and matching the target back to its command    | `Linux/NCR5380.c`, `NCR5380_reselect` |
| the interrupt handler's order of tests                      | `Linux/NCR5380.c`, `NCR5380_intr`   |
| bus reset, and what the driver assumes it clears            | `Linux/NCR5380.c`, `NCR5380_maybe_reset_bus` |

Two independent driver families are kept on purpose.  Linux's `NCR5380.c` and
NetBSD's `ncr5380sbc.c` share no code and were written from the databook by
different people, so where they agree on a sequence, the sequence is the
chip's and not one author's habit.  `NetBSD-atari/ncr5380.c` is a third,
written by Leo Weppelman, and is useful for the same reason.

## The two driver headers agree with the datasheet, and with each other

`NetBSD/ncr5380reg.h` and `Linux/NCR5380.h` are independent transcriptions of
the same databook - the NetBSD one traces back through Mach to a 1991 reading
by Alessandro Forin at CMU, the Linux one to Drew Eckhardt in 1993 - and they
agree with the datasheet and with each other on **every bit of every
register**.  `tb/cpp/tests/test_layout.cpp` transcribes both a third time and
checks all three.

That is worth saying explicitly because the sibling project Wish7990 found a
disagreement in the equivalent place and had to record which side won.  Here
there is nothing to arbitrate.  If a future reading turns one up, it goes in
this section rather than being quietly resolved in favour of whichever file
was open at the time.

The only differences are of vocabulary, and each is a place where a driver
author named a bit for what it meant to them rather than for what the
datasheet calls it:

| datasheet (p. 16)         | NetBSD                  | Linux                    | note |
|---------------------------|-------------------------|--------------------------|------|
| BUSY ERROR                | `SCI_CSR_DISC`          | `BASR_BUSY_ERROR`        | NetBSD names it for what an unexpected loss of BSY means to an initiator: the target disconnected |
| END OF DMA TRANSFER       | `SCI_CSR_DONE`          | `BASR_END_DMA_TRANSFER`  | |
| ENABLE EOP INTERRUPT      | `SCI_MODE_DMA_IE`       | `MR_ENABLE_EOP_INTR`     | NetBSD names it for the DMA completion it usually signals |
| TEST MODE (ICR bit 6, w)  | `SCI_ICMD_TEST`         | `ICR_TRI_STATE`          | Linux names it for its effect: all output drivers float |
| LOST ARBITRATION          | `SCI_ICMD_LST`          | `ICR_ARBITRATION_LOST`   | |

## Two places a driver comment disagrees with the datasheet

Neither changes behaviour, and both are worth knowing before someone "fixes"
the RTL to match a comment:

* `Linux/NCR5380.c` says *"The chip now waits for BUS FREE phase.  Then after
  the 800 ns Bus Free Delay, arbitration will begin."*  800 ns is the SCSI
  specification's bus free delay; the datasheet says this chip's filter is
  **400 ns** - *"If BSY remains inactive for at least 400 nsec then the SCSI
  bus is considered free"* (p. 18).  The chip is faster than the standard
  requires, which is safe, and the RTL follows the datasheet.
* the same file says *"The SCSI-2 arbitration delay is 2.4 us"* and waits
  `udelay(3)`; the datasheet says 2.2 µs (p. 18).  Both are the driver's own
  wait - the chip does not implement the delay at all - so the difference
  never reaches hardware.

## Initiator Command does not read back what was written

Bits 6 and 5 of register 1 are different registers on read and on write
(p. 12): AIP and LOST ARBITRATION come back, TEST MODE and DIFF ENBL go in.
A read/modify/write of this register therefore does not preserve what was
there, and both driver families work around it in their own way:

* NetBSD masks with `SCI_ICMD_RMASK`, defined as `0x1f` with the comment
  *"Bits to keep when doing read/modify/write (leave out RST)"*;
* Linux never reads the register to modify it, keeping what it last wrote.

Two independent workarounds for the same fault is about as strong a signal as
reference software can give that the fault is real, so the replica reproduces
it rather than fixing it.  `reg_initiator_command_reads_arbitration_not_what_was_written`
pins it.

## The 53C80 is the CMOS part, and adds one bit

Appendix A5 (p. 54) describes the NCR 53C80: functionally equivalent, not pin
compatible, and different in four ways.  Only one of them is visible to
software - **bit 7 of the Target Command Register becomes LAST BYTE SENT**,
a true end-of-DMA status for send operations that the NMOS part cannot
provide (p. 20).  Both driver headers carry it, NetBSD's with the comment
`(not on 5380/1)`.

The others are a pull-up on RST to stop spurious interrupts on an unterminated
bus, faster REQ/ACK paths, and a refusal to assert ACK again after EOP without
being told to.  None changes a register.

## Which machines used it, and what that means here

The 5380 has no address counter and no byte counter: everything above a single
REQ/ACK handshake is the driver's or the board's job.  So the machine glue
around it varies more than the chip does, and the glue is where a replica has
to make choices.  `doc/interface.md` records the ones this design makes.

| machine            | driver here                                  | glue |
|--------------------|----------------------------------------------|------|
| Mac Plus / SE / II | `Linux/mac_scsi.c`, `NetBSD/sbc_obio.c`      | registers 16 bytes apart; two extra pseudo-DMA apertures |
| Atari TT / Falcon  | `Linux/atari_scsi.c`, `NetBSD-atari/ncr5380.c` | a real DMA chip in front of the 5380 |
| Sun 3/50, 3/60     | `Linux/sun3_scsi.c`, `NetBSD/si.c`           | a VME or on-board DMA engine |
| generic ISA card   | `Linux/g_NCR5380.c`                          | plain port-mapped registers, one byte apart |

The Mac is this project's first target, so `mac_scsi.c` and `sbc_obio.c` are
the two to read first.

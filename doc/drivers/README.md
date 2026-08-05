# Drivers for the real chip

These are unmodified copies of drivers for the NCR 5380, kept as reference
material.  They keep their original licences, which are in the file headers;
nothing in `src/` or `tb/` is derived from them.  They are here to be read.

| directory       | what it is                                                   |
|-----------------|--------------------------------------------------------------|
| `Linux/`        | `drivers/scsi/NCR5380.[ch]`, the shared core, plus the Mac, Atari, Sun 3 and generic-ISA board drivers that sit on it |
| `NetBSD/`       | `sys/dev/ic/ncr5380sbc.c` and its headers, the core shared by every NetBSD 5380 port, plus the Mac and Sun 3 attachments |
| `NetBSD-atari/` | NetBSD/atari's own `ncr5380.c`, a second, independent driver by a different author |
| `SunOS34/`      | Sun's own `si` driver for the Sun-3 board, in its three flavours: the kernel's (`sundev/`), the standalone one the boot programs link (`sunstand/`), and the boot monitor's (`mon3/`) |
| `SunOS412/`     | the same driver five years later, from SunOS 4.1.2.  Kept for one reason: it is the version whose diagnostics the co-simulation prints, and 3.4's has neither of them |

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

## The three driver headers agree with the datasheet, and with each other

`NetBSD/ncr5380reg.h`, `Linux/NCR5380.h` and `SunOS34/sundev/sireg.h` are
independent transcriptions of the same databook - the NetBSD one traces back
through Mach to a 1991 reading by Alessandro Forin at CMU, the Linux one to
Drew Eckhardt in 1993, and Sun's is the oldest of the three at 1986, written
by the people who put the part on a board - and they agree with the datasheet
and with each other on **every bit of every register**.
`tb/cpp/tests/test_layout.cpp` transcribes all three a fourth time and checks
them.

That is worth saying explicitly because the sibling project Wish7990 found a
disagreement in the equivalent place and had to record which side won.  Here
there is nothing to arbitrate.  If a future reading turns one up, it goes in
this section rather than being quietly resolved in favour of whichever file
was open at the time.

The only differences are of vocabulary, and each is a place where a driver
author named a bit for what it meant to them rather than for what the
datasheet calls it:

| datasheet (p. 16)         | NetBSD                  | Linux                    | SunOS            | note |
|---------------------------|-------------------------|--------------------------|------------------|------|
| BUSY ERROR                | `SCI_CSR_DISC`          | `BASR_BUSY_ERROR`        | `SBC_BSR_BERR`   | NetBSD names it for what an unexpected loss of BSY means to an initiator: the target disconnected |
| END OF DMA TRANSFER       | `SCI_CSR_DONE`          | `BASR_END_DMA_TRANSFER`  | `SBC_BSR_EDMA`   | |
| ENABLE EOP INTERRUPT      | `SCI_MODE_DMA_IE`       | `MR_ENABLE_EOP_INTR`     | `SBC_MR_EEI`     | NetBSD names it for the DMA completion it usually signals |
| TEST MODE (ICR bit 6, w)  | `SCI_ICMD_TEST`         | `ICR_TRI_STATE`          | `SBC_ICR_TEST`   | Linux names it for its effect: all output drivers float |
| LOST ARBITRATION          | `SCI_ICMD_LST`          | `ICR_ARBITRATION_LOST`   | `SBC_ICR_LA`     | |

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
* Linux never reads the register to modify it, keeping what it last wrote;
* SunOS reads it and masks off the one bit it has just been looking at -
  `icr = *icrp & ~SBC_ICR_AIP` in `si_arb_sel`, three times over
  (`SunOS34/sundev/si.c` lines 1084, 1086, 1108).

Three independent workarounds for the same fault is about as strong a signal
as reference software can give that the fault is real, so the replica
reproduces it rather than fixing it.
`reg_initiator_command_reads_arbitration_not_what_was_written` pins it.

Sun's is the leakiest of the three: it clears AIP but not LOST ARBITRATION,
so a write-back after a lost arbitration would set DIFF ENBL.  That is
harmless on a 5380, where the bit does nothing, and the path is not reached
anyway - but it is the sort of thing a replica must not tidy up, because a
part that ignored the write is what made the driver survive it.

## What Sun's own driver settles

`SunOS34/` is the third family, and it is a different kind of witness from the
other two: it is not someone else's reading of the databook but the code of
the company that designed the board, for the machine this project's
co-simulation actually boots.  It comes in three flavours, and they are worth
telling apart:

| file                   | what it is |
|------------------------|------------|
| `sundev/si.c`          | the kernel driver: arbitration, selection, disconnect/reselect, DMA through the Am9516, and an interrupt handler that has to tell four sources apart |
| `sundev/sireg.h`       | Sun's register map for both the 5380 and the board around it |
| `sundev/scsi.h`        | the command and sense structures the driver sends, which is what `doc/target.md` has to answer |
| `sunstand/si.c`        | the standalone driver the boot programs link: the same board, polled, with no interrupts and no disconnect |
| `mon3/si.c`, `mon3/sireg.h` | the boot monitor's own copy, from 1986.  The PROM the co-simulation runs is Rev 3.0.1 and later than this, but it is the same lineage, and it is the closest thing there is to source for the code that prints `Boot: sd(0,0,0)` |

**NetBSD's `sireg.h` is not independent of Sun's.**  It carries Sun's
paragraph about which registers belong to which interface word for word, and
the two define the same `SI_CSR_*` names with the same values.  For the *board*
there is therefore one witness and not two - which is worth knowing before
citing "both drivers agree" about a board register.  For the *chip* the
independence is real: NetBSD reaches the 5380 through `ncr5380reg.h`, which
has nothing to do with Sun.

What the kernel driver settles that the other two families do not:

* **The byte count register is written before the target enters DATA phase**
  and not after - `si_cmd` does it with the comment *"must init bcr before
  tgt goes into data phase"* (line 857).  NetBSD's `si_obio.c` writes it in
  the same window and reads it back to check.  The board's rule that a write
  to it is ignored once DATA phase has started is therefore something both
  drivers were written around, and a model that accepted the late write would
  let a driver pass that the hardware would have failed.
* **The Mode Register's DMA bit is the interrupt enable**, and it is set only
  with BSY already true - after selection in `si_sbc_dma_setup` (line 1317),
  and after the target has reselected in `si_recon` (line 2264, *"turn
  interrupts back on"*).  Nothing in either driver sets it while disconnected,
  which is what makes the chip's refusal to take that write (p. 14) safe to
  reproduce.
* **The 2.2 µs arbitration delay is the driver's**, again: `SI_ARBITRATION_DELAY`
  is 3 µs, and `si_arb_sel` waits it out before believing LOST ARBITRATION
  (line 1080).  Three families, three separate delays, none of them expecting
  the chip to do it.
* **The UDC's count register is read back**, not just written: the driver
  works out how many leftover bytes are in the byte-pack register by
  comparing `sir->udc_rdata * 2` against `sir->bcr` (line 1421, and the same
  line in `sunstand/si.c`).  A board model whose UDC registers are write-only
  would look correct until an odd-length transfer.
* **A DMA interrupt is an error.**  `siintr` tests `SI_CSR_DMA_IP` and
  `SI_CSR_DMA_CONFLICT` together, before anything else, and on the on-board
  board prints *"dma ip, unknown reason"* and fails the command (lines 1516 to
  1553).  Both drivers arm the UDC's channel interrupt - Sun's comment says
  *"in case of error"* - so the hardware cannot be raising it at terminal
  count, or SunOS would never have completed a transfer.  This one cost us a
  bug: our board model raised it on every normal completion, which NetBSD
  tolerates and SunOS would have treated as fatal.  `cosim/README.md` records
  the fix.
* One thing it does not settle, but is worth not forgetting: *"It seems that
  the tcr must be 0 for arbitration to work"* (line 1004).  The datasheet does
  not say that, the RTL does not require it, and no other driver mentions it.
  It reads like an empirical note about a real board, so it stays here rather
  than becoming a rule.

## How SunOS arms itself between phases, and why the poll matters

`SunOS412/si.c` is here for one passage.  `siintr` handles **one bus phase per
interrupt** and then leaves through `SET_UP_FOR_NEXT_INTR_AND_LEAVE`, which
arms the chip for whatever the target asks next:

```c
SBC_WR.tcr = TCR_UNSPECIFIED;   /* a phase that is not a phase */
junk = SBC_RD.clr;              /* clear any pending interrupt */
SBC_WR.mr |= SBC_MR_DMA;        /* DMA mode on - this is the arm */
if (si_sbc_wait(&SBC_RD.cbsr, SBC_CBSR_REQ, si_phase_wait, 20, 1) == OK)
        goto SYNCHRONIZE_PHASE; /* already asking - handle it here */
/* else leave, and be interrupted when it does */
```

No transfer is started.  The chip sits in DMA mode looking for a phase it can
never match, so the target's next request is a mismatch and the mismatch is
the interrupt.  Two things about the part have to be right for that to work,
and both are now pinned by tests:

* **the mismatch interrupt is an edge.**  "If the DMA MODE bit is active and a
  phase mismatch occurs when REQ transitions from false to true, an interrupt
  is generated" (p. 22).  A request already standing when DMA mode goes on has
  no transition and produces nothing -
  `dma_mode_does_not_interrupt_for_a_request_that_was_already_there`.
* **Current SCSI Bus Status reports the bus's own REQ**, not one gated by
  phase match - which is what makes the driver's poll able to catch the case
  the edge misses.

The poll is therefore load-bearing rather than belt-and-braces, and a replica
that interrupted on a standing request would let a driver that omitted it
appear to work.  This is also the answer to a question the co-simulation
raised and did not settle: `si0: lost interrupt` is printed by `si_deque`
*after* a request has already timed out, when the CSR shows `SBC_IP` - it is
the driver's explanation for a timeout and not an independent fault.

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
| Sun 3/50, 3/60     | `Linux/sun3_scsi.c`, `NetBSD/si.c`, `SunOS34/sundev/si.c` | a VME or on-board DMA engine |
| generic ISA card   | `Linux/g_NCR5380.c`                          | plain port-mapped registers, one byte apart |

The Mac is this project's first target, so `mac_scsi.c` and `sbc_obio.c` are
the two to read first.

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with
code in this repository.

## What this is

A SystemVerilog SCSI controller that is software-compatible with the NCR 5380,
targeting FPGAs: Wishbone B4 on the host side, an SD card on the storage side.
The point is to let recreated vintage machines run their original, unmodified
drivers against a disk that is really a memory card.

There is **no SCSI cable and no SCSI pads**.  The chip's initiator side and an
SD-backed target sit on a private wired-OR fabric inside the design.  A driver
sees a SCSI bus with a disk on it; a board sees a card slot.

The first target machine is the **Mac Plus / SE**, which sets what the
Wishbone front end looks like and which drivers the testbench models.

### Where answers come from

**`doc/NCR5380_design_manual_Mar86.pdf` is the authority on bit positions and
register behaviour.**  Register summary p. 10, Initiator Command pp. 11-12,
Mode p. 13, Target Command p. 14, Current SCSI Bus Status and Bus and Status
pp. 15-16, DMA strobes pp. 16-17, hardware support p. 18, interrupts pp. 19-22,
resets p. 23, data transfers pp. 24-25, register reference chart p. 58.  The
scan is clean; cite the printed page, which is two less than the PDF page.

**The drivers in `doc/drivers/` are the authority on sequencing** - what order
a driver writes the registers in, how long it waits, which bits it polls, and
what it assumes happens between two accesses.  `doc/drivers/README.md` indexes
the passages that settle specific questions.  Two independent families are
kept on purpose: where Linux's `NCR5380.c` and NetBSD's `ncr5380sbc.c` agree,
the sequence is the chip's and not one author's habit.

**The two driver headers agree with the datasheet and with each other on every
bit.**  That is the comfortable case and is worth knowing, because the sibling
project Wish7990 found a disagreement in the equivalent place.  If a future
reading turns one up here, it goes in `doc/drivers/README.md` rather than
being quietly resolved.

`tb/cpp/tests/test_layout.cpp` transcribes both headers a third time and pins
our constants to them.  It exists before any engine, on purpose: Wish82586
wrote its command unit first and had to redo it when the layout turned out to
be wrong.

## Commands

```sh
make                    # build the testbench
make test               # build and run the whole regression
make test T=reg         # only tests whose name contains "reg"
make test FLAGS=-v      # show test notes and why pending tests failed
make wave T=<test>      # run with tracing -> build/waves/<test>.vcd
make list               # list registered tests
make lint               # Verilator lint, -Wall, must stay clean
make lint-icarus        # Icarus parse check
make synth              # Yosys elaboration check of every module
make clean
```

`make test` must stay green.  Green means 0 failed; pending tests are expected
failures and do not break the build.

Work proceeds test first: pick a pending test, implement the RTL, drop the
marker.  The pending list is the project todo list - prefer adding a pending
test for missing behaviour over leaving it untested.

## Architecture

### The two-sided contract

`doc/interface.md` is the agreement between `src/` and `tb/`.  Anything that
changes the register map, the bus behaviour, the internal SCSI bus or the
block back end has to change the RTL, the testbench and that document
together.

Two files hold the same NCR 5380 constants and must be kept in step by hand:
`src/wish5380_pkg.sv` (RTL) and `tb/cpp/ncr5380.h` (testbench).  They are
deliberately independent - the testbench is not allowed to derive its
expectations from the RTL.

### RTL (`src/`)

```
wish5380_wb                  Wishbone B4 slave, one clock, irq_o
├── wb_5380                  machine glue: apertures, byte lanes, pseudo-DMA
├── wish5380                 the part
│   ├── sci_regs             the eight registers and the port they hide behind
│   └── sci_bus              arbitration, selection, handshake, interrupts
├── scsi_fabric              the wired-OR joining everything on the bus
└── scsi_targ                a direct-access device; see doc/target.md
    └── blk_sd -> sd_spi     the SD card behind it
```

`doc/target.md` is the target's own contract: its command set, the three rules
that are easy to get subtly wrong, and what it deliberately does not do.

**The eight registers are inside the part**, and this is not the arbitrary
choice the same question was in the sibling projects: the 5380 decodes `/CS`
with `A0..A2` and `/IOR` or `/IOW` on the die (p. 6), and every board that
used one presented the same eight registers because the chip did.  What boards
differ in - register spacing, byte lane, whether there is a pseudo-DMA
aperture - is what `wb_5380` holds, and it is the only place a second machine
needs work.

**There is no Wishbone master anywhere.**  The 5380 has a DMA handshake but no
address counter and no byte counter, so it never masters a bus; an external
DMA controller or the CPU moves every byte.  This is the biggest structural
difference from the sibling projects, and it is why `wb_master.sv` and
`wb_arb.sv` were not carried across.

### Faithfulness where it is inconvenient

Reproduce these as the silicon has them.  Each is or will be pinned by a test;
do not "fix" them:

* **Initiator Command does not read back what was written.**  Bits 6 and 5 are
  different registers on read and on write - AIP and LOST ARBITRATION out,
  TEST MODE and DIFF ENBL in (p. 12).  Both driver families work around it in
  their own way, which is about as strong a signal as reference software can
  give that the fault is real.
* **An access with DACK asserted does not decode the address at all** (p. 6).
  The DMA controller has no address lines to offer.  Parking them on register
  7 must not acknowledge an interrupt.
* **Reading register 7 clears three status bits and not the fourth.**  END OF
  DMA goes away when the Mode Register's DMA bit is reset instead (p. 16).
* **An SCSI reset clears every register except the interrupt latch and ASSERT
  RST** (p. 23), so a driver's own reset pulse is not switched off underneath
  it.  That is a different reset from the RESET pin, which clears everything.
* **Setting the Mode Register's DMA bit is refused while BSY is false**
  (p. 14).  It is the only write the chip does not take, and the one place
  the register file has to see the bus.
* **Arbitration is lost only to another device's SEL** (p. 12).  A driver
  asserts SEL itself while still arbitrating - Linux writes
  `ICR_ASSERT_SEL | ICR_ASSERT_BSY` and clears the Mode Register afterwards -
  so counting its own would break selection outright.
* **The chip does not decide who won arbitration.**  It reports AIP and LOST
  ARBITRATION and nothing else; comparing IDs against the data bus is the
  driver's job, which is why Linux reads Current SCSI Data and tests it
  against `id_higher_mask`.

And on the target's side, three of the same kind:

* **READ CAPACITY reports the last addressable block, not the count.**
* **A six-byte transfer length of zero means 256 blocks**, and it is the only
  place SCSI-1 counts that way; an *allocation* length of zero means zero, so
  the two cannot share a decode.
* **Sense is consumed by reading it**, so it is cleared when the REQUEST SENSE
  finishes rather than at the top of the next command.

### A clockless part in a clocked design

The 5380 is *"a clockless device"* whose delays come out of gate propagation
and *"may differ between devices because of inherent process variations"*
(p. 18).  Every delay the replica counts out is derived from `SYS_PERIOD_PS`
in the Makefile rather than written down, so building for a slower machine
moves them all together.

The 2.2 µs arbitration delay is deliberately **not** one of them: the
datasheet says the chip does not implement it and the driver must, and both
drivers do.  Counting it in the RTL would make a driver that omitted it work
when it should not.

### Testbench (`tb/cpp/`)

Verilator plus a C++ testbench, everything in one binary.  Verilator and
Icarus both lack usable SystemVerilog class support, which is why the reusable
layer is C++ rather than SV.

**Models attach on the negative edge of their clock.**  At a negedge the
outputs the RTL produced at the preceding posedge are stable and anything
driven takes effect at the next posedge.  New models must follow this;
sampling on the posedge sees post-edge values and silently gives wrong
results.

Blocking helpers pump the kernel internally via `Sim::run_until`, so tests
read like driver code.  Never call one from inside a clock callback.  Their
timeouts should be the driver's own, since a model more patient than the
driver hides bugs.

`sci_driver` follows `doc/drivers/Linux/NCR5380.c` step for step - `select()`
is `NCR5380_select`, `pio()` is `NCR5380_transfer_pio`, `execute()` is the
phase loop of `NCR5380_information_transfer` - so a `sys_` failure is a
failure a real driver would have hit.  Its one deliberate departure is a
selection timeout of 1 ms where the standard allows 250; a *shorter* timeout
can only make a test stricter, and it keeps a failing test from simulating a
quarter of a second of nothing.

`disk` models the block back end rather than an SD card, on purpose.  A card
that has to be initialised and clocked out a bit at a time would bury a SCSI
failure in a hundred thousand clocks of unrelated traffic.  Its latency is
adjustable and not zero, because a back end that answers instantly hides a
target that forgets to wait for one.

`sim`, `wb` and `test` come from the sibling project at `../Wish7990`, where
they are already tested.  Read it, do not change it.

`tb/sv/tb_top.sv` is the Verilator top: it wires the DUT plus the leaf modules
that have unit tests, so one binary covers both levels.  Adding a unit-tested
leaf means adding its ports here.

### Tests (`tb/cpp/tests/`)

Prefixes are meaningful and ordered by trust: `infra_` checks the testbench
itself, `layout_` checks our constants against the datasheet and both driver
headers, `unit_` checks RTL leaves against software models, `reg_` the
register file, `bus_` the SCSI engine, `sd_` the card back end, `sys_` the
whole thing the way a driver drives it - arbitrate, select, command, data,
status, message, bus free.  If an `infra_` test breaks, no `sys_`
result means anything.

`TEST(name)` must pass.  `TEST_PENDING(name, "reason")` runs, is expected to
fail, and is reported as PENDING; when it starts passing the runner says XPASS
and the marker comes off.

Tests self register; adding a file in `tb/cpp/tests/` is enough, the Makefile
globs it.  Checks are `CHECK`, `CHECK_MSG`, `CHECK_EQ`, `CHECK_NE` and
`CHECK_DRV`.

## Conventions

RTL: SystemVerilog, `always_ff`/`always_comb`, synchronous active-high reset,
two-space indent, ports one per line with the direction aligned, `_i`/`_o`
suffixes on bus ports.

C++: C++14, two-space indent, `namespace wtb`, models own their signal
pointers through a small ports struct rather than including the Verilated
header.

Comments explain intent and reference the vintage behaviour being reproduced;
they do not narrate the code.  Where a rule comes from the datasheet, cite the
page.  British/plain spelling is used throughout (`initialise`, `serialise`,
`colour`) - match it.

Every source file carries `SPDX-License-Identifier: MIT`.  The files under
`doc/drivers/` are not ours and keep their own licences; nothing in `src/` or
`tb/` is derived from them.

## Tool quirks that will bite

* Verilator parses comments starting with the word "verilator" as pragmas.
  Never start a comment line with it.
* Yosys 0.23 rejects `import pkg::*` in both module headers and module bodies,
  so RTL refers to package items as `wish5380_pkg::NAME`.  Keep it that way or
  `make synth` breaks.
* Icarus 11 aborts with an internal assertion - `elab_type.cc:86` - on any
  packed struct typedef declared **inside a package**, reached by a port or
  not.  `scsi_t` is therefore declared at file scope at the bottom of
  `wish5380_pkg.sv`, which Icarus accepts.  Do not move it into the package.
* Yosys 0.23 also rejects `$bits(some_type)` and Icarus refuses a parameter
  whose type is a struct, which is why there is no `SCSI_W` or `SCSI_IDLE`
  constant beside `scsi_t`.
* Verilator treats a packed struct as one signal, so a module whose struct
  output depends on its struct input - which `sci_bus` is, because an
  initiator only drives the data lines when the phase matches - trips
  UNOPTFLAT even though no field depends on itself.  That is one of four
  suppressions in the RTL; the others are UNUSEDPARAM on the constants
  package and two UNUSEDSIGNAL in `scsi_targ`, where a target genuinely never
  reads back the signals it drives and ignores the reserved fields of a
  command block.  All four are narrow and on a declaration.
* Icarus prints `sorry: constant selects in always_* processes ...` for the
  field-by-field assembly of `csb_o`.  It concerns sensitivity-list
  construction in an `always_comb`, is harmless, and does not fail the run;
  do not "fix" it by unrolling.
* `wish5380_pkg.sv` carries the RTL's only `lint_off`, for `UNUSEDPARAM`: it
  is a file of constants describing the whole part, so a constant with no user
  yet is expected.  Do not widen the suppression to other files.
* `make lint` runs with `-Wall` and must stay silent.  Suppressions are narrow
  `lint_off` pragmas - narrow them further rather than widening.

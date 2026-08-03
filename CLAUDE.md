# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with
code in this repository.

## What this is

A SystemVerilog SCSI controller that is software-compatible with the NCR 5380,
targeting FPGAs: Wishbone B4 on the host side, an SD card on the storage side.
The point is to let recreated vintage machines run their original, unmodified
drivers against a disk that is really a memory card.

There is **no SCSI cable and no SCSI pads**.  The chip's initiator side and two
SD-backed targets sit on a private wired-OR fabric inside the design.  A driver
sees a SCSI bus with two disks on it; a board sees two card slots.

The machine the design is *for* is the **Mac Plus / SE**, which is what the
Wishbone front end's three windows and its register spacing are shaped by.  The
machine the co-simulation actually boots is an i386 Linux, because every Mac
emulator needs a ROM a script cannot fetch; `cosim/README.md` argues that at
length, and it is the question everyone asks first.

The design is complete.  All 103 tests pass on both boards at three clock
rates, and an unmodified Linux mounts a filesystem off it.

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
make test               # build and run the whole regression (the Macintosh)
make test BOARD=isa     # the same tests against a generic ISA card
make test-all           # both boards, which is what CI runs
make test T=reg         # only tests whose name contains "reg"
make test FLAGS=-v      # show test notes and why pending tests failed
make wave T=<test>      # run with tracing -> build/waves/<test>.vcd
make list               # list registered tests
make lint               # Verilator lint, -Wall, must stay clean
make lint-icarus        # Icarus parse check
make synth              # Yosys elaboration check of every module
make clean

make -C cosim/rtl check # drive the Verilated core through a driver's own
                        # sequences, with no emulator in the loop
```

`make test` must stay green on **both boards**.  Green means 0 failed; pending
tests are expected failures and do not break the build.

`BOARD` is the register spacing, and it is the one thing that changes the
decode: sixteen bytes apart for the Macintosh, one for a generic ISA card.
With a stride of one the register window is eight bytes rather than a hundred
and twenty-eight, and the byte a register lands in is the low two bits of its
own address rather than always zero.  Linux's `g_NCR5380` drives the second
with `board=0`, which is what the co-simulation boots against.  A test that
needs the spacing asks `env.cfg().reg_stride`; one that hard-codes sixteen is
a bug.

Nothing is pending and nothing is unfinished, so the test-first loop the
project was built with - add a `TEST_PENDING`, implement the RTL, drop the
marker - is now the loop for *adding* something rather than for finishing it.
It still applies: behaviour that is not pinned by a test does not stay
working.  A change that touches the RTL runs `make test-all`, not `make test`,
because a stride of one and a stride of sixteen are different decodes.

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
wish5380_sd                  the whole thing: a WB B4 slave and two card slots
├── wish5380_wb              the same, for a board with some other back end
│   ├── wb_5380              machine glue: three windows, lanes, pseudo-DMA
│   ├── wish5380             the part
│   │   ├── sci_regs         the eight registers and the port they hide behind
│   │   └── sci_bus          arbitration, selection, handshake, interrupts
│   ├── scsi_fabric          the wired-OR: the chip, the drives, and one spare
│   └── scsi_targ  x TARGETS a direct-access device each; see doc/target.md
└── blk_sd -> sd_spi  x TARGETS  one SD card each; see doc/sd.md
```

`TARGETS` is one or two and two is the default.  A board carrying one drive
leaves the second out entirely rather than wiring an empty slot to it: a
device driving nothing is a device that is not there.  Two is the default
because a bus with one device never exercises the ID decode against anything
that could get it wrong - the only other outcome is silence.

**The two tops are kept apart because the block interface between them is the
seam that makes the rest testable.**  `tb_top` instantiates both: the bulk of
the regression drives `wish5380_wb` against a software disk, and only the
`sd_` tests pay for a card that has to be initialised at 400 kHz before it
will say anything.  Those thirteen tests take longer than the other ninety.

Either way **every test reaches the chip the way a machine does** - through
the Wishbone slave and its register window.  There is no back door, and adding
one would be a mistake: the first bug the Wishbone path found was a test that
had only ever passed because a direct register accessor was faster than a real
bus cycle.

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
* **BUSY ERROR is a level-sensitive latch**, "set whenever the MONITOR BUSY
  bit is true and BSY is false" (p. 16).  Reading register 7 empties it and
  the condition immediately fills it again, which is why `NCR5380_intr`
  writes the Mode Register *before* it reads register 7.  Acknowledging alone
  achieves nothing, and a replica that let the acknowledge stick would make a
  driver that got the order wrong appear to work.
* **Arbitration is lost only to another device's SEL** (p. 12).  A driver
  asserts SEL itself while still arbitrating - Linux writes
  `ICR_ASSERT_SEL | ICR_ASSERT_BSY` and clears the Mode Register afterwards -
  so counting its own would break selection outright.
* **The chip does not decide who won arbitration.**  It reports AIP and LOST
  ARBITRATION and nothing else; comparing IDs against the data bus is the
  driver's job, which is why Linux reads Current SCSI Data and tests it
  against `id_higher_mask`.

One on the glue's side, where there is no silicon to be faithful to and the
choice had to be argued instead:

* **A wide access to the register window answers `ERR_O`.**  Every driver
  reaches a register with a byte access; what a board would really do with a
  `movew` there is its decoder's business.  A fault says so, where serving one
  lane and dropping the rest would not.  `doc/interface.md` records it.

And on the card's side, two more (`doc/sd.md`):

* **A version 1 card is addressed by byte offset and a version 2 card by block
  number.**  Below four gibibytes either is legal and real ones of both exist.
* **The two CSD layouts state the size in quite different ways.**  Reading one
  as the other gives a disk of the right shape and the wrong size.

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

`sd_card` is deliberately stricter than a real card has to be: it checks the
CRC7 on CMD0 and CMD8, computes a real CRC16 over every block it sends, and
holds the line low after a write.  A lax model would let a controller with the
wrong constants pass and fail against silicon.

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

Prefixes are meaningful and ordered by trust:

| prefix   | what it checks |
|----------|----------------|
| `infra_` | the testbench itself: the clock, the reset, the accessors |
| `layout_`| our constants, against the datasheet and both driver headers |
| `reg_`   | the register file, on its own |
| `bus_`   | the SCSI engine |
| `dma_`   | the same chip driven by a real DMA controller: one DACK per DRQ, and EOP on the last byte |
| `wb_`    | the machine glue and its three windows |
| `sys_`   | the whole thing the way a driver drives it - arbitrate, select, command, data, status, message, bus free |
| `two_`   | what only two drives on the bus can check: the ID decode, and a wired-OR with three devices on it |
| `sd_`    | that same stack again with a real card underneath it |

If an `infra_` test breaks, no `sys_` result means anything.  The counts are
in the top-level README and add to the total, so a prefix that stops being
counted shows up as arithmetic.

`TEST(name)` must pass.  `TEST_PENDING(name, "reason")` runs, is expected to
fail, and is reported as PENDING; when it starts passing the runner says XPASS
and the marker comes off.

Tests self register; adding a file in `tb/cpp/tests/` is enough, the Makefile
globs it.  Checks are `CHECK`, `CHECK_MSG`, `CHECK_EQ`, `CHECK_NE` and
`CHECK_DRV`.  `CHECK_DRV` reports `env.drv()`'s error, which is the fast
path's driver; `test_sd.cpp` runs on the other one and has its own `CHECK_SD`
for that reason.

## Co-simulation (`cosim/`)

A separate and much slower loop, deliberately not part of `make test`.
**Nothing in `src/` or `tb/` may grow a dependency on it** - the traffic goes
the other way.  `cosim/README.md` records why the guest is an i386 Linux and
not a Macintosh, which is the question everyone asks first.

`make -C cosim/rtl check` is the thing to run before blaming the RTL: it drives
the shared library through the driver's own sequences with no emulator in the
loop, so a fault has two places to be instead of four.

The library reuses `sim`, `wb`, `util` and `sd_card` from `tb/cpp` unchanged.
A co-simulation with its own bus model would drift from the regression and stop
saying anything about it.

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
  UNOPTFLAT even though no field depends on itself.  Synthesis sees the real,
  acyclic netlist.
* A mutation test on a parameterised module has to change the parameter at the
  level that *sets* it.  Changing `wb_5380`'s default proves nothing, because
  `wish5380_wb` overrides it.  And a mutation that leaves a signal unused
  fails the build rather than the test, which looks like a passing mutation
  if only the summary line is read.
* Icarus prints `sorry: constant selects in always_* processes ...` for the
  field-by-field assembly of `csb_o`.  It concerns sensitivity-list
  construction in an `always_comb`, is harmless, and does not fail the run;
  do not "fix" it by unrolling.
* `make lint` runs with `-Wall` and must stay silent.  Every suppression in
  the RTL is a narrow `lint_off`/`lint_on` pair around a declaration, and
  there are four reasons for one - `grep -n lint_off src/*.sv` is the list,
  not a number in this file, because a number here goes stale:
  * **UNUSEDPARAM** on `wish5380_pkg.sv`, which describes the whole part
    rather than the part of it wired up today;
  * **UNOPTFLAT** on `drive_data` in `sci_bus.sv`, for the struct-granularity
    reason above;
  * **UNUSEDSIGNAL** on registers a device defines and this design reads only
    some fields of - a command block's reserved bytes in `scsi_targ.sv`, the
    card's own registers in `blk_sd.sv`;
  * **UNUSEDSIGNAL** on the second drive's wires and ports in
    `wish5380_wb.sv` and `wish5380_sd.sv`, which exist whether or not
    `TARGETS` builds a drive on the end of them.

  Narrow them further rather than widening, and prefer using a signal to
  suppressing it: two of the four above started as something that could have
  been used and was not.

# The interface

This is the agreement between `src/` and `tb/`.  Anything that changes the
register map, the bus behaviour, the internal SCSI bus or the block back end
has to change the RTL, the testbench and this document together.

Two files hold the same NCR 5380 constants and are kept in step by hand:
`src/wish5380_pkg.sv` for the RTL and `tb/cpp/ncr5380.h` for the testbench.
They are deliberately independent - the testbench is not allowed to derive its
expectations from the RTL - and `tb/cpp/tests/test_layout.cpp` is what stops
them drifting, from each other and from the two driver headers.

Page numbers are the printed pages of `doc/NCR5380_design_manual_Mar86.pdf`.

## The shape of the thing

```
wish5380_sd                  the whole thing: a WB B4 slave and two card slots
├── wish5380_wb              the same, for a board with some other back end
│   ├── wb_5380              the machine glue: windows, byte lanes, pseudo-DMA
│   ├── wish5380             the part
│   │   ├── sci_regs         the eight registers and the port they hide behind
│   │   └── sci_bus          arbitration, selection, handshake, interrupts
│   ├── scsi_fabric          the wired-OR: the chip, the drives, and one spare
│   └── scsi_targ  x TARGETS a direct-access device each
└── blk_sd -> sd_spi  x TARGETS  one SD card each
```

The two tops are separate because the block interface between them is a seam
worth having: `wish5380_wb` is the design for a board that backs its drives
with something other than an SD card, and it is what the bulk of the
regression drives, against a software disk.

## Where the line between the part and the machine falls

**The eight registers are inside the part.**  This is not the arbitrary
choice it was in the sibling project, where the same question had to be
argued: the NCR 5380 decodes `/CS` with `A0..A2` and `/IOR` or `/IOW` on the
die (p. 6), and every board that ever used one presented the same eight
registers because the chip did.  There is no board-specific register file to
factor out.

What boards differ in is entirely outside: how far apart the registers sit in
the address space, which byte lane they land on, whether there is a
pseudo-DMA aperture and what it does when the chip is not ready.  That is
`wb_5380`, and it is the only place a second machine will need work.

`irq_o` is the part's IRQ pin, already gated by the Mode Register's interrupt
enables, because those bits are on the die.  A board that latches or inverts
the interrupt does it in `wb_5380`.

## The register port

`sci_regs` presents a synchronous port rather than the part's asynchronous
`/CS`, `/IOR`, `/IOW`:

| signal   | meaning |
|----------|---------|
| `stb_i`  | one access, this cycle.  Exactly one clock wide |
| `we_i`   | write when high, read when low |
| `dack_i` | the access is a DMA acknowledge: it reaches a data register and the address is not decoded at all (p. 6) |
| `adr_i`  | `A2..A0` |
| `dat_i`  | write data |
| `dat_o`  | read data, combinational on `adr_i` and `dack_i` |

A synchronous port is enough because nothing a driver can do distinguishes the
two.  The datasheet's CPU timings (p. 26) are setup and hold requirements on
an access that the host completes; a Wishbone cycle that takes a whole clock
satisfies all of them by construction, and the chip's *responses* - the ones a
driver actually waits for, like REQ going true - are bus-side events measured
in hundreds of nanoseconds, not in access timing.

What the port must preserve is that **an access is an event**.  Four of the
eight addresses do something rather than hold something: writing 5, 6 or 7
starts a DMA transfer whatever is on the data bus, and reading 7 clears three
status bits (pp. 16-17).  So `wb_5380` must raise `stb_i` exactly once per
Wishbone cycle and never speculatively, and a read must never be turned into a
peek.  `reg_strobes_fire_on_the_access_and_hold_nothing` pins the first half;
the second is a rule for `wb_5380`.

There is now a **third** place that rule can be got wrong.
`cosim/patches/qemu/` adds `hw/scsi/ncr5380.c`, a software model of the same
part written for QEMU, and a board reaching it through a `MemoryRegionOps`
read handler has exactly the same obligation: call the model once per guest
access, and never speculatively.  That file is not a consumer of this
document - it is an independent second reading of the same datasheet, and is
deliberately not derived from `src/` any more than `tb/cpp/ncr5380.h` is - but
it is another implementation of this port, and the traps are the same traps.
`cosim/README.md` records what the two agreeing is worth.

## The Wishbone slave

Wishbone B4 classic, 32-bit data, **word addressed**: `ADR` carries the index
of a 32-bit word and `SEL` alone picks bytes.  This follows the sibling
projects and is what `tb/cpp/wb.h` converts at the pin, so everything inside
the core and inside the tests is in byte addresses.

The 5380 is a byte-wide slave-only peripheral.  It has a DMA *handshake*
(DRQ/DACK) but no address counter and no byte counter, so it never masters a
bus: an external DMA controller or the CPU itself moves every byte.  There is
therefore no Wishbone master anywhere in this design, which is the single
biggest difference from the sibling projects.

It crosses module boundaries as two packed structs, `wb_req_t` and `wb_rsp_t`,
declared beside `scsi_t` in `src/wish5380_pkg.sv`.  A SystemVerilog interface
with modports is what this ought to be and is not usable: Icarus 11 will not
parse an interface port and Yosys 0.23 elaborates one into a disconnected
netlist without complaining, so `make synth` would pass while checking
nothing.  `doc/block.md` sets that out in full, since the same reasoning
governs the block back end.

**The two testbench tops keep the signals flat** - `tb/sv/tb_top.sv` and
`cosim/rtl/rtl_top.sv` both take the struct apart at their own ports.  The
C++ models on the other side hold a pointer per signal, and Verilator
presents a packed struct as one wide vector; a model that had to be told a
field's bit position would drift from the RTL silently.  That is the same
reason `scsi_t` is unpacked there, and it is a rule for anything added later.

### Three apertures, because the Mac has three

NetBSD's `sbc_obio.c` names them (lines 60-74), and they are the model here:

| aperture | `sbc_obio.c` | what an access does |
|----------|--------------|---------------------|
| registers | `SBC_REG_OFS` | one ordinary register access |
| pseudo-DMA with handshake | `SBC_HSK_OFS` (`sc_drq_addr`) | a DACK access that waits for DRQ, and raises `ERR_O` if it waits too long |
| pseudo-DMA without handshake | `SBC_DMA_OFS` (`sc_nodrq_addr`) | a DACK access that does not wait |

The register window puts register *n* at byte offset *n* × `REG_STRIDE`.
`REG_STRIDE` is an elaboration parameter and defaults to 16, which is the
Mac's `(reg) << 4` in `mac_scsi.c` line 38; a generic ISA card sets it to 1,
which is `inb(base + reg)` in `g_NCR5380.c`.

That is the whole difference between the two boards, and it is enough to
change the decode: with a stride of one the register window is eight bytes
rather than a hundred and twenty-eight, and the byte a register lands in is
the low two bits of its own address rather than always zero.  `make test-all`
runs the whole suite against both, the way the sibling project runs its
against both MII and GMII.

The handshake aperture is where the Mac's character comes from.  Linux's
`mac_scsi.c` reads and writes it with `moveb` and `movew`, so an access there
moves one SCSI byte per asserted byte lane - a `movew` is two consecutive
REQ/ACK handshakes on the bus, not one wide one.

**A `movew` is the widest access the aperture has to serve.**  `MOVE_16_WORDS`
looks like a burst instruction and is not: it is sixteen `movew` instructions
unrolled for speed (lines 163-210), and there is no `movem` anywhere in the
file.  A glue that prepared for a thirty-two byte transaction would be
preparing for something no driver issues.

The driver wraps those instructions in an exception fixup table, because the
Mac's hardware raises a **bus error** when the chip does not produce a byte in
time; that is `ERR_O` here, and the driver counts on it as a normal outcome
rather than a fault - a faulting `moveb` on receive is retried, a faulting
`movew` abandons the transfer because the residual count is then uncertain.
`macscsi_wait_for_drq` shows what it polls before each chunk and is the
sequence to match.

The no-handshake aperture exists for the machines where the hardware
handshake is broken or absent; there an access is a plain DACK cycle and the
driver has already satisfied itself that a byte is ready.

## The internal SCSI bus

There are no SCSI pads.  The only devices are the chip and the SD-backed
drives beside it, so the bus is a private fabric rather than a cable.

The real bus is open collector and active low, and every device sees the OR of
what everybody is driving.  Here each device drives a `scsi_t`
of active-**high** "asserted" signals and `scsi_fabric` ORs them, which is the
same function with the inversion taken out.  Nothing above the fabric can tell
the difference, because the sense inversion in the real part is at the pad and
there is no pad; the registers still report the bus exactly as the datasheet
describes them, where "a one (1) is used to indicate signal assertion" (p. 10)
even though the wire is low.

A device sees its own contribution in what comes back, which matters and is
easy to get wrong: the Current SCSI Bus Status Register shows BSY as true when
this chip is the one asserting it, and both drivers rely on that when they
check that arbitration was won.

Parity travels as a bit rather than as something the fabric computes, so a
test can drive bad parity at a chip that has ENABLE PARITY CHECKING set.

**The fabric carries four devices**: the chip, two drives, and one spare.  The
spare is what the testbench drives to stand in for something else on the bus,
and is tied to zero in the real top level - a device driving nothing is a
device that is not there, which is exactly what an open-collector bus means by
it, and it is also how `TARGETS` switches the second drive off for a board
that carries one.

Two drives is the default because a bus with one device never exercises the ID
decode against anything that could get it wrong: the only other outcome is
silence, so a target that answered every selection and one that answered only
its own would be indistinguishable.  `doc/target.md` says more, including why
"arbitration" is the wrong word for what a second drive exercises.

## Clocking, and a clockless part

The 5380 is *"a clockless device"* whose bus free, bus set and bus settle
delays *"may differ between devices because of inherent process variations"*
(p. 18).  A clocked replica has to count them out instead.

Every such delay is derived from `SYS_PERIOD_PS` in the Makefile rather than
written down, so building for a slower machine moves them all together and the
regression checks the clock that was built:

| delay | value | where |
|-------|-------|-------|
| bus free | 400 ns of BSY inactive before arbitration may begin | p. 18 |
| bus settle | 400 ns before a selection or a loss of BSY is believed | p. 19, p. 22 |

The bus clear delay is not counted anywhere, and does not need to be.  It is a
*deadline* rather than a wait - a device must release the bus "within a bus
clear delay (800 nsec)" of RST going true (p. 21) - and this chip releases
everything on the clock edge that sees RST, which meets it at any clock the
design will ever be built for.

The arbitration delay of 2.2 µs is deliberately *not* in that table.  The
datasheet is explicit that the chip does not implement it - *"This delay must
be implemented in the controlling software driver"* (p. 18) - and both drivers
do, so counting it here would make a driver that omitted it work when it
should not.  `tb/cpp/ncr5380.h` carries it because the driver model needs it.

The software model in `cosim/patches/qemu/` counts *neither* of the two out,
and the argument for why that is legal belongs beside this one.  Both delays
exist to filter a real cable - to stop a glitch looking like a free bus or a
lost target - and a fabric made of C structs has no glitches to filter, so
both collapse to "BSY is false once the wires have settled".  That is a
property of the medium and not a shortcut.  The arbitration delay is the
opposite case and is absent there for exactly the reason it is absent here:
the part does not implement it, so neither may a model of the part.  A
software chip that quietly counted it would make a broken driver pass on the
fast path and fail on the slow one, which is the worst of both.

## One register write the chip refuses

Every register write lands except one: "BSY must be active in order to set the
DMA Mode bit" (p. 14).  Writing the Mode Register with DMA MODE set while the
bus shows no BSY writes every other bit and leaves that one clear, so a driver
that started a transfer with no target connected would find it had not started
one.  It is the only place the register file needs to see the bus at all, and
`bus_dma_mode_needs_a_connected_bus` pins it.

## The block back end

**`doc/block.md` is this interface's own contract** - the handshake, the three
rules a back end can break without anything reporting an error, and who owns
the sector buffer when.  What follows is why it exists; that document is what
it is.

`scsi_targ` reaches storage through a 512-byte sector buffer and four fields -
start, direction, block address, done - and never talks to a card itself.  The
back end fills the buffer before a READ and drains it after a WRITE, through
its own port on the same dual-ported memory; the two sides never touch it at
the same time, so no arbitration is needed and none is implemented.

512-byte blocks, which is both the SD card's unit and the sector size every
vintage driver expects.

That interface is what lets `blk_sd` stand where `tb/cpp/disk.h` stands
without the SCSI side noticing - which is not a hypothetical, since the
regression tests SCSI against the software disk and only the `sd_` tests pay
for a card - and what would let an SD 4-bit layer replace the SPI one without
the block side noticing.  `doc/target.md` sets out the rest of the target's
contract and `doc/sd.md` what is on the far side of this one.

Each drive gets a whole card, and that is an assumption rather than a
conclusion: a modern card holds many times a vintage disk, and every
comparable device carves one card into several drives instead.
`doc/storage.md` surveys what they do and argues what this design should
adopt.  Nothing of it is implemented; this seam is where it would go.

## Four decisions with no silicon to be faithful to

Everything above is what the datasheet or a driver settles.  These four had to
be argued instead, and are recorded because the argument is the only thing
that makes them reviewable.

* **A wide access to the register window answers `ERR_O`.**  A register is one
  byte and every driver reaches it with one; what real hardware would do with
  a `movew` there is a property of the board's decoder rather than of the
  chip, so there is nothing to be faithful to.  A fault says so where quietly
  serving one lane and dropping the rest would not.
* **`DRQ` and `EOP` are brought out of `wish5380_wb`**, because they are the
  part's own pins and a board with a real DMA controller in front of the chip
  needs both.  The Mac drives neither: its pseudo-DMA uses the DRQ that
  `wb_5380` already watches internally, and it has no End of Process at all.
  `READY` is not brought out, because it belongs to block mode DMA, which
  nothing here uses.
* **The private SCSI bus is brought out too**, as `bus_o` to watch and
  `peer_i` to drive.  An interconnect that cannot be seen cannot be debugged
  on hardware either, and an integrator will want it on a logic analyser.  Tie
  `peer_i` to zero and leave `bus_o` unconnected in a real design.
* **The pseudo-DMA windows stay addressable in the ISA build**, even though no
  generic-ISA driver looks there - Linux's own boot line says
  `NO_PSEUDO_DMA`.  Taking them away would be more faithful to a real generic
  card and would only take away coverage, since every test that exercises
  those windows would then have to be Mac-only.

## Nothing is open

Every question this document once listed as unsettled has an answer above.
What would change it now is a new board, a new back end, or a device on the
fabric that is not a disk - and any of those changes the RTL, the testbench
and this document together, which is the rule at the top.

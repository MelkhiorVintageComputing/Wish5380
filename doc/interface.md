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
wish5380_wb                  Wishbone B4 slave, one clock, irq_o
├── wb_5380                  the machine glue: apertures, byte lanes, pseudo-DMA
├── wish5380                 the part
│   ├── sci_regs             the eight registers and the port they hide behind
│   └── sci_bus              arbitration, selection, handshake, interrupts
├── scsi_fabric              the wired-OR joining the part to the target
└── scsi_targ                a direct-access device
    └── blk_sd -> sd_spi     the SD card behind it
```

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

### Three apertures, because the Mac has three

NetBSD's `sbc_obio.c` names them (lines 60-74), and they are the model here:

| aperture | `sbc_obio.c` | what an access does |
|----------|--------------|---------------------|
| registers | `SBC_REG_OFS` | one ordinary register access |
| pseudo-DMA with handshake | `SBC_HSK_OFS` (`sc_drq_addr`) | a DACK access that waits for DRQ, and raises `ERR_O` if it waits too long |
| pseudo-DMA without handshake | `SBC_DMA_OFS` (`sc_nodrq_addr`) | a DACK access that does not wait |

The register window puts register *n* at byte offset *n* × `REG_STRIDE`.
`REG_STRIDE` is an elaboration parameter and defaults to 16, which is the
Mac's `(reg) << 4` in `mac_scsi.c` line 38.  A generic ISA card sets it to 1.

The handshake aperture is where the Mac's character comes from.  Linux's
`mac_scsi.c` reads and writes it with `movew` and `moveml` - two and thirty-two
bytes per instruction (lines 215-266) - so a wider access there is *n*
consecutive byte handshakes on the SCSI bus, not one wide one.  And it wraps
those instructions in an exception fixup table, because the Mac's hardware
raises a **bus error** when the chip does not produce a byte in time; that is
`ERR_O` here, and the driver counts on it as a normal outcome rather than a
fault.  `macscsi_wait_for_drq` (line 279) shows what it polls before each
chunk and is the sequence to match.

The no-handshake aperture exists for the machines where the hardware
handshake is broken or absent; there an access is a plain DACK cycle and the
driver has already satisfied itself that a byte is ready.

## The internal SCSI bus

There are no SCSI pads.  The only target is the SD-backed one inside the
chip, so the bus is a private fabric rather than a cable.

The real bus is open collector and active low, and every device sees the OR of
what everybody is driving.  Here each device drives a `wish5380_pkg::scsi_t`
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

## Clocking, and a clockless part

The 5380 is *"a clockless device"* whose bus free, bus set and bus settle
delays *"may differ between devices because of inherent process variations"*
(p. 18).  A clocked replica has to count them out instead.

Every such delay is derived from `SYS_PERIOD_PS` in the Makefile rather than
written down, so building for a slower machine moves them all together and the
regression checks the clock that was built:

| delay | value | where |
|-------|-------|-------|
| bus free | 400 ns of BSY inactive before the bus is free | p. 18 |
| bus settle | 400 ns before SEL is believed | p. 19 |
| bus clear | 800 ns to release the bus after RST | p. 21 |

The arbitration delay of 2.2 µs is deliberately *not* in that table.  The
datasheet is explicit that the chip does not implement it - *"This delay must
be implemented in the controlling software driver"* (p. 18) - and both drivers
do, so counting it here would make a driver that omitted it work when it
should not.  `tb/cpp/ncr5380.h` carries it because the driver model needs it.

## The block back end

`scsi_targ` reaches storage through a block interface and never talks to a
card directly, so the SD 4-bit layer can replace the SPI one without the SCSI
side noticing.  512-byte blocks, which is both the SD card's unit and the
sector size every vintage driver expects.

## What is not settled yet

* Whether `wb_5380` should let a 16- or 32-bit access to the register window
  through at all, or answer `ERR_O`.  The Mac only ever does byte accesses
  there; the question is what a *wrong* driver should see.
* How many targets the fabric carries.  One is enough to boot; a second would
  exercise arbitration properly.
* Whether the part's DRQ, EOP and READY pins are brought out of `wish5380_wb`
  for a real external DMA controller, or stay inside for `wb_5380` alone.

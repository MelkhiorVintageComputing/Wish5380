# Wish5380

## Overview

A SystemVerilog SCSI controller, software-compatible with the NCR 5380 SCSI
Interface Chip, targeted at FPGA.  Wish5380 connects to a Wishbone B4 bus
host-side and to an SD or micro-SD card for storage.  It can be used to give a
recreated vintage computer a disk that its own unmodified driver - an operating
system's or a ROM's - can talk to.

The part implemented is the **NCR 5380** itself.  Its CMOS successor the
53C80 is functionally equivalent and adds one software-visible bit, LAST BYTE
SENT in the Target Command Register; that is an elaboration option rather than
the default, because the NMOS part is what the vintage machines shipped with
and a driver that finds the bit will use it.
`doc/NCR5380_design_manual_Mar86.pdf` is the datasheet.

There is no SCSI cable.  The chip's initiator side and an SD-backed target sit
on a private wired-OR fabric inside the design, so from the driver's point of
view there is a SCSI bus with a disk on it, and from the board's point of view
there is a card slot.

## Status

The design is complete.  An unmodified Linux, an unmodified NetBSD, an
unmodified SunOS 4.1.1 and an unmodified EmuTOS all mount a filesystem off it,
one moving every byte with the CPU and the other three through three different
DMA controllers.  A driver
reaches the Wishbone slave through the three windows a Macintosh expects,
arbitrates and selects, and reads and writes blocks - either a byte at a time
in programmed I/O or through the pseudo-DMA aperture the Mac uses for speed -
and those blocks come off SD cards in the board's two slots.

Everything the chip does in hardware is there: arbitration with AIP and LOST
ARBITRATION, the REQ/ACK handshake it automates in DMA mode in both roles and
both directions, and all six of its interrupt sources - selection and
reselection, end of process, SCSI reset, parity, bus phase mismatch and loss
of BSY.

| block               | what it is                                           | tested by              |
|---------------------|------------------------------------------------------|------------------------|
| `wish5380_sd`       | the whole thing: a Wishbone slave and two card slots  | `sd_` (13)             |
| `wish5380_wb`       | the same, for a board with some other back end        | `sys_` (23)            |
| `wb_5380`           | machine glue: three windows, byte lanes, pseudo-DMA   | `wb_` (15)             |
| `wish5380`          | the part itself                                       | `sys_`, `bus_`         |
| `sci_regs`          | the eight registers and the port they hide behind     | `reg_` (10)            |
| `sci_bus`           | arbitration, the handshake, the interrupts            | `bus_` (22), `dma_` (15) |
| `scsi_fabric`       | the wired-OR joining everything on the bus            | `two_` (8), `bus_`     |
| `scsi_targ`         | a direct-access device, one per drive                 | `sys_`, `two_`, `sd_`  |
| `blk_sd`, `sd_spi`  | an SD card in SPI mode                                | `sd_`                  |

`dma_` is the chip driven by a *real* DMA controller rather than by the Mac's
pseudo-DMA aperture, which is the CPU pretending to be one: a CPU has no End
of Process pin, so EOP and everything downstream of it is unreachable that
way and is on the path a Sun 3/60 takes.

A prefix is counted once, against the block it is mostly about; the others
beside it reach the same block through something else.  Two more prefixes are
about the testbench rather than the design: `layout_` (7) pins the constants
to the datasheet and to all four drivers' own declarations, and `infra_` (5)
checks the clock, the reset and the accessors everything else stands on - if
one of those fails, no other result means anything.

All 118 pass, with none pending, in six builds: two boards - the Macintosh,
with its registers sixteen bytes apart, and a generic ISA card with them one
byte apart - at each of 7.8 MHz, 50 MHz and 125 MHz.  The part's delays are
derived from the clock rather than written down and the register spacing is an
elaboration parameter, so the regression checks whichever build it was given
rather than a nominal one.

Beyond the regression, three guests drive the design in the two ways the chip
can be driven.  An unmodified i386 Linux probes it with its own `g_NCR5380`,
attaches `sda`, mounts an ext2 root filesystem off it and reads and writes
files, all in programmed I/O.  And an unmodified NetBSD/sun3 boots off it on an
emulated Sun-3/60, where the bytes move by real bus-master DMA instead: the
3/60 PROM reads the disk label, NetBSD's own two-stage bootloader loads a
1.9 MB kernel through the chip's DMA port, and NetBSD's own `si` driver then
arbitrates, selects and attaches the disk -

```
[   1.0000000] si0 at obio0 addr 0x140000 ipl 2: options=0xf
[   3.1400030] sd0 at scsibus0 target 0 lun 0: <DOLBEAU, WISH5380 SD CARD, 0001> disk fixed
```

- mounts its root filesystem off it, and, at a shell, builds a new filesystem
on a spare partition with `newfs`, writes a file into it and reads the same
checksum back after an unmount and a remount.  That is the design writing
under a driver that is not ours.

A third guest is **SunOS 4.1.1**, which matters because its `si` is the
driver Sun wrote for this exact board, and it uses the hardware in ways
NetBSD does not:

```
SunOS Release 4.1.1 (GENERIC) #1: Sat Oct 13 06:05:48 PDT 1990
si0 at obio 0x140000 pri 2
sd0 at si0 slave 0
sd0:  <Awesome cyl 2046 alt 2 hd 16 sec 16>
root on sd0a fstype 4.2
```

Every fault the co-simulation has found in the Sun-3 board model came from
that driver rather than from NetBSD - three of them, all about transfers that
do not end at terminal count, because SunOS asks a disk for more than it has
and NetBSD never does.

A fourth is **EmuTOS on an Atari TT**, which is the free TOS the Atari never
had, and a third machine after the ISA card and the Sun.  Its driver takes no
interrupts at all - it arms Atari's DMA chip and spins on the MFP until the
5380 raises its line - so it is the one guest that depends on End of Process
and the END OF DMA bit that follows it:

```
                    EmuTOS Version:     1.4
                    Machine:            Atari TT
                    GEMDOS drives:      ABC

the 5380 is a replica and the disk is a memory card
and it wrote that back to C:\WROTE.TXT
```

`C` is a FAT partition found by reading the card's own partition table; the
line under it was printed by a program that EmuTOS found in `C:\AUTO`, read
off the chip and ran; and the line under *that* is the same journey backwards,
checked afterwards on the host by parsing the card image.

See [`cosim/`](cosim/README.md).

## Building and testing

Needs Verilator (5.x), a C++ compiler and GNU make; Icarus Verilog and Yosys
are optional extra checks.

```sh
make test               # build and run the whole regression (the Macintosh)
make test BOARD=isa     # the same tests against a generic ISA card
make test-all           # both boards, which is what CI runs
make test T=reg         # just the tests whose name contains "reg"
make wave T=<test>      # run with tracing, waveform in build/waves/
make lint               # Verilator lint, -Wall, must stay clean
make lint-icarus        # Icarus parse check
make synth              # Yosys elaboration check
```

`make test` must stay green.  Green means 0 failed; pending tests are expected
failures and do not break the build.

## Documentation

[`doc/README.md`](doc/README.md) is the index, and says which document answers
which question.  The two to start from:

* [`doc/NCR5380_design_manual_Mar86.pdf`](doc/NCR5380_design_manual_Mar86.pdf) -
  the datasheet, and the authority on every bit of every register.
* [`doc/interface.md`](doc/interface.md) - the contract between `src/` and
  `tb/`: the register port, the Wishbone slave's three windows, the internal
  SCSI bus, and the delays a clockless part leaves a clocked replica to count.

Then [`doc/target.md`](doc/target.md) for the disk, [`doc/sd.md`](doc/sd.md)
for the card behind it, [`doc/drivers`](doc/drivers/README.md) for the vintage
drivers and which of them is the authority on what, and
[`cosim/`](cosim/README.md) for the four guests - an unmodified Linux, an
unmodified NetBSD, an unmodified SunOS 4.1.1 and an unmodified EmuTOS, one
driving the chip byte by byte and the other three through three different DMA
controllers.

## Layout

```
src/      RTL
tb/sv/    simulation top level
tb/cpp/   testbench: bus models, SCSI and SD models, test framework, tests
cosim/    the Verilated core behind a C interface, and the guests that drive it
doc/      datasheets, interface notes, vintage drivers
```

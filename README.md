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

Early.  The register file is done and tested; the SCSI engine, the Wishbone
front end, the target and the SD back end are not yet written.

| block                                            | state         |
|--------------------------------------------------|---------------|
| constants, checked against both driver headers   | done, tested  |
| the eight registers (`sci_regs`)                 | done, tested  |
| SCSI engine: arbitration, selection, handshake (`sci_bus`) | not started |
| Wishbone front end and pseudo-DMA (`wb_5380`)    | not started   |
| internal SCSI fabric (`scsi_fabric`)             | not started   |
| SCSI target and command set (`scsi_targ`)        | not started   |
| SD card back end (`blk_sd`, `sd_spi`)            | not started   |
| driver model and system tests                    | not started   |
| co-simulation against a real driver              | not started   |

22 tests pass, none pending.

## Building and testing

Needs Verilator (5.x), a C++ compiler and GNU make; Icarus Verilog and Yosys
are optional extra checks.

```sh
make test               # build and run the whole regression
make test T=reg         # just the tests whose name contains "reg"
make wave T=<test>      # run with tracing, waveform in build/waves/
make lint               # Verilator lint, -Wall, must stay clean
make lint-icarus        # Icarus parse check
make synth              # Yosys elaboration check
```

`make test` must stay green.  Green means 0 failed; pending tests are expected
failures and do not break the build.

## Documentation

* [`doc/interface.md`](doc/interface.md) - the register map, the Wishbone
  apertures and the internal SCSI bus: the contract between the RTL and the
  testbench.
* [`doc/NCR5380_design_manual_Mar86.pdf`](doc/NCR5380_design_manual_Mar86.pdf) -
  the datasheet, which is the authority on bit positions and register
  behaviour.
* [`doc/drivers`](doc/drivers/README.md) - drivers for the real chip, used as
  the reference for software-visible sequencing.

## Layout

```
src/      RTL
tb/sv/    simulation top level
tb/cpp/   testbench: bus models, SCSI and SD models, test framework, tests
doc/      datasheets, interface notes, vintage drivers
```

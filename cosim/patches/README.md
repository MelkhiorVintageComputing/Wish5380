# Patches

Changes to emulators, kept as patches because the emulators are not ours and
their sources do not belong in this repository.

## `qemu/` and `qemu-rtl/`

Two series, one tree, two machines.  Both apply to the **Sun-3 QEMU fork**,
because mainline has no Sun-3 machine at all.  The fork lives outside this
tree (`~/qemu-sun3` by default) and is a separate piece of work under separate
ownership; nothing here modifies it.

`cosim/scripts/build-sun3-qemu.sh` clones the fork's already-patched tree into
`work/sun3-qemu`, applies `qemu/` and then `qemu-rtl/`, and builds
`qemu-system-m68k` and `qemu-system-i386`.  The order is not a convenience:
the second series rewrites files the first creates.  `-s` stops after the
first, which is how "the first series stands on its own" is tested rather than
asserted.

### `qemu/` - the software 5380, and it could be posted

Five patches that owe nothing to this project.  They add an NCR 5380 to QEMU
and two boards that carry it, and there is no mention of Verilog, of a shared
library, or of Wish5380 anywhere in them.

| patch | what it does |
|---|---|
| `0001-hw-scsi-a-software-NCR-5380.patch` | the part itself, in C, on a real `SCSIBus` |
| `0002-...-the-ISA-NCR-5380-card.patch` | the i386 card: eight ports, an interrupt, no DMA |
| `0003-...-let-the-MMU-reach-the-SCSI-registers.patch` | lets the Sun-3 MMU reach the board at all |
| `0004-...-the-Sun-3-si-onboard-SCSI-board.patch` | the `si` board: Am9516 UDC, packing FIFO, CSR |
| `0005-...-attach-the-onboard-SCSI-board.patch` | puts one on the machine |

QEMU has no NCR 5380 and never has: the only hits for it in the tree are a
Linux bootinfo tag and a line of captured `ls` output used as QDict test data,
and `hw/m68k/q800.c` models a Macintosh that really had one and uses an ESP
anyway.  The first patch is written the way `esp.c` is - the chip a bare
`TYPE_DEVICE`, the boards separate objects around it - because that is the
shape the part demands: every 5380 machine presented the same eight registers,
and they differed only in where those landed and in what answered DRQ.

It models wires rather than commands, which is the decision worth knowing
about before reading it.  A 5380 has no sequencer, so there is no command
level to hide behind; the adapter at the bottom of the file is the only part
that knows what a CDB is, and it is where QEMU's command-oriented `SCSIBus`
gets its REQ/ACK put back.

The third is not optional and is easy to miss.  `sun3mmu.c` carries a
whitelist of physical addresses the MMU is allowed to reach, and anything
outside it becomes a bus-error timeout *before* the access is dispatched.  A
device can be correctly modelled, correctly mapped, and still be invisible:
the PROM's probe writes one word to the board, takes a bus error, and reports
"Device not found" without ever having read a register.  That was the first
day of this work.

The fourth carries three decisions about the board's status register that
only a driver could settle, and all three came from SunOS rather than from
NetBSD - `DMA_CONFLICT` never set, `DMA_IP` not raised at terminal count, and
`DMA_ACTIVE` cleared when the chip stops asking rather than when the count
runs out.  Its commit message argues each; `cosim/README.md` has the
evidence.

The fifth attaches the board unconditionally, because every 3/60 shipped with
onboard SCSI, and turns off QEMU's default CD-ROM in the same breath.  That
pairing is not cosmetic: with `block_default_type = IF_SCSI` the default
empty CD lands at target 2 and answers NOT READY, and the 1.9 boot PROM
auto-boots by walking the bus - so it finds the phantom drive and retries it
for ever instead of saying "Device not found" and stopping at the monitor.

### `qemu-rtl/` - the Verilated chip, and it could not be

Three patches that are local to this project and are not offered upstream:
they `dlopen` a shared library built from `src/`.

| patch | what it does |
|---|---|
| `0001-...-ncr5380-isa-carry-the-Verilated-core-instead.patch` | `core=auto\|sw\|rtl` on the card |
| `0002-...-sun3-si-...-carry-the-Verilated-core-instead.patch` | the same on the board and the machine |
| `0003-...-a-debug-only-latency-on-the-interrupt-pin.patch` | off unless built otherwise; see the comment on it |

Everything that makes a board a board is untouched by these.  Eight one-line
dispatchers are the whole difference on the Sun-3, because the Am9516 chain
parsing, the packing FIFO and the CSR semantics are all about the chip's
observable pins and not about how the chip is built.  What the second series
does add, and only to the RTL path, is *time*: a Verilated core advances when
it is stepped and not otherwise, so there is a catch-up to virtual time, a
microsecond bought by each register access, and a 500 us timer.  The software
chip has no time in it and none of that is created for it.

The ISA card used to be a `diff -ruN` against a QEMU 7.2.22 release tarball in
a tree of its own.  It is here now because the two boards are not as separate
as two trees implied: they model the same part behind different glue, and a
file both of them need - which is what a software 5380 turned out to be -
would have had to exist twice, in two patch formats, with nothing to report
when one copy was fixed and the other was not.  Rebasing the card onto the
newer tree cost only QOM idioms.

## `sdspi/`

One patch against ZipCPU's sdspi, whose `bench/cpp/sdspisim.cpp` is an SD card
in SPI mode written by someone else.  `cosim/scripts/build-sdspi.sh` clones the
repository into `work/sdspi-src` at a pinned commit and applies it;
`cosim/sdcheck` links the result.  It is GPLv3-or-later, which is why nothing
of it is kept here and why `make test` cannot reach any of it.

The patch is not about making our controller pass.  It is about the model
reporting the size of the file it was given: `CSD()` runs in the constructor,
before `load()` has opened the image, and is never called again, so every card
describes itself as whatever a block count of zero encodes.  And C_SIZE in a
version 2 CSD is a count of 512 KB units rather than of sectors, so writing the
sector count into it overstates the card by a factor of 1024.  The patch's own
message argues both.

Everything else our controller needed, it needed to fix in itself.  Three
things did: the power-up clocks were two short, there was no N(RC) gap before a
command, and /CS was never released between commands.  `doc/sd.md` records all
three.  They were found by the model refusing to talk to us, which is what a
second opinion is for.

## `hatari/`

An Atari TT whose SCSI controller is a Verilated wish5380 - the chip and the
disk both, since Hatari's own SCSI device is left out of the path along with
its 5380.  Applies to a checkout of Hatari's git tree, which
`cosim/scripts/build-hatari.sh` clones into `work/hatari-src`; driven by
`cosim/scripts/run-tt.py` with EmuTOS as the guest.

One patch, and it is small because the seam is: the TT reaches the chip
through eight byte-wide registers at `0xff8780`, two apart and on the odd
byte, and that is the whole of what it can do to it.  What had to be written
rather than forwarded is the DMA controller, because the TT has one and the
5380 does not - including the habit of packing bytes into longwords and
leaving up to three of them in a residue register, which EmuTOS reads back by
hand and a controller that skipped it would silently short-change.

Hatari is GPL-2+.  That is a stronger reason than usual to keep it at arm's
length: nothing of it may reach `src/` or `tb/`, which are MIT.

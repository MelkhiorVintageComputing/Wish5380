# Patches

Changes to emulators, kept as patches because the emulators are not ours and
their sources do not belong in this repository.

## `qemu/`

One series, one tree, two machines.  These apply to the **Sun-3 QEMU fork**,
because mainline has no Sun-3 machine at all.  The fork lives outside this tree
(`~/qemu-sun3` by default) and is a separate piece of work under separate
ownership; nothing here modifies it.

`cosim/scripts/build-sun3-qemu.sh` clones the fork's already-patched tree into
`work/sun3-qemu`, applies these on top, and builds `qemu-system-m68k` and
`qemu-system-i386`.  Run it, then `cosim/scripts/run-sun3.py` or
`cosim/scripts/run-cosim.py`.

The ISA card used to be a `diff -ruN` against a QEMU 7.2.22 release tarball in
a tree of its own.  It is here now because the two boards are not as separate
as two trees implied: they model the same part behind different glue and they
load the same shared library to carry it.  A file both of them need - which is
what a software 5380 is about to be - would have had to exist twice, in two
patch formats, with nothing to report when one copy was fixed and the other
was not.  Rebasing the card onto the newer tree cost only QOM idioms.

Four patches, in order:

| patch | what it does |
|---|---|
| `0001-...-let-the-MMU-reach-the-SCSI-registers.patch` | lets the MMU reach the SCSI registers at all |
| `0002-...-the-Sun-3-si-onboard-SCSI-board.patch` | the `si` board: Am9516 UDC, packing FIFO, CSR |
| `0003-...-attach-the-onboard-SCSI-board.patch` | puts one on the machine, behind `si-rtl=` |
| `0004-...-the-ISA-NCR-5380-card.patch` | the i386 card: eight ports, an interrupt, no DMA |

The first is not optional and is easy to miss.  `sun3mmu.c` carries a
whitelist of physical addresses the MMU is allowed to reach, and anything
outside it becomes a bus-error timeout *before* the access is dispatched.  A
device can be correctly modelled, correctly mapped, and still be invisible:
the PROM's probe writes one word to the board, takes a bus error, and reports
"Device not found" without ever having read a register.  That was the first
day of this work.

The second carries three decisions about the board's status register that
only a driver could settle, and all three came from SunOS rather than from
NetBSD - `DMA_CONFLICT` never set, `DMA_IP` not raised at terminal count, and
`DMA_ACTIVE` cleared when the chip stops asking rather than when the count
runs out.  Its commit message argues each; `cosim/README.md` has the
evidence.

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

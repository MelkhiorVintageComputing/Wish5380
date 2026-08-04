# Patches

Changes to emulators, kept as patches because the emulators are not ours and
their sources do not belong in this repository.

## `0001-isa-ncr5380-card.patch`

An ISA card for mainline QEMU carrying a Verilated wish5380 behind
`libwish5380rtl.so`.  Applies to `qemu-system-i386`; used by
`cosim/scripts/build-qemu.sh` and driven by the purpose-built Linux guest in
`cosim/guest/`.  The card moves every byte with the CPU, which is all an ISA
5380 card ever did.

## `sun3/`

A Sun-3/60 with the same chip on its onboard SCSI, where the bytes move by
real bus-master DMA instead.  These apply to the **Sun-3 QEMU fork**, not to
mainline: mainline has no Sun-3 machine at all.  The fork lives outside this
tree (`~/qemu-sun3` by default) and is a separate piece of work under separate
ownership; nothing here modifies it.

`cosim/scripts/build-sun3-qemu.sh` clones the fork's already-patched tree into
`work/sun3-qemu`, applies these on top, and builds.  Run it, then
`cosim/scripts/run-sun3.py`.

Three patches, in order:

| patch | what it does |
|---|---|
| `0001-...-let-the-MMU-reach-the-SCSI-registers.patch` | lets the MMU reach the SCSI registers at all |
| `0002-...-the-Sun-3-si-onboard-SCSI-board.patch` | the `si` board: Am9516 UDC, packing FIFO, CSR |
| `0003-...-attach-the-onboard-SCSI-board.patch` | puts one on the machine, behind `si-rtl=` |

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

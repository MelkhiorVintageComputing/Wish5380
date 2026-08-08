# Co-simulation

The regression in `tb/` checks the design against a model of what the drivers
do.  This checks it against drivers themselves, on three machines that use the
chip in the ways it can be used:

* **An ISA card in an i386 Linux guest** - programmed I/O, every byte carried
  across the register port by the CPU.  Linux's own `g_NCR5380` finds it and
  mounts a filesystem off the SD card model.
* **The onboard SCSI of a Sun-3/60** - real bus-master DMA, where an Am9516
  answers the chip's DRQ and moves the bytes into memory itself.  Three
  guests boot on it: NetBSD/sun3, which also builds a filesystem and writes
  to it; SunOS 4.1.1, driven by the driver Sun wrote for this board; and the
  3/60 boot PROM itself, which reads the label before either of them exists.
* **The SCSI controller of an Atari TT** - DMA again, but by Atari's own chip
  and a driver that takes no interrupts at all.  EmuTOS mounts a FAT
  filesystem off the card, runs a program out of `C:\AUTO`, and writes a file
  back.

The Sun-3's second guest earned its keep.  **Every fault the co-simulation has
found in the Sun-3 board model came from SunOS and not from NetBSD**, because
the two drivers use the hardware differently and NetBSD forgives what SunOS
does not:

| bit | what it took | what NetBSD does |
|---|---|---|
| `DMA_CONFLICT` | never set: arming the UDC before programming the chip is the normal order, and flagging it would condemn every driver | reports it and carries on |
| `DMA_IP` | not raised at terminal count: `siintr` tests it first and calls it "dma ip, unknown reason" | calls the main handler and finds the chip has interrupted too |
| `DMA_ACTIVE` | cleared when the chip stops asking, not only at terminal count | never asks for more than it will get, so its transfers always reach terminal count |

It is a separate and much slower loop and is deliberately not part of
`make test`.  Nothing in `src/` or `tb/` may grow a dependency on it - the
traffic goes the other way.

## Two chips behind the same eight registers

The two QEMU boards take `--core`, and it picks which NCR 5380 answers:

| | `--core rtl` | `--core sw` |
|---|---|---|
| the chip | the Verilated `wish5380`, in `libwish5380rtl.so` | `hw/scsi/ncr5380.c`, compiled into QEMU |
| the target | `scsi_targ.sv` and an SD card model, over a raw image | QEMU's `scsi-disk.c`, over a block backend |
| the disk | `image=` | `-drive` |
| what it proves | the RTL | the driver, and the board around the chip |
| Linux, whole boot | 160 s | 8 s |

QEMU had no 5380 at all before this: `grep` over the tree finds a Linux
bootinfo tag and a line of captured `ls` output used as test data, and
`hw/m68k/q800.c` models a Macintosh that really had one and uses an ESP
anyway.  So the software core is a genuinely independent second reading of
the same datasheet - written in C, from the same pages, against the same four
driver families - and the referee is a driver that has seen neither.  Where
they agree, the datasheet has been read right twice.

It is not a replacement for the RTL path and cannot be.  Only `--core rtl`
says anything about `src/`; `--core sw` says whether a *driver* is happy, and
it says it twenty times faster.  The useful pairing is to reach for `sw` first
when a change might have broken a driver's view of the chip, and for `rtl`
when the question is whether the hardware is right.

### Comparing them directly

```sh
cosim/scripts/diff-5380.py                    # thirteen scripted checks, then a walk
cosim/scripts/diff-5380.py --steps 0          # the scripted checks alone
cosim/scripts/diff-5380.py --seed 7 --steps 50000
```

Both cards go in **one** QEMU at different I/O ports, and the script speaks
the qtest protocol to it, issuing every access to both and comparing every
read.  The stimulus is identical by construction rather than by two scripts
being kept in step, and `-accel qtest` means the guest CPU never executes an
instruction, so a divergence is the chips and nothing else.

Thirteen scripted checks, one per trap the datasheet sets - the Initiator
Command read-back, the Target Command mask, the refused Mode write, what
reading register 7 empties and what it does not, BUSY ERROR as a level,
arbitration, the two routes to the data bus, phase match, the SCSI reset,
TEST MODE, DRQ - and then a seeded random walk over the register port for
whatever nobody thought of.

**The rule is that no target answers on either side**, because the two cores
have different ones behind them and a target that answered differently would
diverge for reasons that are not about the chip.  The software card is given
no drive; the Verilated card always carries a `scsi_targ` at ID 0 that cannot
be removed, so the harness never selects ID 0 and never lets a random write
put bit 0 into the Output Data Register.  Every ID it does select is empty on
both sides, and an empty ID behaves identically: nothing asserts BSY.

What that leaves out is the DMA handshake proper - DACK cycles and End of
Process - because the ISA card is the only board with a register window this
can drive, and an ISA 5380 card has no DMA controller in front of it.  That
half is covered by the Sun-3 co-simulation on both cores and by the fifteen
`dma_` tests in `tb/`.

It found three bugs on its first run, which is the whole argument for it.

Two were in the software model.  Its settle loop stopped as soon as no state
had changed, without checking whether the *wires* had converged - so on
leaving target mode the chip went on driving the data bus for a phase that no
longer matched.  And emptying BUSY ERROR was treated as the end of it rather
than as a level that immediately refills, which would have made
`NCR5380_intr`'s ordering pointless and let a driver that got it backwards
appear to work.

The third was in the RTL, and is the one worth the price of admission.
`bus_free` compared a *registered* count against a *current* SEL - two
different clocks in one condition - so for exactly one clock after an access
that asserted BSY and released SEL together, the chip would read "BSY has been
false for 400 ns" and "BSY is true" at the same time and start arbitrating on
a bus it was itself holding.  No driver writes that sequence, which is why
118 tests and four guests had never met it.  `bus_free` and
`bsy_settled_false` now require BSY to be false *now* as well, and
`bus_arbitration_needs_bsy_false_now_and_not_only_lately` pins it.

The Atari TT keeps the Verilated core and has no `--core`.  Hatari has no
`SCSIBus` and cannot link this model; its software comparison already exists
as `run-tt.py --stock`, which leaves our shim out of the path entirely and
uses Hatari's own 5380 and its own disk against the same image.

## Running it

```sh
cosim/scripts/check-deps.sh      # reports what is missing; installs nothing
make -C cosim/rtl                # build work/lib/libwish5380rtl.so
make -C cosim/rtl check          # drive it with no emulator in the loop
cosim/scripts/build-sun3-qemu.sh # one QEMU, both machines: m68k and i386
cosim/scripts/build-guest.sh     # an i386 Linux, and a card image for it
cosim/scripts/run-cosim.py       # the verdict, against the Verilated core
cosim/scripts/run-cosim.py --core sw   # the same, against the software one

cosim/scripts/build-hatari.sh    # the Atari TT, and the EmuTOS that drives it
cosim/scripts/run-tt.py          # its verdict
```

`make -C cosim/rtl` is only needed for `--core rtl`; the software core is part
of QEMU and is built with it.

Everything downloaded, built or generated lands in `work/`, which is not in
git; these scripts are what put it back.

`make -C cosim/rtl check` is the thing to run first when the guest misbehaves.
Between the driver, the emulated card, the shared library and the RTL there are
four places a fault can be; the self test has two, and it writes the driver's
sequences out in the open rather than leaving them inside a kernel.  Given a
disk image and a block number it will also read that block - or a run of them
- and compare it with the file, which is how a guest's complaint about its
filesystem is separated from a fault in the path that carries it.

## What it says when it works

```
[   22.346236] scsi host0: Generic NCR5380/NCR53C400 SCSI, irq 0, io_port 0x350,
               ... flags { DMA_FIXUP NO_PSEUDO_DMA }
[   22.420778] scsi 0:0:0:0: Direct-Access  DOLBEAU  WISH5380 SD CARD 0001 PQ: 0 ANSI: 2
[   29.222463] sd 0:0:0:0: [sda] 4096 512-byte logical blocks: (2.10 MB/2.00 MiB)
[   30.979728] sd 0:0:0:0: [sda] Attached SCSI disk
[   33.618797] VFS: Mounted root (ext2 filesystem) on device 8:0.
WISH5380-COSIM: init running
WISH5380-COSIM: read back 'the host put this here before the guest booted'
WISH5380-COSIM: wrote a file back
```

Every one of those lines is Linux's own code.  `g_NCR5380.c` found the card,
`NCR5380.c` arbitrated, selected and handshaked each byte, the SCSI midlayer
issued INQUIRY and READ CAPACITY, and ext2 read and wrote blocks - all of it
through the Verilated chip, its SCSI target and the SD card behind them.

`run-cosim.py` checks the log and then checks the card image, because the two
answer different questions: the log says the guest read what the host put
there, and the image says what the guest wrote came back out.

| flag | what it does |
|---|---|
| `--trace` | make the library log every register access and what the core answered |
| `--keep` | let the run change `card.img` instead of a copy |
| `--timeout N` | how long to allow the guest, which is slow |

## Why this card and this guest

Neither was the first choice, and the reasons the others fell away are worth
recording.

**The Macintosh was the target throughout the design, and is not the guest
here.**  Every Mac emulator needs a Mac ROM, which is copyrighted and cannot be
fetched by a script.  A co-simulation nobody else can run is not one.

**Hatari was blocked on SDL2 headers, and is now the third machine.**  It
models the 5380 at the register level and EmuTOS is free, so the Atari TT was
always the obvious second lineage; what stopped it was that building Hatari
needs SDL2 development headers and these scripts do not install packages.  Once
those were present it took an afternoon.  See *The Atari TT* below.

**NetBSD has no plain 5380 ISA driver.**  Its `sea` driver at
`sys/dev/isa/seagate.c` is for the Seagate ST-01/02, whose control and status
registers are the board's own and nothing like the chip's eight.  That is why
the sibling project's NetBSD guest could not be reused.

**Linux's `g_NCR5380` with `board=0` is a plain 5380 at an I/O port**, eight
registers one byte apart, reached with `inb(base + reg)` and no pseudo-DMA.
That is the simplest thing the design can present, and `rtl_top.sv` builds it
with `REG_STRIDE` of one to match - which is the one configuration `tb/` does
not cover, since everything there is built for the Mac's stride of sixteen.

**The guest kernel is i386 because `CONFIG_ISA` exists only there.**
`arch/x86/Kconfig` puts `config ISA` inside `if X86_32`, so an ISA card cannot
be probed by an x86-64 guest at all.  The host's own kernel is therefore no
use, and the guest is built rather than downloaded.

Nothing in the guest is patched.  What is chosen is its configuration:
`CONFIG_SCSI_GENERIC_NCR5380=y` rather than a module, because there is no
initramfs and anything needed to reach the root filesystem has to be built in.

## Two things about the library

**There are no guest-memory callbacks, and their absence is the point.**  The
sibling project needed them because a LANCE masters the bus and fetches its own
descriptors.  The NCR 5380 has no address counter and no byte counter and never
masters anything, so every byte crosses the register port under the driver's
own control.  A whole class of co-simulation difficulty - where the guest's RAM
is, whether it is contiguous, what an IOMMU does to it - simply does not arise.

**What arises instead is time.**  A LANCE is ready the moment it is reset; an
SD card takes about six milliseconds of simulated time to come up, and until it
has, the disk answers NOT READY.  So `wish_rtl_reset` does not return until the
card is up, which is what a machine that finishes its power-on self test before
probing SCSI would see.  After that the core is stepped a microsecond per
register access, and a timer steps it while the guest is doing something else -
waiting out a `udelay`, or spinning on jiffies.

The whole run takes a few minutes, most of it in the guest's own boot.

## The Sun-3/60

The second guest exists because the first one cannot exercise the chip's DMA
mode at all.  An ISA 5380 card has no DMA controller in front of it, so `tb/`
was the only thing that had ever driven DRQ, DACK and EOP.  A Sun-3/60 has an
Am9516 Universal DMA Controller doing exactly that, and a boot PROM that uses
it.

Mainline QEMU has no Sun-3.  The machine model lives in a separate fork under
separate ownership, which nothing here modifies; `cosim/patches/qemu/` holds
what has to be added to it and `cosim/patches/README.md` explains the shape.

That series carries the ISA card too.  Both boards model the same part behind
different glue and load the same shared library to carry it, so building a
second QEMU to hold the second board bought nothing and cost a file that had
to exist twice.  `build-sun3-qemu.sh` builds `qemu-system-m68k` and
`qemu-system-i386` from one tree.

```sh
make -C cosim/rtl                  # the shared library, as before
cosim/scripts/build-sun3-qemu.sh   # clone the fork, apply our patches, build
dd if=/dev/zero of=work/sun3/disk.img bs=1M count=16
cosim/scripts/run-sun3.py 'b sd()' -- -trace 'sun3_si_*'

# a NetBSD disk built from what NetBSD publishes, and a boot off it
cosim/scripts/make-sun3-disk.py --miniroot work/netbsd/miniroot.fs \
    --bootxx work/netbsd/usr/mdec/bootxx --out work/sun3/disk.img \
    --size-mb 320 --swap-mb 32
cosim/scripts/run-sun3.py --image work/sun3/disk.img -t 1500 -i 900 \
    'b sd() netbsd.sun3'

# SunOS, whose disk is built under TME from the install tape - see below
cosim/scripts/make-sun3-sunos-disk.sh
cosim/scripts/run-sun3.py --image work/tme-run/sun3-80s.img -t 14000 -i 5000 \
    'b sd()'
```

**The guest writes to a copy**, `<image>-run.<ext>` beside the one `--image`
names; `--keep` gives it the original instead, and `run-cosim.py` does the
same for the ISA card's `card-run.img`.  That is not tidiness: a run's writes
reach the disk image, so without the copy every run would change what the next
one started from, which is no basis for comparing two runs of anything - least
of all an intermittent fault.

Getting those writes to arrive at all took two goes, and both wrong turns are
worth knowing because each looked right.  The card is written back by
`wish_rtl_flush`, which `wish_rtl_free` calls, which the board reaches through
`qemu_add_exit_notifier` - **and an exit notifier does not run when the
process is killed with signal 9**, which is what this script used to do.  So
no Sun-3 guest's writes had ever reached an image, and `--keep` had never kept
anything.  The teardown believed it was asking QEMU to quit first, by writing
`Ctrl-a x` and sleeping 200 ms; that never had a hope either, because the
console is `-serial stdio` and not `mon:stdio`, so there is no monitor on that
line and the two bytes went to the guest as data.  The first fix replaced only
the 200 ms with a proper wait and changed nothing, because the quit request
was never delivered.  It sends `SIGTERM` now - which QEMU turns into an
orderly shutdown that does run its exit notifiers - and waits for the process,
draining the console so a QEMU blocked on a full pty cannot stall its own
teardown.  `SIGKILL` remains the backstop.

Checked against a full boot rather than a short one, which matters: a run
stopped shortly after root is mounted changes 152 bytes and exercises almost
nothing of a 170000-block writeback.  A boot all the way to the login prompt
changes **1077695**, across logs, `utmp`, `mtab`, inode access times and the
cylinder-group summaries, and the flushed image still holds its label, its
partition table, its boot block, its `fstab` and its root directory, with
`/vmunix`, `/usr/etc/init` and `/dev/console` all present and 30.1 MB used of
62.3.  The original's checksum is unchanged; with `--keep` the named image
changes and no copy is made.

One consequence of the flush now working.  The run copy left behind is a
**crash-consistent snapshot of a running system** - SunOS was multi-user when
`SIGTERM` arrived, so its filesystem is legitimately dirty.  That is correct
and not a defect, and it is harmless only because every run starts from a
fresh copy of the original.  Booting that leftover copy directly, with fsck
switched off by the fstab pass field, is precisely the situation that produced
`panic: ialloc: dup alloc` under *Where it stops*.

A run that has to survive a long silence - `fsck` says nothing for minutes -
wants `-i` raised; it gives up after a minute by default, and `-t` ends the
whole run after ten.  And if something else on the machine sweeps up emulators
with `pkill -f qemu-system-m68k`, point `--qemu` at a copy under another name.

**Both defaults are far too small for a guest that boots an operating
system**, and the two guests are not in the same class:

| guest | what works | how far it gets |
|---|---|---|
| NetBSD 10.1 INSTALL | `-t 1500 -i 900` | root on `sd0a`, ffs, sysinst's prompt and a root shell |
| SunOS 4.1.1 | `-t 14000 -i 5000` | all of `/etc/rc` and a login prompt, in about four hours |

SunOS is the expensive one and the kernel load is why: `vmunix` is 893160
bytes and every one of them crosses the Verilated chip, which on its own
outruns a total of 1500 seconds.  A run cut short there looks like a hang and
is not one - the last thing on the console is `Boot: vmunix` and a spinner,
and the spinner writes no newline, so a line-based idle detector sees silence
while the machine is working.  Raise `-i` before concluding anything from it.

Guest time counts guest instructions by default; `--icount` changes the rate
and `--icount 0` hands it back to the wall clock.  There is a section on why
below, and it is not a detail.

The positional arguments are typed at the PROM's `>` and nowhere else.  Once
the PROM has handed over, what to say depends on what the guest asks, which is
what `-s 'PATTERN:=TEXT'` is for - the rules fire in the order given, once
each, on a regular expression matched against the console as it arrives.  A
run that drives a newfs from inside NetBSD is a stack of those.

What a working run says:

```
>b sd() netbsd.sun3
Boot: sd(0,0,0)netbsd.sun3
>> NetBSD/sun3 ufsboot [1.13 (Mon Dec 16 13:08:11 UTC 2024)]
1522676+76500 [167024+154826]=0x1d5414
starting program at 0x4000
[   1.0000000] NetBSD 10.1 (INSTALL) #0: Mon Dec 16 13:08:11 UTC 2024
[   1.0000000] Model: sun3 60
[   1.0000000] si0 at obio0 addr 0x140000 ipl 2: options=0xf
[   1.0000000] scsibus0 at si0: 8 targets, 8 luns per target
[   3.2100030] sd0 at scsibus0 target 0 lun 0: <DOLBEAU, WISH5380 SD CARD, 0001> disk fixed
[   3.2500030] sd0: 16384 KB, 16 cyl, 64 head, 32 sec, 512 bytes/sect x 32768 sectors
[   3.5400030] boot device: sd0a
[   3.7000030] root on sd0a dumps on sd0b
[   3.8400030] root file system type: ffs
```

Every line of that is somebody else's code.  The PROM found the board and read
its label; NetBSD's `bootxx` loaded `ufsboot` from a list of raw block numbers;
`ufsboot` read the filesystem and pulled nearly two megabytes of kernel across
the chip's DMA port; and then NetBSD's own `si` driver - the one written for the
real board in 1994 - probed the bus, arbitrated, selected, sent INQUIRY, and
attached the disk - and then mounted its root filesystem off it.

Underneath, each of those transfers looks like this:

```
sun3_si_chain    table at 0xf0400c rsel 0x0182 -> addr 0xf00000 count 256 words
sun3_si_dma_done residual 0 bytes, csr 0x0003, first four bytes 0xdeadbeef
```

The PROM built a six-word chain table in DVMA memory and told the Am9516 to
fetch it; the UDC read it back through the Sun-3 MMU, found a 256-word receive
into 0xF00000, and moved 512 bytes from the Verilated chip into the guest's
memory with nothing left over and no bus error - the bytes coming from an SD
card image by way of the SCSI target, across a REQ/ACK handshake the driver
never touched.

### Where guest time comes from, which turned out to matter a great deal

The core runs far slower than the part it models, so the question of what the
guest thinks the time is decides whether any of this works.  Left alone, QEMU
runs its virtual clock off the wall clock: every blocking access into the
Verilated chip lets guest time run away, the machine's own driver sees each
command take an age, and the board's catch-up then spends simulation on time
the guest never spent.  A boot to the NetBSD shell took about fifty minutes
and moved bytes at 1.2 kB/s.

`-icount shift=N` makes guest time count guest instructions instead - one
instruction every 2^N nanoseconds - which is what a driver's timeout measures
on real hardware.  The same boot then takes about five minutes.  The runner
passes `shift=8` by default and `--icount 0` turns it off.

**Eight is not a tuning constant, it is the machine.**  One instruction every
256 ns is about four MIPS, which is a 20 MHz 68020.  At `shift=6` the guest
runs four times faster than the real one while the chip does not, and NetBSD's
probe of the disk's geometry starts failing - `sd0: drive offline`, then a
fabricated geometry - on the same image that is read perfectly at `shift=8`.
That is worth stating plainly because it looked for a while like a
size-dependent capacity bug.  Every failing run happened to be a big disk
*and* `shift=6`:

| disk | guest clock | what NetBSD made of the disk |
|---|---|---|
| 16 MB | the wall clock | `16384 KB, 16 cyl, 64 head, 32 sec` |
| 16 MB | `shift=6` | `drive offline` |
| 16 MB | `shift=8` | `16384 KB, 16 cyl, 64 head, 32 sec` |
| 32 MB | `shift=6` | `drive offline` |
| 320 MB | `shift=6` | `drive offline` |
| 320 MB | `shift=8` | `320 MB, 320 cyl, 64 head, 32 sec, 512 bytes/sect x 655360 sectors` |

The two only came apart on the third and sixth rows.  The chip and the target
answer READ CAPACITY correctly at 655360 blocks in the regression too, which
is what said the fault was not there; and the last row is the other thing that
needed proving, because a disk big enough for a SunOS 4.1.1 root and `/usr` is
twenty times the one NetBSD boots from.

### It writes, too

Booting only ever reads.  The first thing that makes a real driver write is a
filesystem, so `make-sun3-disk.py` leaves a spare partition and the run drives
NetBSD into building one on it:

```
# newfs -s 16384 /dev/rsd0d
/dev/rsd0d: 8.0MB (16384 sectors) block size 4096, fragment size 512
	using 4 cylinder groups of 2.00MB, 512 blks, 960 inodes.
super-block backups (for fsck_ffs -b #) at:
32, 4128, 8224, 12320,
# mount /dev/sd0d /mnt
# dd if=/netbsd.sun3 of=/mnt/k bs=8192 count=8
8+0 records in
8+0 records out
# cksum /mnt/k
3755572741 65536 /mnt/k
# umount /mnt
# mount /dev/sd0d /mnt
# cksum /mnt/k
3755572741 65536 /mnt/k
```

The unmount is the point of the exercise: it forces everything back through
the chip, and the checksum after the remount is computed from blocks that have
been written to an SD card image and read back off it.  Until this, nothing
had ever made the design write under a driver that was not ours.

Both consoles are kept whole rather than in extracts:
[`netbsd-sun3-boot.txt`](netbsd-sun3-boot.txt) and
[`netbsd-sun3-newfs.txt`](netbsd-sun3-newfs.txt), each running to the end of
what the machine printed rather than stopping at the good part.

### Five things this cost, worth knowing before the next one

**A device can be perfectly modelled, correctly mapped, and still invisible.**
The Sun-3 MMU checks every translated physical address against a list of what
exists on the machine and turns anything else into a bus error before the
access is dispatched.  SCSI was not on the list.  The PROM's probe wrote one
word, took a bus error, and said "Device not found" without ever reading a
register - which looks exactly like a device that is not there.

**The VME-only registers are not optional.**  One driver serves both the
onboard and the VME board and writes them unconditionally; the 3/60 PROM puts
`VME_SUPV_DATA_24` in `si_iv_am` at offset 0x1e before it does anything else.
A model that stopped at `si_csr`, where the onboard register list stops, failed
the probe outright.

**An Am9516 address is not a 32-bit number.**  It is a pair of words whose high
word carries A23-A16 in its *high byte*, the low byte being reference and
control.  Read flat, the chain table address 0xF04000 becomes 0x004000, which
is not wrong data but a translation failure - so it surfaces as a DMA bus error
rather than as garbage, and points away from itself.

**A DMA controller waits for DREQ; it does not sample it.**  The board model
looked at DRQ once per guest register access and moved a byte if it happened to
be asserted, which made a transfer advance about a byte per poll.  The PROM
waits for the chip's interrupt with a finite count and gave up long before the
data arrived.  What found it was asking `tb/` the question the PROM had already
answered - does the chip interrupt at the end of a transfer when the driver has
*not* enabled the End of Process interrupt?  It does, and
`dma_a_transfer_ends_by_interrupting_without_the_eop_enable` now says so, which
is what moved the search off the RTL and onto the board.

**Reaching terminal count is not an interrupt**, and the guest that was running
could not tell us.  The board raised `SI_CSR_DMA_IP` at the end of every
transfer, because both drivers arm the UDC's channel interrupt first and it
seemed to follow.  NetBSD tolerates it: `si_intr` sees the bit, calls the main
handler, finds the 5380 has already interrupted too, and gets on with it.  Sun's
own driver does not - `siintr` tests `SI_CSR_DMA_IP` before anything else and,
on this board, prints *"dma ip, unknown reason"* and fails the command.  A
transfer that always set it would have failed every command SunOS ever issued,
so the hardware does not set it, and now neither do we.  This one was found by
reading a third driver rather than by running anything, which is the argument
for keeping three: the guest you have will not complain about the thing it
happens to forgive.

### The disk

`cosim/scripts/make-sun3-disk.py` builds a bootable NetBSD/sun3 disk out of
parts NetBSD publishes, and assembles it here rather than installing it inside
the machine, because installing it inside the machine needs a machine that
already boots.

```sh
cd work/netbsd
curl -O https://cdn.netbsd.org/pub/NetBSD/NetBSD-10.1/sun3/installation/miniroot/miniroot.fs.gz
curl -O https://cdn.netbsd.org/pub/NetBSD/NetBSD-10.1/sun3/binary/sets/base.tgz
gunzip -k miniroot.fs.gz
tar xzf base.tgz ./usr/mdec
cd ../..
cosim/scripts/make-sun3-disk.py --miniroot work/netbsd/miniroot.fs \
    --bootxx work/netbsd/usr/mdec/bootxx --out work/sun3/disk.img
```

What that produces, and why each piece is where it is:

| blocks | what |
|---|---|
| 0 | the Sun label - geometry and the partition table |
| 1-15 | `bootxx`, which the PROM loads whole and jumps into |
| 32 on | NetBSD's miniroot, holding `/ufsboot` and `/netbsd.sun3` |

`--size-mb` and `--swap-mb` decide the rest of the label: `a` is the miniroot,
`b` is swap, `c` is the whole disk as the Sun convention requires, and `d` is
whatever is left over.  Neither `b` nor `d` is needed to boot and both are
needed to do anything afterwards - `d` is where a `newfs` goes, which is the
first thing that makes a real driver *write*.

**`bootxx` is not a filesystem reader.**  It carries a list of raw block
numbers, and copies those blocks into memory and jumps to them; that is how
the second stage gets loaded before anything in the machine understands FFS.
On a real installation `installboot(8)` asks the running kernel where
`/ufsboot` lives and patches the list in.  Here `cosim/scripts/ffs.py` reads
the miniroot's own inode for it and we patch it ourselves - which needs a
filesystem *reader* and not a writer, and the label and boot blocks go in the
sixteen kilobytes the filesystem already reserves ahead of its superblock.

`ffs.py` is checked against itself: the `/ufsboot` it extracts from the
miniroot is byte-identical to the `usr/mdec/ufsboot` in the distribution, and
the `/netbsd.sun3` it extracts - which is large enough to need the indirect
blocks - is a valid m68k ELF that says `NetBSD 10.1 (INSTALL)`.

### SunOS 4.1.1, and a disk we could not build ourselves

The third guest is Sun's own operating system, driven by Sun's own `si` -
the driver whose source is in `doc/drivers/SunOS34/`.  It boots:

```
>b sd()
Boot: sd(0,0,0)
root on sd0a fstype 4.2
Boot: vmunix
SunOS Release 4.1.1 (GENERIC) #1: Sat Oct 13 06:05:48 PDT 1990
mem = 4096K (0x400000)
si0 at obio 0x140000 pri 2
st0 at si0 slave 32
sr0 at si0 slave 48
sd0 at si0 slave 0
sd0:  <Awesome cyl 2046 alt 2 hd 16 sec 16>
le0 at obio 0x120000 pri 3
root on sd0a fstype 4.2
swap on sd0b fstype spec size 20480K
init is /usr/etc/init
```

Every line of that is Sun's, from 1990.  The PROM read the label off the
chip; the boot block SunOS's own `installboot` wrote loaded `/boot`; `/boot`
pulled 1.28 MB of kernel across the Am9516's DMA path; and then `si0` - the
driver in `doc/drivers/SunOS34/sundev/si.c` - probed the chip, walked all
seven targets, read the disk's label and mounted a root filesystem off it.

**The disk had to be made by SunOS.**  NetBSD's boot blocks we can install
ourselves, because `bootxx` carries a list of raw block numbers and
`ffs.py` can read the inode that says what they are.  SunOS 4.1.1's
`installboot` does the same job - its strings give it away: *"Block
locations:"*, *"The boot program has too many discontinuous blocks"*, *"Boot
checksum: 0x%x"* - but the array it patches, `_blknos`, is `N_BSS`, so there
is nothing in the file to patch.  Writing a SunOS boot block means
reverse-engineering an m68k binary.

So SunOS writes it for us.  TME - the other Sun-3 emulator, and not ours -
runs the 4.1.1 installer against a disk image, its own `installboot` puts the
boot block on, and the finished image is what QEMU boots on the Verilated
chip.  `cosim/scripts/run-tme.py` drives it.  Nothing in `src/` or `tb/`
depends on TME, and it is not in `make test`.

The image is named by the TME configuration rather than by any option, so it
is wherever that says - `disk0` gets a filename relative to the directory TME
runs in.  Its first block is a Sun label and begins with the disk's own name
in ASCII, which is the quickest way to tell a finished image from an empty
one.

### The disk that is used now, and why it is small

`cosim/scripts/make-sun3-sunos-disk.sh` builds it end to end from the tape.
It is a **Quantum ProDrive 80S** - the geometry `/etc/format.dat` gives for
that drive, 832 data cylinders by 6 heads by 34 sectors, so 204 sectors a
cylinder and 82.9 MB.  Its *sample partition table* in the same file is not
used: that block assumes 256 sectors per cylinder and does not reconcile with
the `disk_type` printed directly above it.

| part | cyl | blocks | size | use |
|---|---|---|---|---|
| a | 0 | 137088 | 66.9 MB | `/` |
| b | 672 | 32640 | 15.9 MB | swap, and the miniroot the installer plants |
| c | 0 | 169728 | 82.9 MB | whole disk |

Small on purpose.  `newfs` scales the inode count with the size of the
filesystem and fsck's cost is mostly the inode scan, so 62 MB carries about
29000 free inodes where 200 MB carried about 100000 - for the same 30 MB of
content.

**There is one filesystem and there cannot be two.**  The sun3 proto root on
this tape is a *diskless client's*: its stock `/etc/fstab` is
`server:/rootdir / nfs`, its `/sbin` is an empty directory, and `/etc/mount`,
`/etc/fsck`, `/etc/umount` and `/bin` are all symlinks into `/usr`.  Nothing
on it can run before `/usr` is mounted.  A diskless kernel mounts `/usr` over
NFS from bootparams before it execs init; a local disk has no equivalent, so
a split install dies in `panic: icode` having mounted nothing.  Seeding
`/sbin` from the miniroot does not rescue it either - every miniroot binary is
dynamically linked against `/usr/lib/ld.so`.  This is a property of the media,
not a partitioning mistake, and it is worth knowing before anyone tries the
conventional `a`/`b`/`g` layout again.

So there is no `/usr` line in fstab.  What there is instead is one changed
line in `/etc/rc.single`, whose `mount -o remount /usr` fails on a machine
with no separate `/usr` and answers `exit 2` - which `/etc/rc.boot` turns into
its own exit status and init reads as a failed single-user setup, stranding
the boot.  An earlier image worked around that with an fstab entry pointing
`/usr` at the root's own device; that is a fiction, and it also made fsck
check `sd0a` twice under two names.

**fsck is switched off in fstab**, by the sixth field - the pass number -
which `fsck -p` uses to decide what to check at all.  Walking a 62 MB
filesystem's inodes at the ~12 KB/s the Verilated chip sustains takes about
2700 s, and the `sd0: I/O request timeout` fault fires during it, so the check
was the whole of what stood between a boot and a prompt.  `/fastboot` is the
other switch and is worse: `/etc/rc` removes it after one boot.  A clean
shutdown buys nothing at all, because 4.1.1's `fsck` is 4.3BSD-derived and
carries no clean-flag logic - `FILE SYSTEM STATE`, `FSCLEAN` and `fs_clean`
are all absent from the binary - so it runs five phases however the filesystem
was unmounted.  Skipping it is safe only because the image is rebuilt from the
tape and restored from a pristine copy rather than maintained in place.

With that done **SunOS 4.1.1 boots to a login prompt off the Verilated
chip**, all of `/etc/rc` and out the other side:

```
starting rpc and net services: portmap ypbind keyserv ypupdated routed.
starting additional services: biod.
starting system logger
starting local daemons: auditd sendmail statd lockd
link-editor directory cache
clearing /tmp
standard daemons: update cron.
starting network daemons: inetd printer.
Thu Aug  6 18:51:07 GMT 2026

Amnesiac login:
```

`Amnesiac` is SunOS's own default hostname when it cannot find one, which is
correct for a machine with no `/etc/hostname.le0` and no NIS.

It takes about four hours, and most of that is not storage: the network
daemons each burn their timeouts against a machine with no network, and the
`ypbind`, `sendmail` and `automount` stages account for the long silences.
Two runs from the same pristine image tracked each other closely - root
mounted at +138 s, `/etc/rc` starting at ~+2250 s, the local daemons at
~+4330 s, the network daemons at ~+10500 s.

**The prompt arrives with its high bit set** - `A\355\356e\363i\341c...` in
the raw log - and it is not corruption.  `getty` sets the console to seven
bits and a parity bit from `/etc/ttytab`, the emulated line carries all eight
through, and the parity bit lands in the top one.  Mask it off and the text is
clean ASCII.  Worth knowing before anyone reads the tail of a log and concludes
the data path is dropping bits.

Seven `I/O request timeout`s and six `lost interrupt`s along the way, all of
them recovered from.  They cost real time and they are the first entry under
*Where it stops*; they did not stop the boot.

Five things about that install are worth writing down, because none of them
is in anyone's instructions:

* **TME 0.8 has no `/dev/ptmx` support**, whatever its example configuration
  says - there is no `ptsname()` call anywhere in it.  The pty has to be made
  by the driver and the slave's path substituted into the config.
* **Its tape only answers to the EMULEX identity** the example config's
  comment tells you to remove.  Without `vendor EMULEX product "MT-02 QIC"`
  the PROM prints `Boot: st(0,0,0)` and hangs for ever.
* **`make -j` breaks it**: `all-local` copies `tme_generic.la` before it is
  linked, and `tme-preopen.txt` is *appended to* on every build, so a stale
  entry from a previous configure breaks the link afterwards.
* **`zcat` does not leave a tape at the filemark**, so the next set has to
  rewind and seek to its own file number rather than read straight on.
* **The proto root ships `/dev/MAKEDEV` and no device nodes**, and SunOS's
  `tar` refuses to archive special files, so the miniroot's `/dev` cannot be
  copied across.  Without them `init` starts, cannot open a console, and says
  nothing at all.

And one about the layout: 4.1.1's kernel execs `/usr/etc/init`, and nothing
mounts a separate `/usr` before that happens, so a split install panics with
`panic: icode`.  Everything goes in one filesystem, which meant rewriting the
label's partition table - and our own Sun-label code read back what SunOS had
written, checksum and all, which is a free cross-check on
`make-sun3-disk.py`.

### The bug SunOS found, and the two wrong turns on the way to it

The first time SunOS reached the disk it reset the bus instead of using it:

```
si0:  resetting scsi bus
	csr= 0x8003  bcr= 20  tcr= 0x1
	cbsr= 0x6d (STATUS)  cdr= 0x0  mr= 0x2  bsr= 0x0
	target= 0, lun= 0    DMA addr= 0x4c  count= 20 (56)
	cdb=    12  0  0  0  38  0
	last phase= 0x4 (DATA IN)   56
```

That is an INQUIRY - `cdb 12` - asking for 56 bytes.  A SCSI-1 disk answers
with the standard 36 and goes to STATUS, so **the transfer ended on a phase
mismatch with twenty bytes still on the count**, which is a thing NetBSD
never does: it asks for exactly what it is going to get, so every transfer
this design had seen until now ended at terminal count.

The dump says `bsr= 0x0`, and the obvious reading is that the chip failed to
raise the phase-mismatch interrupt.  **That reading was wrong, and a test
said so before any RTL was touched** -
`dma_a_target_that_answers_short_ends_the_transfer_and_interrupts` sets up
exactly this exchange against the real target and passes: 36 bytes move,
PHASE MATCH drops, IRQ rises.  The dump is state printed by `si_reset`, which
has already read register 7 and cleared the latch.  Had the test not been
written first, a correct chip would have been "fixed".

What settled it was the board's own stall trace:

```
sun3_si_stalled stalled with 20 bytes left: csb 0x6d bsr 0x10 drq 0
sun3_si_reg_read addr 0x18 size 2 -> 0x8203      (over, and over, and over)
```

`bsr 0x10` is the interrupt, plainly there.  `csr 0x8203` is what the driver
kept reading, and bit fifteen of it is `DMA_ACTIVE`.  `si_cmdwait` waits for
that bit to *clear* - "DMA_ACTIVE still on" is its complaint - and the model
dropped it only at terminal count, which a short answer never reaches.  On
the real board the UDC is fed by a FIFO that has stopped asking and stops
with it, leaving the byte count as the residual.  The fix is in
`sun3_si_pump`, and the traces afterwards read:

| residual | transfers |
|---|---|
| 0 bytes | 534 |
| 14 bytes | 3 |
| 20 bytes | 1 |
| stalls | 0 |

Knowing *when* the chip has stopped asking took three tries, and the two wrong
answers are the useful part, because each is right on its own terms:

| condition | what it breaks |
|---|---|
| the chip has interrupted | an interrupt is a **latch** - it stays asserted until the driver reads register 7, so one left over from the command before is asserted when the next transfer starts, and the pump aborts a transfer that has not begun |
| the phase no longer matches | the phase is **momentary** - it is false between arming the UDC and writing the Target Command Register, which wedges the PROM's first read, and it flickers mid-transfer, which ended the boot block read early and got `scsi: dma never completed` |
| either of those, on the four-millisecond timeout | the same two wrong answers by another route |
| both, after the chip has asked once | boots |

Each wrong version failed differently and visibly - `residual 32` of a 32-byte
read, `count= 8192 (8192)`, `Boot: load failed` - which is the argument for
running the thing after every attempt rather than reasoning about it twice.

The short ones are the driver asking for more than the target has; everything
else still moves whole.

The self test is the other half of that investigation and is worth knowing
about before the next one.  It takes an image and a block number now:

```sh
work/bin/rtl-selftest work/lib/libwish5380rtl.so disk.img 313312
work/bin/rtl-selftest work/lib/libwish5380rtl.so disk.img 313312 16
```

It reads that block - or a run of them - through the library and compares it
with the file, which is how the storage path was ruled out here without an
emulator in the loop at all.  Nothing else in the self test goes past block
three, and a disk with an operating system on it is read a long way from
block zero.

### Where it stops

Five faults are on the record and none is fully explained, though the first
is now narrowed to one exchange.  All but the first are intermittent, and they are written down here
rather than left in a commit message because an unexplained fault that nobody
can find again is worth less than one that is.

**`sd0: I/O request timeout` / `si0: lost interrupt`**, once or twice in a
boot, reproducibly.  It is named after the wrong thing: the interrupt is not
lost, and the command in the dump is not the one that failed.

Tracing the level-2 line, the guest's clock and the core's clock together,
and finding the failing CDB in the same stream, the sequence is:

1. the data phase completes perfectly - 8192 bytes, 16 µs of guest time,
   `residual 0`;
2. the interrupt is raised in the *same guest microsecond* as the completion
   and the driver acknowledges it;
3. the driver runs its end-of-command sequence - `TCR` to unspecified,
   `MR = 0`, a FIFO reset;
4. the target sits in STATUS with REQ asserted, holding BSY;
5. **nothing happens for 117 seconds of guest time**, with both clocks
   advancing together, so it is real elapsed time and not a pacing artefact;
6. the disk driver's request watchdog fires and `si` dumps whatever command
   is current - which is why the CDB in it looks innocent.

So the fault is a stall in the STATUS handshake *after* the data phase, not
anything to do with delivering an interrupt.  Note step 3: with `MR_DMA`
clear the chip no longer interrupts on a phase change, so a driver waiting
for an interrupt about STATUS rather than polling for it would wait for ever.

**The software core reproduces it, and that is the most useful thing now
known about it.**  `--core sw` puts an independently written C model of the
same part behind the same board, with no Verilator, no shared library and no
pacing anywhere in the picture - and SunOS reports the same fault, in the same
place, on the same 8192-byte transfers.  That removes two of the four suspects
this has always had.  It is not the RTL, and it is not the pacing that keeps
guest time and Verilated time in step; the board model, the driver's
expectations of it, or something both chip models read the same way out of the
same datasheet is what is left.

Two things did turn out to be genuinely missing from the board, both found by
building the software path and both about who wakes the board up: the chip's
interrupt pin was polled rather than listened to, so an interrupt raised
between two register accesses waited for the driver to touch something; and a
transfer that ends short interrupts rather than raising DRQ, so the pump had
to be re-entered from the interrupt as well.  Connecting those cut the reports
on a SunOS boot by a third.  Neither is the fault above - it survives both -
but the RTL path had been hiding both behind its 500 µs pacing timer, which
called the pump and re-read the interrupt whatever else was going on.

NetBSD, for what it is worth, is clean on the software core: a whole boot to a
shell is 600 commands and 569 DMA transfers with `residual 0` on every one.
Which is the same asymmetry the table at the top of this file describes, and
the reason the second guest earns its keep.

**But the software core is worse than the RTL under SunOS, not better, and
that is the state to be honest about.**  On `--core rtl` SunOS reaches
`Amnesiac login:` in about 14000 s with four to seven of these reports on the
way.  On `--core sw` it gets through fsck, single-user, `rc`, the rpc and net
services, the system logger and `inetd`, stalls at the end of `rc` around the
printer step, and then produces nothing but fault reports - 2269 of them in
2000 s, having never reached multi-user.  So the fault is *more* frequent with
the software chip and eventually fatal, where with the Verilated one it is
survivable.

That does not weaken the conclusion above - the fault plainly is not the RTL
and not the pacing, since it happens without either - but it does mean the two
chips differ somewhere, and the RTL is the one with the tests behind it.

**The differential harness was built for exactly this, found three real bugs,
and did not fix it.**  `diff-5380.py` is described above; it turned up a
settle loop that stopped before the wires had converged, a BUSY ERROR latch
treated as an edge, and a one-clock race in the RTL's bus-free filter.  All
three are fixed.  SunOS on `--core sw` still stalls in `rc` in exactly the
same place, with 1820 reports in 2000 s where before it was 2269 - a real
reduction, and nowhere near enough to matter.

That is worth stating plainly because it narrows the search rather than
widening it.  The harness compares the register port and the bus engine, and
those two now agree over eight seeds of a twenty thousand step walk.  What it
explicitly does *not* reach is the DMA handshake - DACK cycles and End of
Process - because the ISA card is the only board with a register window qtest
can drive and an ISA 5380 card has no DMA controller in front of it.  The
fault is on 8192-byte DMA transfers.  So the remaining suspect is the half
that has never been compared, and the next step is a harness that can reach
it: driving the Sun-3 board's register block and its DVMA memory from qtest,
so the Am9516 chain, the packing FIFO and the DACK path are exercised against
both cores the way the register port now is.

The regression covers the textbook version of that sequence -
`dma_a_block_reads_back_through_a_real_engine` ends a DMA read exactly this
way, clears `MR` and then reads STATUS and MESSAGE IN - and it passes, as
does `dma_the_chip_interrupts_promptly_after_the_last_byte`, which measures
the chip raising IRQ **70 ns** after the last byte.  The driver's own source settles half of it.  `doc/drivers/SunOS412/si.c` is
now here, and `si_deque` shows that **`lost interrupt` is printed after a
request has already timed out**, when the CSR happens to show `SBC_IP`: it is
the driver's explanation for the timeout, not an independent fault, and the
command in the dump is simply whichever was current.  `siintr` handles one
phase per interrupt and arms for the next with `TCR = TCR_UNSPECIFIED`,
`MR |= SBC_MR_DMA` and *then a poll of the bus status for REQ* - and both
halves of what that needs from the chip are now pinned by tests in
`tb/cpp/tests/test_dma.cpp`, because the mismatch interrupt is an edge and a
request already standing produces nothing.  The poll is what covers that, and
it is the driver's, not ours.

The poll is not the problem either, and tracing the register *reads* settles
it.  The whole exchange after the data phase is clean:

```
CSB 0x6d   STATUS, REQ      the driver reads the status byte, 0x00
ICR 0x10 / 0x00             ACK
CSB 0x4d -> 0x7d            REQ drops, target moves to MESSAGE IN and asks
TCR=4, read reg 7, MR=2     the arm
CSB 0x7d                    the poll - and it sees REQ
...message byte read and acknowledged...
CSB 0x00                    BUS FREE: the target disconnected
ODR 0x80, MR=1, ICR 0x40    arbitration for the next command
```

Status, message, disconnect: the command finishes properly and the driver
goes straight on to the next one.  **The stall is between two commands, not
inside either**, and the whole window holds ninety-five register accesses -
the driver is not spinning on anything, it is idle.

So nothing on the SCSI side is wrong.  What is left is guest timekeeping: the
virtual clock advancing about two minutes - 118.5 s in one run, 117.4 s in
another - while the machine is idle between commands, with `catch_up`
faithfully burning the same two minutes of core time to follow it.  Two
minutes trivially exceeds `sd`'s I/O timeout, which is what fires, and `si`
then prints its "lost interrupt" explanation.

Pinning that wants a timestamp on *every* register access rather than on
chain and completion events, so the jump can be located between two adjacent
accesses instead of inferred from the space between them.

It still fires on the small ProDrive 80S disk, four times in one boot with
three `lost interrupt`s beside them, and the driver recovered from every one.
One of those dumps carries `last phase= 0x40 (DATA OUT) 8192` and a ten-byte
CDB, where every earlier sighting had `0x4 (DATA IN)` and a six-byte READ.
That *hints* the stall is not read-specific and the 8192-byte transfer is the
common factor - but it is only a hint, because the paragraph above is exactly
about the dump reporting whichever command was current rather than the one
that failed, and the phase ring is the same dump.  Worth checking against a
per-access timestamp; not worth believing before that.

**`panic: ialloc: dup alloc`**, once, on a filesystem that is not corrupt.
The panic named inode 1565 and killed the boot 20 s after `checking
filesystems`; `fsck -y` under TME afterwards walked all five phases, changed
nothing and reported `3571 files, 30715 used, 33108 free`.  The same image
booted past that point both before and after.  It is only visible with fsck
switched off in fstab - not because skipping the check causes it, but because
the check had been repairing whatever it is, silently, every boot.  Nothing
rules out the chip here: a lost or misapplied write during the boot's first
writes would look exactly like this, and the damage cannot have been inherited
from an earlier run.  It could not then, because the writeback was broken at
the time and no run had ever changed an image; and it cannot now, because
every run starts from a fresh copy.  Four later boots from the same pristine
image all reached a login prompt without it, so it is an outlier rather than
something that happens on the way past `checking filesystems`.

**`Can't invoke /usr/etc/init, error 2`**, on an image where that file
demonstrably exists - `ffs.py` reads it straight out of the disk at inode
52819, and the same image resolves it under TME and on two runs out of three
here.  The kernel's other complaints in that list are correct: `/sbin/init`,
`/etc/init` and `/bin/init` really are absent.  So one lookup fails that
should not, intermittently.  It is not the storage path: the self test reads
the blocks `/usr/etc` lives in - 313312, singly and sixteen at a time -
byte-identical to the file, and the DMA traces from a failing boot show no
short transfer and no stall.  What is left is the pacing or something above
the chip, and it wants a run with the register trace on and the boot driven
to that exact lookup.

**`si0: cmd timeout, targ=0, lun=0`**, once, exactly sixty seconds of guest
time after root was mounted - the command's own timeout, so the command
produced nothing at all rather than being slow.  NetBSD retried, the system
carried on into userland, and a later traced boot of the same image did not
reproduce it and showed no pump stall anywhere.  A command that returns
nothing is either one the target never answered or one the pacing starved, and
those are different bugs.

**`Exception 0x7C at 0x0FEF8F44`**, once, immediately after `bootxx` had
loaded the second stage and jumped into it.  Vector 31 is the level 7
autovector, which on this machine is the memory-error and clock line; the
address is inside the PROM.  The same image booted cleanly on the next run.
The machine model already takes far more level 7s than a real 3/60 would,
which is a property of the fork and not of this board, so the first thing to
establish is whether a run without the board takes them too.

The last two appeared before the instruction counter was turned on, when guest
time was following the wall clock and every command looked minutes long to the
guest.  Neither has appeared in any run since.  That is a handful of runs and
not a campaign, so it is a reason to retest rather than a diagnosis - but the
one fault that *was* chased to the bottom, `sd0: drive offline`, turned out to
be exactly this and nothing else, which is why they are filed together.

## The Atari TT

A third machine, and the cheapest of the three to reach - which is the first
thing worth recording about it, because the Sun-3 cost five days and this cost
one afternoon.

```sh
cosim/scripts/build-hatari.sh                   # Hatari, patched, plus EmuTOS
cosim/scripts/run-tt.py                         # boot it
cosim/scripts/run-tt.py --stock                 # Hatari's own 5380, to compare
cosim/scripts/run-tt.py -w 'drives:      ABC'   # exit 1 if it does not mount
```

### Why it was cheap

Everything that made the Sun-3 expensive is absent here.  There is no MMU with
a whitelist of physical addresses to be added to, no DVMA and no IOMMU, no boot
PROM to be argued with, no proprietary install media, and no second emulator
needed to build a disk that the first one could then boot.  Above all there is
**a free operating system that a script can fetch**: EmuTOS is GPL and its
release images are a download away, which is the exact criterion that ruled the
Macintosh out and sent this project to a Sun in the first place.

The emulator was cheap too.  Hatari models the 5380 at register level already -
`src/ncr5380.c`, derived from WinUAE - so the seam was there to be cut rather
than built.  The whole change is one new file and eleven lines in theirs.

### What the machine looks like from the chip's side

The TT presents the eight registers at `0xff8780`, **two bytes apart and on the
odd byte**.  That is a third spacing after the Macintosh's sixteen and the ISA
card's one, and it costs the design nothing: `REG_STRIDE` already parameterises
it and Hatari's own decode - `addr = IoAccessBaseAddress / 2 & 0x7`, taken only
on odd addresses - undoes it before the library ever sees a register number.

In front of the chip is Atari's DMA controller, at `0xff8701..0xff8715`: four
bytes of address, four of count, a control byte whose bit 0 is the direction
and bit 1 the enable, and a four-byte residue register.  It is a third distinct
DMA arrangement after the Sun's Am9516 and the ISA card's nothing-at-all, and
it has one habit neither of the others has: **it packs bytes into longwords and
writes only whole ones**, leaving up to three behind.  EmuTOS's
`cleanup_tt_dma()` reads the low two bits of the final pointer to find out how
many, and copies them out of the residue register itself.  A controller that
had written them to memory already and left the residue empty would give it a
short read that it has no way to detect.

### The driver is polled, all the way down

`doc/drivers/EmuTOS/scsi.c` takes no interrupts.  It spins on the chip's status
registers for selection and for REQ, and for the data phases it arms the DMA
chip and then spins on the MFP's GPIP 7, which is where the TT wires the 5380's
interrupt output:

```c
while(!(TT_MFP_BASE->gpip & 0x80))  /* until we get IRQ */
```

That is a genuinely different way of using the part from either of the other
two - Linux carries every byte itself, SunOS takes an interrupt per phase - and
it exercises one thing neither of them does: **End of Process**.  The interrupt
EmuTOS is waiting for there is END OF DMA, set because the controller asserted
EOP across the last acknowledge cycle (p. 16).

It also makes the co-simulation simpler than the Sun-3's in a way worth
stating, because the Sun-3 section spends a long time on the opposite problem.
**No timer is needed to keep the core moving.**  A little core time bought with
each register access carries the polling loops forward, and the data phases are
run to completion inside the register write that starts them - so the interrupt
is already there when the first poll looks for it.  There is no window in which
the guest's clock advances while the core's does not, which is exactly the
window the Sun-3's `sd0: I/O request timeout` lives in.

### What it says when it works

```
                    EmuTOS Version:     1.4
                    Machine:            Atari TT
                    GEMDOS drives:      ABC

the 5380 is a replica and the disk is a memory card
and it wrote that back to C:\WROTE.TXT
```

`ABC` is the point: A and B are the floppy drives EmuTOS always offers, and C
is the partition it found by reading the AHDI table and the FAT boot sector off
the card.  The two lines after it are a program that was **not** in the ROM -
`make-tt-disk.py` puts it in `C:\AUTO`, and EmuTOS found it in a directory,
read it out of a data cluster, loaded it and jumped into it.  A filesystem
read, a program load and an execution, all of which had to cross the replica to
happen at all.

The second line is the same journey backwards.  It matters on its own account:
the receive and send directions of a DMA controller are not one piece of logic,
and a lineage that only ever read would not know which of the two it had
proved.  After the run the file is there in the card image and a FAT parser on
the host can read it, which is the check `run-tt.py` cannot make from inside.

### Two things that went wrong, both worth keeping

**The pump must not end on the chip's interrupt.**  This is the same lesson the
Sun-3 taught and it had to be applied here before it could be got wrong again:
an interrupt is a *latch* and may be left over from the command before, so a
transfer that tested it would end immediately, at zero bytes moved.  What says
the target has moved on is PHASE MATCH, and the end condition is that a request
was seen, the phase no longer matches, and only then that the chip has
interrupted.

**Nothing the guest wrote reached the card image at first**, and the run said
nothing about it, because Hatari's `--run-vbls` calls `exit(0)` where it stands
and never reaches `Main_UnInit`.  The transfers had all happened - the trace
showed 2570 acknowledge cycles in the send direction - and the flush that would
have made them permanent simply never ran.  The fix is an `atexit` handler, and
the reason it is recorded here is the failure mode: a card that silently
discarded every write looks exactly like a co-simulation that had proved reads
and writes, because the guest is perfectly happy either way.  Only a check made
from outside the guest can tell the two apart.

### What it does not do

There is no second drive on the bus - `rtl_top.sv` builds `TARGETS=1` - so the
ID decode that `two_` covers in the regression is not exercised here.  There is
no reselection, because EmuTOS never disconnects.  And EmuTOS is a BIOS rather
than an operating system: it reads and writes a filesystem but it does not
schedule, so nothing here puts the kind of sustained concurrent load on the
chip that NetBSD's `fsck` does on the Sun-3.

## Rules this directory follows

* No QEMU or Linux source, binaries or images in git.  They live in `work/`.
* The Sun-3 fork is read-only to us.  Its tree is cloned into `work/`, our
  commits go on a branch there, and what is kept is `git format-patch` output.
* Hatari is treated the same way, and for the same reason plus one more: it is
  GPL-2+, so nothing of it may reach `src/` or `tb/`.  Its tree is cloned into
  `work/hatari-src` and the one commit is kept as a patch.
* All QEMU changes are one series in `cosim/patches/qemu/`, against that one
  clone, and they build both machines.  There is no second QEMU and no release
  tarball: two trees meant two copies of anything both boards needed.
* The guest is never patched.  If something only works with a modified guest,
  it does not work.
* The scripts never install system packages; `check-deps.sh` reports and stops.

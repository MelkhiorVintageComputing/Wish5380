# Co-simulation

The regression in `tb/` checks the design against a model of what the drivers
do.  This checks it against drivers themselves, on two machines that use the
chip in the two ways it can be used:

* **An ISA card in an i386 Linux guest** - programmed I/O, every byte carried
  across the register port by the CPU.  Linux's own `g_NCR5380` finds it and
  mounts a filesystem off the SD card model.
* **The onboard SCSI of a Sun-3/60** - real bus-master DMA, where an Am9516
  answers the chip's DRQ and moves the bytes into memory itself.  Three
  guests boot on it: NetBSD/sun3, which also builds a filesystem and writes
  to it; SunOS 4.1.1, driven by the driver Sun wrote for this board; and the
  3/60 boot PROM itself, which reads the label before either of them exists.

The third one earned its keep.  **Every fault the co-simulation has found in
the Sun-3 board model came from SunOS and not from NetBSD**, because the two
drivers use the hardware differently and NetBSD forgives what SunOS does not:

| bit | what it took | what NetBSD does |
|---|---|---|
| `DMA_CONFLICT` | never set: arming the UDC before programming the chip is the normal order, and flagging it would condemn every driver | reports it and carries on |
| `DMA_IP` | not raised at terminal count: `siintr` tests it first and calls it "dma ip, unknown reason" | calls the main handler and finds the chip has interrupted too |
| `DMA_ACTIVE` | cleared when the chip stops asking, not only at terminal count | never asks for more than it will get, so its transfers always reach terminal count |

It is a separate and much slower loop and is deliberately not part of
`make test`.  Nothing in `src/` or `tb/` may grow a dependency on it - the
traffic goes the other way.

## Running it

```sh
cosim/scripts/check-deps.sh      # reports what is missing; installs nothing
make -C cosim/rtl                # build work/lib/libwish5380rtl.so
make -C cosim/rtl check          # drive it with no emulator in the loop
cosim/scripts/fetch-qemu.sh      # download, verify, unpack, apply patches/
cosim/scripts/build-qemu.sh      # i386-softmmu only, into work/qemu-install
cosim/scripts/build-guest.sh     # an i386 Linux, and a card image for it
cosim/scripts/run-cosim.py       # the verdict
```

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

**Hatari would have worked and cannot be built here.**  It models the 5380 at
the register level, and EmuTOS is free, so an Atari TT guest is a real
possibility - but building Hatari needs SDL2 headers, and these scripts do not
install packages.  It is the first thing to try if a second guest is ever
wanted.

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
separate ownership, which nothing here modifies; `cosim/patches/sun3/` holds
what has to be added to it and `cosim/patches/README.md` explains the shape.

```sh
make -C cosim/rtl                  # the shared library, as before
cosim/scripts/build-sun3-qemu.sh   # clone the fork, apply our patches, build
dd if=/dev/zero of=work/sun3/disk.img bs=1M count=16
cosim/scripts/run-sun3.py 'b sd()' -- -trace 'sun3_si_*'

# a NetBSD disk built from what NetBSD publishes, and a boot off it
cosim/scripts/make-sun3-disk.py --miniroot work/netbsd/miniroot.fs \
    --bootxx work/netbsd/usr/mdec/bootxx --out work/sun3/disk.img \
    --size-mb 320 --swap-mb 32
cosim/scripts/run-sun3.py --image work/sun3/disk.img 'b sd() netbsd.sun3'

# SunOS, whose disk is made under TME first - see below
cosim/scripts/run-tme.py SUN3-emulex -s '>:=b st()' ...
cosim/scripts/run-sun3.py --image work/sun3/sunos.img 'b sd()'
```

A run that has to survive a long silence - `fsck` says nothing for minutes -
wants `-i` raised; the default gives up after three.  And if something else
on the machine sweeps up emulators with `pkill -f qemu-system-m68k`, point
`--qemu` at a copy under another name.

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
with it, leaving the byte count as the residual.  The fix is four lines in
`sun3_si_pump`, and the traces afterwards read:

| residual | transfers |
|---|---|
| 0 bytes | 534 |
| 14 bytes | 3 |
| 20 bytes | 1 |
| stalls | 0 |

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

Three faults are on the record and none is understood.  All are intermittent,
all smell of pacing rather than of the chip, and they are written down here
rather than left in a commit message because an unexplained fault that nobody
can find again is worth less than one that is.

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

## Rules this directory follows

* No QEMU or Linux source, binaries or images in git.  They live in `work/`.
* The Sun-3 fork is read-only to us.  Its tree is cloned into `work/`, our
  commits go on a branch there, and what is kept is `git format-patch` output.
* QEMU changes exist only as patches against the released tarball, regenerated
  with `diff -ruN` against the pristine copy `fetch-qemu.sh` keeps beside it.
* The guest is never patched.  If something only works with a modified guest,
  it does not work.
* The scripts never install system packages; `check-deps.sh` reports and stops.

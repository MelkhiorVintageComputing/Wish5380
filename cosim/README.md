# Co-simulation

The regression in `tb/` checks the design against a model of what the drivers
do.  This checks it against drivers themselves, on two machines that use the
chip in the two ways it can be used:

* **An ISA card in an i386 Linux guest** - programmed I/O, every byte carried
  across the register port by the CPU.  Linux's own `g_NCR5380` finds it and
  mounts a filesystem off the SD card model.
* **The onboard SCSI of a Sun-3/60** - real bus-master DMA, where an Am9516
  answers the chip's DRQ and moves the bytes into memory itself.  The 3/60
  boot PROM selects a target and reads a block through it.

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
sequences out in the open rather than leaving them inside a kernel.

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
```

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

It is slow.  The core runs about sixteen times slower than the part it models,
so loading the kernel takes twenty minutes of wall clock where a real 3/60
took seconds.  Nothing is waiting on anything; it is simply that many bytes.

### Four things this cost, worth knowing before the next one

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

### Where it stops

## Rules this directory follows

* No QEMU or Linux source, binaries or images in git.  They live in `work/`.
* The Sun-3 fork is read-only to us.  Its tree is cloned into `work/`, our
  commits go on a branch there, and what is kept is `git format-patch` output.
* QEMU changes exist only as patches against the released tarball, regenerated
  with `diff -ruN` against the pristine copy `fetch-qemu.sh` keeps beside it.
* The guest is never patched.  If something only works with a modified guest,
  it does not work.
* The scripts never install system packages; `check-deps.sh` reports and stops.

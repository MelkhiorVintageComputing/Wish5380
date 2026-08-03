# Co-simulation

The regression in `tb/` checks the design against a model of what the drivers
do.  This checks it against a driver itself: QEMU gets an ISA card carrying the
Verilated `wish5380_sd`, an unmodified i386 Linux probes it with its own
`g_NCR5380`, and the guest mounts a filesystem off an SD card model and reads
and writes files on it.

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

## Rules this directory follows

* No QEMU or Linux source, binaries or images in git.  They live in `work/`.
* QEMU changes exist only as patches against the released tarball, regenerated
  with `diff -ruN` against the pristine copy `fetch-qemu.sh` keeps beside it.
* The guest is never patched.  If something only works with a modified guest,
  it does not work.
* The scripts never install system packages; `check-deps.sh` reports and stops.

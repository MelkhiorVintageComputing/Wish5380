# Co-simulation

The regression in `tb/` checks the design against a model of what the drivers
do.  This checks it against a driver itself.

It is a separate and much slower loop and is deliberately not part of
`make test`.  Nothing in `src/` or `tb/` may grow a dependency on it - the
traffic goes the other way.

## What works now

```sh
make -C cosim/rtl            # build work/lib/libwish5380rtl.so
make -C cosim/rtl check      # drive it with no emulator in the loop
```

`cosim/rtl/` builds `wish5380_sd` into a shared library with a small C ABI, and
`selftest.cpp` drives that library through the sequences `NCR5380_select` and
`NCR5380_transfer_pio` perform, transcribed by hand from
`doc/drivers/Linux/NCR5380.c`.  It arbitrates, selects, sends IDENTIFY, and
issues TEST UNIT READY, INQUIRY, READ CAPACITY, WRITE(6) and READ(6):

```
wish5380 self test, ABI 1
resetting, and waiting for the card...
  card up after 6200 us of simulated time
TEST UNIT READY...
  GOOD
INQUIRY...
  type 00  'DOLBEAU '  'WISH5380 SD CARD'  '0001'
READ CAPACITY...
  last block 4095, 512 bytes each
WRITE(6) then READ(6) of block 3...
  512 bytes, unchanged
```

This is the thing to run first when a guest misbehaves.  Between the driver,
the emulated card, the shared library and the RTL there are four places a fault
can be; the self test has two, and it writes the driver's sequences out in the
open rather than leaving them inside a kernel.

Setting `WISH_RTL_TRACE=1` makes the library log every register access, what
the core answered, and how much simulated time had passed.

## Two things about the library

**There are no guest-memory callbacks, and their absence is the point.**  The
sibling project needed them because a LANCE masters the bus and fetches its own
descriptors.  The NCR 5380 has no address counter and no byte counter and never
masters anything, so every byte crosses the register port under the driver's
own control.  A whole class of co-simulation difficulty - where the guest's RAM
is, whether it is contiguous, what an IOMMU does to it - simply does not arise.

**What arises instead is time.**  A LANCE is ready the moment it is reset; an
SD card takes milliseconds to come up, and until it has, the disk answers NOT
READY.  So `wish_rtl_reset` does not return until the card is up, which is what
a machine that finishes its power-on self test before probing SCSI would see.

`rtl_top.sv` builds the design with `REG_STRIDE` of one - eight registers one
byte apart, which is what Linux's `g_NCR5380` drives with `board=0`.  That is
the one configuration `tb/` does not cover, since everything there is built for
the Mac's stride of sixteen.

## What is not done yet

An emulator, and a guest whose own driver drives the library.  The intended
shape follows the sibling project: an ISA card added to QEMU that `dlopen`s
this library, and an i386 Linux built with `CONFIG_SCSI_GENERIC_NCR5380` - the
`NCR5380.c` and `g_NCR5380.c` in `doc/drivers/Linux/`, unmodified - probing it
over the ISA bus.

The guest needs no userspace for this to be worth running.  A kernel with the
driver built in prints the INQUIRY strings and the capacity as it probes, and
can mount a root filesystem off the card, which is a filesystem read through
the RTL.

## Rules this directory follows

* No QEMU or Linux source, binaries or images in git.  They live in `work/`.
* Emulator changes exist only as patches against a released tarball.
* The guest is never patched.  If something only works with a modified guest,
  it does not work.
* The scripts never install system packages; `check-deps.sh` reports and stops.

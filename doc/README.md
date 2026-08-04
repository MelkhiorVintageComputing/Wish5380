# Documentation

Two kinds of thing live here: what this project wrote down, and what it reads
from.  The second is the authority and the first defers to it.

## Which one answers which question

| if the question is | read |
|---|---|
| what does bit *n* of register *m* do | [`NCR5380_design_manual_Mar86.pdf`](NCR5380_design_manual_Mar86.pdf), and nothing else |
| in what order does a driver do things, and how long does it wait | [`drivers/`](drivers/README.md) |
| where does a register sit in a machine's address space | [`interface.md`](interface.md) |
| what does the disk answer to | [`target.md`](target.md) |
| how does the card come up | [`sd.md`](sd.md) |
| does a real driver actually work against this | [`../cosim/README.md`](../cosim/README.md) |
| why is the RTL like that | the comment beside it, which cites the page |

## What this project wrote

| file | what it is |
|---|---|
| [`interface.md`](interface.md) | **the contract between `src/` and `tb/`.**  The register port, the Wishbone slave and its three windows, the internal SCSI bus, and the delays a clockless part leaves a clocked replica to count.  Anything that changes one of those changes the RTL, the testbench and that document together |
| [`target.md`](target.md) | the SCSI target behind the fabric: the command set it answers, three rules that are easy to get subtly wrong, and what it deliberately does not do |
| [`sd.md`](sd.md) | the SD card behind the block interface: SPI mode, the order a card has to be brought up in, the two capacity layouts, and which CRC is checked and which is not |
| [`../cosim/README.md`](../cosim/README.md) | co-simulation: an unmodified Linux booting off the RTL, and why the guest is not a Macintosh when the whole design was built for one |
| [`drivers/README.md`](drivers/README.md) | which of the sources below is the authority on what, and the places two of them disagree |

## What it reads from

| file | what it is |
|---|---|
| [`NCR5380_design_manual_Mar86.pdf`](NCR5380_design_manual_Mar86.pdf) | **the specification.**  NCR's SP-1051, March 1986.  The authority on every bit of every register |
| [`NCR5380_design_manual_May85.pdf`](NCR5380_design_manual_May85.pdf) | the same manual ten months earlier, before the 53C80 appendix.  Kept as a check on the scan |
| [`NCR53C400.pdf`](NCR53C400.pdf) | the 53C400, a 5380 with a block-move engine and a 128-byte buffer wrapped around it.  Not implemented; here because Linux's `g_NCR5380.c` supports one and the register names leak into `NCR5380.h` |
| [`NCR_SCSI_Engineering_Notebook_Rev2_Oct85.pdf`](NCR_SCSI_Engineering_Notebook_Rev2_Oct85.pdf) | NCR's own application notes on SCSI bus behaviour, useful for the phases and the timings the chip leaves to software |
| [`drivers/`](drivers/README.md) | unmodified drivers for the real chip, from three independent families - Linux, NetBSD, and Sun's own for the machine the co-simulation boots.  They keep their own licences; nothing in `src/` or `tb/` is derived from them |

Both design manuals are scans from [bitsavers](https://www.bitsavers.org/components/ncr_symbios/scsi/5380/)
and are legible throughout; the register sections in particular are clean.
Where a rule in the RTL comes from the datasheet, the comment cites the
printed page number, which is two less than the PDF page number.

The March 1986 edition is the one to read.  It is the May 1985 text plus
appendix A5, which describes the CMOS NCR 53C80 and the one software-visible
thing it adds - LAST BYTE SENT in bit 7 of the Target Command Register.

The datasheet settles bit positions and register behaviour; the drivers settle
sequencing, because what order a driver writes registers in and how long it
waits are things the datasheet does not say.  Where the two could disagree,
`drivers/README.md` records which won and why - including two places where a
driver's *comment* disagrees with the datasheet about a bus delay, which are
noted so nobody "fixes" the RTL to match a comment.

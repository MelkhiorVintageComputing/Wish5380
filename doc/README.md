# Documentation

| file | what it is |
|------|------------|
| [`NCR5380_design_manual_Mar86.pdf`](NCR5380_design_manual_Mar86.pdf) | **the specification.**  NCR's SP-1051, March 1986.  The authority on every bit of every register |
| [`NCR5380_design_manual_May85.pdf`](NCR5380_design_manual_May85.pdf) | the same manual ten months earlier, before the 53C80 appendix.  Kept as a check on the scan |
| [`NCR53C400.pdf`](NCR53C400.pdf) | the 53C400, a 5380 with a block-move engine and a 128-byte buffer wrapped around it.  Not implemented; here because Linux's `g_NCR5380.c` supports one and the register names leak into `NCR5380.h` |
| [`NCR_SCSI_Engineering_Notebook_Rev2_Oct85.pdf`](NCR_SCSI_Engineering_Notebook_Rev2_Oct85.pdf) | NCR's own application notes on SCSI bus behaviour, useful for the phases and the timings the chip leaves to software |
| [`interface.md`](interface.md) | the contract between `src/` and `tb/`: register port, Wishbone apertures, the internal SCSI bus, clocking |
| [`drivers/`](drivers/README.md) | vintage drivers for the real chip, and which source is the authority on what |

Both design manuals are scans from [bitsavers](https://www.bitsavers.org/components/ncr_symbios/scsi/5380/)
and are legible throughout; the register sections in particular are clean.
Where a rule in the RTL comes from the datasheet, the comment cites the
printed page number, which is two less than the PDF page number.

The March 1986 edition is the one to read.  It is the May 1985 text plus
appendix A5, which describes the CMOS NCR 53C80 and the one software-visible
thing it adds - LAST BYTE SENT in bit 7 of the Target Command Register.

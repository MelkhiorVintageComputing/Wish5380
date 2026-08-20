# Drivers for the card

Unmodified copies of three SPI-mode SD host drivers, kept as reference
material.  They keep their original licences, which are in the file headers;
nothing in `src/` or `tb/` is derived from them.  They are here to be read.

This is the same arrangement `doc/drivers/README.md` makes for the NCR 5380
and for the same reason: where three independently written hosts agree, the
sequence belongs to the card rather than to one author's habit.  Where they
*disagree*, that is worth more than agreement, and the first thing this
directory produced was a disagreement - see *The CRC7 question* below.

| directory  | what it is |
|------------|------------|
| `Linux/`   | `drivers/mmc/host/mmc_spi.c`, the kernel's SPI-mode host.  The largest of the three and the only one that carries a full MMC-layer request model on top |
| `u-boot/`  | `drivers/mmc/mmc_spi.c`, U-Boot's.  Written to boot a machine and then get out of the way, so the sequence is visible without a subsystem around it |
| `FatFs/`   | ChaN's `sdmm.c`, "Foolproof MMCv3/SDv1/SDv2 (in SPI mode) control module".  Bit-banged, four GPIO pins, no peripheral at all.  The easiest of the three to read a sequence out of, and the ancestor of a great many embedded SPI-mode drivers |

A fourth reading exists and is not a driver: `hw/sd/ssi-sd.c` in QEMU, whose
header records that it was validated against U-Boot v2021.01 and Linux v5.10's
`mmc_spi`.  It is the card side rather than the host side, which makes it the
useful check on what a host may assume.

## Which source is the authority on what

The **SD Physical Layer Simplified Specification** wins any disagreement about
what a card must do; `doc/sd.md` says which version and where to get it.  The
drivers are the authority on what a host actually does, which is a different
question and the one that matters when deciding what our controller must
tolerate.

| what | where |
|---|---|
| the power-up clocks, deselected, line high | `FatFs/sdmm.c:406` - ten bytes of ones, "80 dummy clocks", against the specification's 74 |
| the initialisation order, end to end and in one screen | `FatFs/sdmm.c:409-432` |
| CMD8 as the version test, and the echo-back that confirms it | `FatFs/sdmm.c:410-412` - argument `0x1AA`, check `buf[2]==0x01 && buf[3]==0xAA` |
| ACMD41 as CMD55 then the command | `FatFs/sdmm.c:327-329` |
| the Host Capacity Support bit in ACMD41 | `FatFs/sdmm.c:414` - `1UL << 30` |
| Card Capacity Status, and what it decides | `FatFs/sdmm.c:417-419` - `buf[0] & 0x40` sets `CT_BLOCK`, which is what makes the address a block number |
| CMD16 only where it is needed | `FatFs/sdmm.c:432` - in the SDv1/MMC branch only.  FatFs never sends it to an SDv2 card of either capacity |
| N(CR), the idle gap before a response | `Linux/mmc_spi.c:428` - "N(CR) (== 1..8) bytes of all-ones".  A window, not a number, which is why a differential harness must not compare it byte for byte |
| the data start token | `u-boot/mmc_spi.c:44-48` - `0xFE` single, `0xFC` multi-write, `0xFD` stop |
| waiting for the card to stop being busy after a write | `FatFs/sdmm.c:204,247,297`; `Linux/mmc_spi.c:182,305-315` |
| checking the CRC16 on read data, which is optional and mostly skipped | `u-boot/mmc_spi.c:208-213`, behind `CONFIG_MMC_SPI_CRC_ON` |

## The CRC7 question, and what these three settled

`doc/sd.md` used to say that CRC7 is computed for exactly two commands and
that "every SPI-mode driver in existence carries the same two constants for
the same reason".  That is wrong, and these three files are how it was found
out.  One of them does it that way and two do not:

| driver | what it puts in the CRC byte |
|---|---|
| `FatFs/sdmm.c:345-347` | `0x01`, except `0x95` for CMD0 and `0x87` for CMD8 |
| `Linux/mmc_spi.c:424` | `crc7_be(0, cp + 1, 5) \| 0x01` - always |
| `u-boot/mmc_spi.c:109` | `(crc7(0, &cmdo[1], 5) << 1) \| 0x01` - always |

Linux says why, at `mmc_spi.c:415`:

> `crc7 (plus end bit) ... always computed, it's cheap`

Both readings are legal, because a card in SPI mode does not check the CRC
until CMD59 turns checking on - which is why the two-constant version works at
all.  But "legal" and "what hosts do" are different claims, and only the first
was ever true of the two-constant version.  A card that had CRC checking
enabled, or a model written to the stricter reading, would reject every
command after CMD8 from a host that sent `0x01`.

That is not hypothetical: it is exactly what happens to `blk_sd` against
ZipCPU's card model, which asserts on any bad CRC7 and has no CMD59 to turn
checking off.  `doc/sd.md` records what was done about it.

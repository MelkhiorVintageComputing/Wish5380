# The SD card

`src/blk_sd.sv`, over `src/sd_spi.sv`, is what sits behind the block interface
the SCSI target reaches storage through.  The target asks for one 512-byte
block at a time and never sees a card.

The whole card is one drive, and one drive is a whole card.  That is the
simplest thing that works and not the only sensible thing; `doc/storage.md`
records what the devices that solved this before us do instead, and why
several drives on one card means several SCSI IDs rather than several logical
units.

SPI mode, not the native four-bit bus.  It is slower - one bit a clock rather
than four - and 25 MHz of it is still comfortably above the 1.5 MB/s the NCR
5380 can manage asynchronously, so the SCSI side is the bottleneck either way.
What it buys is that every card supports it and the protocol is small enough to
be read in one sitting.

## Coming up

The sequence is the one every SPI-mode driver performs, and the order is not
negotiable: a card sent these out of order does not come up.

| step | why |
|------|-----|
| ≥ 74 clocks, deselected, line high | the card finishes its internal power-up |
| CMD0 | into SPI mode |
| CMD8 | does it know the 2.00 spec?  A card that answers "illegal command" is a version 1 card |
| CMD55 + ACMD41, repeatedly | until it stops reporting idle.  This is where the time goes: most of a second, on a real card |
| CMD58 | the OCR, whose Card Capacity Status bit says how blocks are addressed |
| CMD16 | fix the block length at 512.  A high-capacity card has no other length and ignores it |
| CMD9 | the CSD, which is where the capacity comes from |

All of it at no more than 400 kHz, because that is all a card will accept until
it is up.  Afterwards the divider changes and the same logic runs at 25 MHz.
That change of gear is worth a test of its own: a controller that never made it
would move data correctly and be sixty times slower, and no test that only
checked the bytes would notice.

## Two things that are easy to get subtly wrong

Both are pinned by a test, and both were checked by mutating the RTL until the
test failed.

* **A version 1 card is addressed by byte offset and a version 2 card by block
  number.**  Below four gibibytes either kind is legal and real ones of both
  exist.  Getting it backwards reads sector zero for everything on a small
  card, and reads far off the end on a large one.
* **The two CSD layouts state the size in quite different ways** - a version 1
  card gives a size, a multiplier and a block length, a version 2 card gives a
  count of half-megabytes.  Reading one as the other gives a disk of the right
  shape and the wrong size, which shows up only when a filesystem runs off the
  end of it.

## CRC

CRC7 is computed for exactly two commands, and the constants are written down
rather than calculated: `0x95` for CMD0 and `0x87` for CMD8.  Those are the two
that arrive before the card stops checking, and every SPI-mode driver in
existence carries the same two constants for the same reason.  Everything after
them carries a stop bit and nothing else.

**CRC16 on read data is checked**, and that is a deliberate departure from what
most SPI-mode drivers bother with.  Nothing obliges a host to look at it, but
the card computes it over every block it sends, and it is the only thing
standing between a marginal card and a silently corrupt sector arriving on the
SCSI bus as good data.  A wrong CRC becomes a MEDIUM ERROR with UNRECOVERED
READ ERROR, which is what a real drive would have reported.

## What it does not do

* **No multi-block transfers.**  CMD18 and CMD25 would move several blocks per
  command and are a straightforward addition; the target asks for one block at
  a time, so the gain would be in the card protocol rather than on the SCSI
  bus.
* **No card detect or write protect.**  Both are switches in the socket rather
  than anything the card says, and belong to the board.
* **No SDIO, no MMC, no SDUC.**  The initialisation would need more branches
  and nothing here would use them.

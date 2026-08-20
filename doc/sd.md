# The SD card

`src/blk_sd.sv`, over `src/sd_spi.sv`, is what sits behind the block interface
the SCSI target reaches storage through.  The target asks for one 512-byte
block at a time and never sees a card.  `doc/block.md` is the interface itself,
and is the thing to read before writing a second back end; this document is
what is on the far side of it.

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

## Where answers come from

The **SD Physical Layer Simplified Specification** is the authority on what a
card must do.  The SD Association publishes it free of charge at
<https://www.sdcard.org/downloads/pls/>, behind a terms page that has to be
accepted by hand - which is why it is not fetched by any script here.  Cite it
by version and section the way the SCSI side cites datasheet pages.

**`doc/drivers/SD/` is the authority on what a host actually does**, which is
the different and often more useful question.  Three independent SPI-mode
hosts are kept there - Linux's `mmc_spi.c`, U-Boot's, and ChaN's FatFs
`sdmm.c` - on the same principle as the four 5380 drivers: where they agree
the sequence belongs to the card, and where they disagree there is something
to settle.  `doc/drivers/SD/README.md` indexes the passages that answer
specific questions, and records the one disagreement that has already changed
this design - see *CRC* below.

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
| CMD16 | fix the block length at 512, and only for a card that is addressed in bytes.  A high-capacity card has no other length, so it is skipped entirely - as FatFs skips it for every SDv2 card (`doc/drivers/SD/FatFs/sdmm.c:432`) |
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

## How a command is framed

Three things about the frame are not obvious from the command table, and all
three were wrong here until `cosim/sdcheck` drove this back end against a card
model written by somebody else.  Each is now what all three drivers in
`doc/drivers/SD/` do.

* **Seventy-four clocks means seventy-four, with the card deselected.**  The
  power-up burst is ten bytes and the card is selected on a state of its own
  *after* the tenth, because asserting /CS alongside the last transfer sends
  that byte selected and leaves seventy-two - two short of what a card is
  entitled to wait for.  Nothing counted them until the harness did; the RTL
  comment said eighty and meant it, and the code did not.
* **A byte of ones goes in front of every command.**  The specification calls
  the gap N(RC) and gives it as eight clocks between a response and the next
  command.  U-Boot writes it into the frame (`cmdo[0] = 0xff`,
  `u-boot/mmc_spi.c:103`), Linux does the same and says why - "an all-ones
  byte to ensure the card is ready" (`Linux/mmc_spi.c:412`) - and FatFs gets it
  from clocking a dummy byte in `select()`.  This used to send the command
  byte immediately after the response byte.
* **/CS is released between commands.**  That gap byte goes out deselected, so
  every command is framed the way U-Boot frames one with `SPI_XFER_BEGIN` and
  `SPI_XFER_END`, Linux with a message that leaves chipselect active and then
  ends, and FatFs with an explicit `deselect(); select();`.  Holding /CS low
  for a whole session, which is what this did, is legal and unusual - and a
  card that resynchronises when it is deselected never gets the chance.

None of the three stopped the design working against our own card model,
because our own card model was written from the same reading and did not care
about any of them.  That is the whole argument for having a second opinion.

## CRC

**CRC7 is computed for every command**, and the two famous constants - `0x95`
for CMD0, `0x87` for CMD8 - fall out of the arithmetic rather than being
written down.

This used to be the other way round, and the reason for the change is worth
recording because the old text stated a fact that was not one.  It said the
constants were enough and that "every SPI-mode driver in existence carries the
same two constants for the same reason".  Setting three independent SPI-mode
hosts side by side - which is what `doc/drivers/SD/` is for - shows one of them
does that and two do not:

| driver | the CRC byte |
|---|---|
| `FatFs/sdmm.c:345-347` | `0x01`, except `0x95` for CMD0 and `0x87` for CMD8 |
| `Linux/mmc_spi.c:424` | `crc7_be(0, cp + 1, 5) \| 0x01` - always |
| `u-boot/mmc_spi.c:109` | `(crc7(0, &cmdo[1], 5) << 1) \| 0x01` - always |

Linux says why at `mmc_spi.c:415`: *"crc7 (plus end bit) ... always computed,
it's cheap"*.

Both are legal.  A card in SPI mode does not check the CRC until CMD59 turns
checking on, and CMD0 and CMD8 are checked whatever the setting - which is why
the two-constant version works, and why those two constants are famous.  What
is not legal is the inference the old text drew from it: that a host need
never compute one.  A card with checking enabled rejects every command from
such a host, and so does any card model written to the stricter reading.
ZipCPU's is: it asserts on a bad CRC7 and has no CMD59 to turn checking off.

Computing it costs a seven-bit shift register.  Not computing it costs the
ability to talk to a card in a state the specification allows.  So `blk_sd`
computes it, `tb/cpp/sd_card.cpp` checks it on every command - deliberately
stricter than a card in its default state has to be - and
`layout_the_two_known_command_crcs` pins the arithmetic to the two constants
the drivers write down, so that the RTL and the model cannot agree on the same
mistake.

**CRC16 on read data is checked**, and that is a deliberate departure from what
most SPI-mode drivers bother with.  Nothing obliges a host to look at it -
U-Boot's is behind `CONFIG_MMC_SPI_CRC_ON` (`u-boot/mmc_spi.c:208`) and off by
default - but the card computes it over every block it sends, and it is the
only thing standing between a marginal card and a silently corrupt sector
arriving on the SCSI bus as good data.  A wrong CRC becomes a MEDIUM ERROR
with UNRECOVERED READ ERROR, which is what a real drive would have reported.

## What it does not do

* **No multi-block transfers.**  CMD18 and CMD25 would move several blocks per
  command and are a straightforward addition; the target asks for one block at
  a time, so the gain would be in the card protocol rather than on the SCSI
  bus.
* **No card detect or write protect.**  Both are switches in the socket rather
  than anything the card says, and belong to the board.
* **No SDIO, no MMC, no SDUC.**  The initialisation would need more branches
  and nothing here would use them.

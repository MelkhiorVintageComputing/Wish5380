# One card per drive, and whether it should stay that way

The design gives every SCSI target its own SD card: `wish5380_sd` instantiates
`blk_sd` once per drive, and each `blk_sd` hands the whole card over as one
disk, LBA 0 at card block 0.  That is stated in `doc/interface.md` and
`doc/sd.md` as a fact about the design and nowhere as an argument, which is a
gap, because a microSD card bought today is orders of magnitude larger than
the disk a Mac Plus expected, and the arrangement wastes almost all of it.

This document is the argument.  It records what the devices that solved the
same problem before us actually do, what of it is worth adopting, and what
this project would have to change to adopt it.  **Nothing here is implemented.**

## The field

Five projects do essentially what this one does, in software on a
microcontroller or on a Raspberry Pi rather than in logic.  All are real
products people use with real vintage machines, which makes their conventions
worth more than any scheme argued from first principles here.

| project | storage | configuration | LUNs | tape |
|---|---|---|---|---|
| [SCSI2SD](https://github.com/rabbitholecomputing/SCSI2SD) v5 | raw sectors | binary blob in the board's own flash, written over USB | LUN 0 only | placeholder only |
| SCSI2SD v6 | raw sectors | 1 KiB binary blob in the **last two sectors of the card** | LUN 0 only | placeholder only |
| [BlueSCSI](https://github.com/BlueSCSI/BlueSCSI-v2) v2 | FAT32/exFAT files | filename, plus `bluescsi.ini` | LUN 0 only | flat image, partly stubbed |
| [ZuluSCSI](https://github.com/ZuluSCSI/ZuluSCSI-firmware) | FAT32/exFAT files, or a raw range | filename, plus `zuluscsi.ini` | LUN 0 only | SIMH `.tap`, new and incomplete |
| [MacSD](https://macsd.com/) | FAT32 files, or an MBR partition | `macsd.ini` | none | none |
| [PiSCSI](https://github.com/PiSCSI/piscsi) / [SCSI2Pi](https://github.com/uweseimet/scsi2pi) | files on Linux | command line | **0-31** | PiSCSI stubbed; SCSI2Pi real |

They are not independent.  BlueSCSI v2 and ZuluSCSI both carry a copy of
SCSI2SD's firmware under `lib/SCSI2SD/`, and ZuluSCSI's README says outright
that it also accepts BlueSCSI's filenames.  SCSI2Pi is a fork of PiSCSI by
PiSCSI's own former lead on the emulation code.  So the table has three
lineages in it and not six, which is worth remembering before counting any
agreement as corroboration.

## Several drives means several IDs, not several logical units

This is the clearest result of the survey and it settles the question the
document was written for.

Every one of the microcontroller devices supports **only LUN 0**.  SCSI2SD's
target configuration has no LUN field at all.  BlueSCSI and ZuluSCSI both
carry

```c
#define NUM_SCSILUN 1          // Maximum number of LUNs supported (Currently has to be 1)
```

and both reject a non-zero LUN on the wire with `ILLEGAL_REQUEST` and
`LOGICAL_UNIT_NOT_SUPPORTED` - which is, bit for bit, what `src/scsi_targ.sv`
already sends.  ZuluSCSI's `scsiDiskOpenHDDImage()` even takes a `scsi_lun`
argument its body never reads.  MacSD has no LUN in its configuration syntax
at all, and the one mention of the word in its changelog is *"Fixed LUN
handling, resolving cloned drives on some platforms"* - the signature of a
target that used to answer on every unit and now answers on one.

Where a machine genuinely needs logical units, all three fake it the same way.
`MapLunsToIDs` rewrites an incoming LUN *n* into a lookup of target ID *n* and
folds the unit back to 0.  SCSI2SD's own comment names the machine it was
written for: a Philips P2000C whose Xebec S1410 SASI bridge addresses a second
drive as ID 0, LUN 1.

The exception proves the rule rather than breaking it.  PiSCSI and SCSI2Pi do
support units 0-31, addressed `-ID n:u`, and they are the two that run under
Linux on a Raspberry Pi with a filesystem and an allocator underneath them.
The split in the table is not between people who understood SCSI and people who
did not; it is between implementations with room and implementations without.

**So: several targets, not several units.**  It is what the field does, it is
what `scsi_fabric` is already built to carry, and `two_` already tests the ID
decode against a bus with more than one device on it.  `scsi_targ` needs no
change at all to be correct - it already decodes the unit from both places
SCSI-1 puts it, the IDENTIFY message and the top three bits of CDB byte 1, and
refuses everything but zero.

This is not a claim that logical units are wrong or that the chip cannot
reach them.  `doc/drivers/SunOS34/sundev/scsi.h`, which is in this tree, has
carried

```c
#define TARGET(slave)   ((slave >> 3) & 07)
#define LUN(slave)      (slave & 07)
```

since 1987.  Sun addressed target and unit separately when the part was new.
If this design ever wants units, character 3 of a BlueSCSI-style filename is
the position both codebases reserved for one and never filled.

## Where a drive's extent could come from

Three approaches exist and only one of them can be read by logic.

**A filesystem with image files** is what BlueSCSI, ZuluSCSI and MacSD use.
BlueSCSI and ZuluSCSI encode the drive in the name, positionally:

| position | meaning |
|---|---|
| characters 0-1 | device type, case-insensitive: `HD` `CD` `FD` `MO` `RE` `TP` `ZP` |
| character 2 | SCSI target ID, `0`-`7` (ZuluSCSI extends to `A`-`F` for wide) |
| character 3 | the logical unit - parsed, then discarded unless `0` |
| after the first `_` | block size, `strtoul`, anything from 8 to 65536 |
| the rest | ignored, so a descriptive name works |

So `HD10_512.hda` is a fixed disk at ID 1, unit 0, 512-byte blocks, and
`CD3 Myst.iso` is an optical drive at ID 3.  It is a good convention: readable,
editable on any desktop, and self-describing.  It also requires a FAT32
directory walk with cluster chains, which is a great deal of logic to put
behind a state machine that currently asks for a block number and gets a block.

**A raw extent per drive** is what SCSI2SD has always used.  A drive is a
start sector, a count and a sector size on the bare card, with no filesystem
anywhere in the firmware:

```c
uint32_t sdSectorStart;
uint32_t scsiSectors;
uint16_t bytesPerSector;
```

ZuluSCSI offers the same thing through a pseudo-filename, `RAW:first:last`,
reachable from its ini, from a DIP switch, or as the automatic fallback when
no images are found; MacSD offers it as `partition=` against an MBR entry.

**A table on the card itself** is SCSI2SD v6's answer to where the extents are
written down, and it is the interesting one.  The configuration is a 1 KiB
binary blob in the **last two sectors of the card** - `S2S_BoardCfg` followed
by seven `S2S_TargetCfg`, each exactly 128 bytes - found by capacity minus
two and validated by the ASCII magic `BCFG`:

```c
int cfgSectors = (S2S_CFG_SIZE + 511) / 512;
BSP_SD_ReadBlocks_DMA(&s2s_cfg[0], sdDev.capacity - cfgSectors, cfgSectors);
if (memcmp(config->magic, "BCFG", 4)) { /* invalid, use default */ }
```

That is two block reads at a known offset, a four-byte compare and a
128-byte-strided walk.  It is a partition table that logic can read, and each
entry carries rather more than an extent: `vendor[8]`, `prodId[16]`,
`revision[4]`, `serial[16]`, `sectorsPerTrack`, `headsPerCylinder` and a
`quirks` word.  `scsi_targ` has the first three already, as elaboration
parameters - `VENDOR`, `PRODUCT`, `REVISION`, overridable because *"some Apple
utilities - HD SC Setup in particular - refuse to touch a drive whose vendor is
not `APPLE   `"* - so what the table asks for is not new fields but the same
fields settable per drive at run time.
The SCSI ID is packed into the low three bits of `scsiId` with an enable bit
at `0x80`, so a slot can be present and switched off, and the array index is
not the ID.

**The recommendation is the `BCFG` table, with the present whole-card
behaviour kept as the fallback.**  The fallback is not a concession: it is
exactly what BlueSCSI and ZuluSCSI both do when they find nothing to read
(`RAW_FALLBACK_ENABLE 1`, one drive, 512-byte blocks), so keeping it makes
this design agree with all three on the simple case from the first day.

The cost is real and should be written down rather than discovered later: **a
card with no filesystem on it is opaque to a desktop machine.**  An image
cannot be dragged onto it; something has to write the extents and the data.
That is the genuine advantage the filename convention has, and it is the
reason to treat a FAT reader as a possible later layer rather than as
something ruled out.

One defect not to inherit.  SCSI2SD's `s2s_configSave()` writes the blob at
`capacity - S2S_CFG_SIZE`, a byte count, while `s2s_configInit()` reads it at
`capacity - 2`, a sector count.  The two do not agree.  A reader should read
where the reader reads.

## Tape

A sequential-access device is the one thing in the survey this design could do
that nobody has yet done, and the reasons for and against are the same
reasons.

**The format is settled.**  SIMH `.tap` is the interchange format, specified
in [Supnik's *SIMH Magtape Representation and
Handling*](https://simh.trailing-edge.com/docs/simh_magtape.pdf).  Each record
carries a four-byte little-endian length **before and after** the data, so it
can be read in either direction; data is padded to an even length and the pad
is not counted.  Three markers matter:

| marker | meaning |
|---|---|
| `0x00000000` | tape mark, which is to say a filemark |
| `0xFFFFFFFE` | erase gap |
| `0xFFFFFFFF` | end of medium |

SIMH and MAME implement it independently and agree, down to the subtlety that
records are a multiple of two bytes while markers are four, so a record
written over a gap can leave half a marker behind: a forward reader that sees
`0xFFFEFFFF` must back up two bytes, and a reverse reader must treat
`0xFFFF0000`-`0xFFFFFFFD` as the same thing.  Two independent implementations
agreeing on a corner that obscure is about as good as a specification gets.

**The command set is not the disk's.**  For a SCSI-2 sequential-access device,
`REWIND`, `READ BLOCK LIMITS`, `WRITE FILEMARKS`, `SPACE` and - note - `ERASE`
are all mandatory.  `SPACE` by blocks and by filemarks is mandatory; by
sequential filemarks and to end-of-data is optional.  The `Fixed` bit in CDB
byte 1 chooses whether the transfer length counts blocks or bytes, and a block
descriptor length of zero means variable-length records.  None of that exists
in `scsi_targ` today, and none of it resembles anything that does.

**Nobody has made it work.**  Across BlueSCSI, ZuluSCSI, PiSCSI and SCSI2Pi,
no completed installation of a vintage operating system from an emulated tape
could be found.  BlueSCSI's `WRITE FILEMARKS` carries the comment *"Filemarks
storage not implemented, reporting ok"*.  PiSCSI's is `// TODO Add proper
implementation`, and its `SPACE` by blocks is an absolute seek.  ZuluSCSI's
`.tap` support arrived in March 2026 and its issue #211 - `mt fsf`, `fsfm`,
`bsf`, `bsfm`, `bsr` all returning I/O errors - was still open in April.
SCSI2Pi's is the best of them and could read, write, label and `BACKUP` an
OpenVMS tape on a VAXstation 3100 but not boot from it.

**One design consequence is worth extracting even if no tape is ever built.**
`.tap` carries no density, no block size and no drive identity.  Drivers get
those from a table keyed on the INQUIRY strings - NetBSD's `st.c` matches
`"ARCHIVE "` and `"VIPER 150  21247"` and adjusts block size, density and half
a dozen quirks accordingly, and MAME ships a second INQUIRY identity for
exactly that reason.  So **a tape target's INQUIRY strings are load-bearing in
a way a disk's are not**, and the fixed `DOLBEAU` / `WISH5380` identity in
`scsi_targ` would have to become configurable before a tape could work at all.
That is an argument for the `BCFG` table independent of tape, since the table
already carries the strings - and `scsi_targ` already has them as parameters,
so what changes is when they are chosen, not whether they exist.

**It is not needed for anything this project currently boots.**  A Sun-3/60
with an early PROM can boot only from `sd0` and a QIC-24 tape, which is why
tape looked essential at first; but `cosim/scripts/run-sun3.py` boots PROM
3.0.1, which can boot from CD.  Two further facts narrow it: the standalone
boot driver in `doc/drivers/SunOS34/sunstand/si.c` registers only

```c
struct boottab sidriver = {"sd", ...};
```

so there is no tape in the standalone path this tree holds, and the PROM's own
`b st(0,0,3)` addresses a tape by *file number*, which is `SPACE` by
filemarks - the command every existing implementation stubs.

And one caveat against the target as it stands: `doc/target.md` records that
this target never disconnects, while `doc/drivers/NetBSD/si_obio.c` notes that
disconnect and reselect are enabled by default on targets 4-6 because *"those
are normally tapes that really need it enabled"*.  Disconnection is permitted
rather than required, so it is a caveat and not a blocker, but tape transfers
are long and it is the first place a real driver's assumptions about a tape
would part company with its assumptions about a disk.

**So tape is last**, and on its merits rather than on necessity: a device type
nobody has got working against real hardware, exercising a command set no disk
path touches.  That is a good reason to attempt it and a poor reason to
attempt it first.

## What this design would have to change

Recorded so the size of the thing is on the record, not because any of it is
agreed.

* `blk_sd` hands over the whole card.  An extent - a base block added to every
  address, and a count that `scsi_targ` reports through READ CAPACITY - is a
  shim at the seam `doc/interface.md` already describes as the place where the
  back end can be substituted.  This is the small part.
* Something has to read the `BCFG` table before any target answers, and
  distribute the extents and identity strings.  This is the part that does not
  exist in any form today, and the question it raises - a small sequencer in
  logic, or a soft core, or a build-time constant - is the real design
  decision hiding behind this document.
* `scsi_targ`'s identity is settable, but only at elaboration: `VENDOR`,
  `PRODUCT` and `REVISION` are parameters.  A table read off the card needs
  them as ports instead, which is a small change with a wide blast radius -
  every instantiation and every test that relies on the defaults.
* `cosim/rtl/rtl_top.sv` builds `TARGETS=1`, so no co-simulated guest has ever
  seen a second drive.  The `two_` tests cover the ID decode, but no real
  driver has.  That gap is worth closing whatever is decided here, and it is
  cheap.

## What is not settled

* Whether the `BCFG` layout is stable enough to depend on.  It is read from a
  firmware archive of a discontinued product; nothing obliges anyone to keep
  it.  Adopting it is a bet that a documented on-card format is better than an
  undocumented one of our own, not that anybody will maintain it.
* Whether a FAT32 reader belongs anywhere in this design, later or ever.
* ArdSCSino's storage layout could not be confirmed from primary sources; its
  README is in Japanese and documents only a `scsi-config.txt` with vendor,
  product, revision and a machine-type byte.  The frequently repeated claim
  that it uses raw sectors is not verified here.
* The SCSI-1 mandatory-command table for sequential-access devices is not
  freely available.  The mandatory and optional split above is SCSI-2's, and
  the machines this design targets are older than that.

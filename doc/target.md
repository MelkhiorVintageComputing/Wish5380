# The target

`src/scsi_targ.sv` is the disk on the other end of the fabric: the half of the
conversation the NCR 5380 does not do.

The chip is deliberately thin.  It handshakes one byte and reports what it
sees; the driver drives everything above that.  So something has to answer a
selection, sequence the phases and understand a command descriptor block, and
on a real machine that something is the firmware inside the drive.

## What it answers to

The command set is what a vintage driver actually uses to find a disk and boot
from it, and no more.

| opcode | command | what it does here |
|--------|---------|-------------------|
| `0x00` | TEST UNIT READY | GOOD, or CHECK CONDITION with NOT READY when there is no card |
| `0x01` | REZERO UNIT | GOOD |
| `0x03` | REQUEST SENSE | eighteen bytes of extended sense |
| `0x04` | FORMAT UNIT | GOOD, and nothing is formatted |
| `0x08` | READ(6) | |
| `0x0a` | WRITE(6) | |
| `0x0b` | SEEK(6) | GOOD |
| `0x12` | INQUIRY | thirty-six bytes |
| `0x15` | MODE SELECT(6) | the parameters are taken and thrown away |
| `0x16` | RESERVE | GOOD |
| `0x17` | RELEASE | GOOD |
| `0x1a` | MODE SENSE(6) | a header and one block descriptor, no pages |
| `0x1b` | START STOP UNIT | GOOD |
| `0x1d` | SEND DIAGNOSTIC | GOOD |
| `0x1e` | PREVENT/ALLOW MEDIUM REMOVAL | GOOD |
| `0x25` | READ CAPACITY | the last block and the block length |
| `0x28` | READ(10) | |
| `0x2a` | WRITE(10) | |
| `0x2b` | SEEK(10) | GOOD |
| `0x2f` | VERIFY(10) | GOOD |

Anything else is CHECK CONDITION with ILLEGAL REQUEST and INVALID COMMAND
OPERATION CODE, which is what a drive of the period did with a command it had
never heard of.

The commands that do nothing are not padding.  A driver that gets a failure
from START STOP UNIT or PREVENT/ALLOW decides the disk is broken and gives up
on it, so answering GOOD is the behaviour, not a shortcut.

## Three rules that are easy to get subtly wrong

Each is pinned by a test, and each was caught by mutating the RTL to check the
test bites.

* **READ CAPACITY reports the last addressable block, not the number of
  blocks.**  A target that reports the count makes every driver think it has
  one block more than it does.
* **A six-byte transfer length of zero means 256 blocks**, and it is the only
  place SCSI-1 counts that way.  An *allocation* length of zero, in INQUIRY or
  REQUEST SENSE, means zero.  The two cannot share a decode.
* **Sense is consumed by reading it.**  It is preserved "until retrieved by a
  REQUEST SENSE command or until the next command", so the clear happens when
  the REQUEST SENSE finishes and not at the top of the next command, where it
  would erase the very thing the driver is asking about.

## An absent logical unit is not an error

Probing logical units is the first thing a driver does.  INQUIRY to a unit
that is not there answers GOOD with peripheral qualifier 3 and device type
`0x1f` - "nothing here" - because CHECK CONDITION makes some drivers give up
on the whole target rather than on the one unit.  Every other command aimed at
such a unit is refused with LOGICAL UNIT NOT SUPPORTED.

The unit number is taken from the IDENTIFY message when there is one, and from
the top three bits of command byte 1 when there is not, which is where SCSI-1
put it for initiators that send no message at all.

## What it deliberately does not do

* **It never disconnects.**  A target is allowed not to - it keeps BSY and
  finishes the command - and every driver in `doc/drivers/` copes, because a
  target that never disconnects is the easy case for all of them.  The 5380
  side supports reselection; there is simply nothing that reselects.
* **It does not check incoming parity.**  The fabric carries the bit and the
  chip checks it in the other direction; a target that rejected a bad byte
  would need an ABORTED COMMAND path that nothing exercises yet.
* **It supports one logical unit and one initiator.**  The fabric's fourth
  port is the testbench's, for standing another device on the bus.

## Two of them

`TARGETS` on `wish5380_wb` is one or two, and two is the default.  Each drive
has its own ID, its own block back end and its own card slot; a board that
carries one leaves the second out entirely rather than wiring an empty slot to
it, because a device driving nothing is a device that is not there.

The second drive is not decoration.  A SCSI bus with one device never
exercises the ID decode against anything that could get it wrong: a target
that answered every selection and one that answered only its own both pass
every test that has one drive to talk to, because the only other outcome is
silence.  `test_two.cpp` is what tells them apart, and mutating the decode -
both drives on the same ID, or a target that answers any non-zero selection -
fails five or six of its tests.

Note that a second *target* does not exercise arbitration.  Arbitration is
between initiators, and a target arbitrates only to reselect; nothing here
disconnects, so nothing here reselects.  What two drives exercise is
selection, the ID decode, and a wired-OR with three devices on it.

## The block back end

The target reaches storage through a 512-byte sector buffer and four signals -
start, direction, block address, done - and never talks to a card itself.  The
back end fills the buffer before a READ and drains it after a WRITE, through
its own port on the same dual-ported memory; the two sides never touch it at
the same time.

That is what lets `blk_sd` stand where `tb/cpp/disk.h` stands without the SCSI
side noticing, and it is why the regression does not have to simulate an SD
card to test SCSI.  A card that has to be initialised and clocked out a bit at
a time buries a SCSI failure in a hundred thousand clocks of unrelated
traffic - the `sd_` tests take longer than the whole of the rest of the suite.

The model's latency is adjustable and is not zero by default, because a back
end that answers instantly hides a target that forgets to wait for one.

`doc/sd.md` describes what is on the other side of that seam.

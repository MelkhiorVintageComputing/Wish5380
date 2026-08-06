# The block back end

The seam between `scsi_targ` and whatever holds the bytes.  On one side a SCSI
target that knows about logical block addresses and nothing about media; on
the other `blk_sd` and a card, or `tb/cpp/disk.h` and an array in host memory,
or something not yet written.

This is the only interface in the design with no external authority behind it.
Wishbone is a published specification, the SCSI bus is a standard, the SD card
protocol is a specification, and the register map is the datasheet's.  This one
was invented here, so it is written down here.  `doc/interface.md` names it as
the seam that makes the rest testable; this document is what it actually is.

**It is deliberately the narrowest thing that works**, because it is the seam a
substitution happens at, and every signal on it is a signal a replacement has
to implement.  `doc/storage.md` argues for making a drive smaller than a card,
and this is the interface that change lands on - so what follows is both the
contract as it stands and the thing that has to grow.

## The shape of it

One 512-byte block at a time, through a buffer that lives in the target.

```
   scsi_targ                                   blk_sd  /  disk
  ┌───────────────────┐                       ┌────────────────┐
  │                   │  blk_req_t       ───► │                │
  │                   │    start, we, lba     │                │
  │                   │    buf_rdata          │                │
  │   ┌───────────┐   │                       │                │
  │   │  512-byte │   │  ◄───    blk_rsp_t    │                │
  │   │   buffer  │   │    done, err          │                │
  │   └───────────┘   │    ready, count       │                │
  │                   │    buf_we/addr/wdata  │                │
  └───────────────────┘                       └────────────────┘
```

The back end never sees the SCSI bus and the target never sees a card.  The
transfer is not a stream: the back end fills the whole buffer before a READ
and drains the whole buffer after a WRITE, and only then says so.

512-byte blocks, which is the SD card's unit and the sector size every vintage
driver expects.  There is no block size parameter; see *What it does not do*.

## The signals

Two packed structs declared at file scope in `src/wish5380_pkg.sv`, beside
`scsi_t`.  **`blk_req_t` always flows from the target to the back end and
`blk_rsp_t` always the other way**, whichever module is being looked at, so
`scsi_targ` has `output blk_req_t blk_o` and `blk_sd` has `input blk_req_t
blk_i` and the same names mean the same things at both ends.

| field | width | struct | meaning |
|---|---|---|---|
| `start`     | 1  | req | one cycle: begin a transfer |
| `we`        | 1  | req | 1 writes the buffer to the media, 0 fills it |
| `lba`       | 32 | req | the block, valid with `start` |
| `buf_rdata` | 8  | req | the buffer's answer to `buf_addr`, one cycle late |
| `done`      | 1  | rsp | one cycle: the transfer is over |
| `err`       | 1  | rsp | only meaningful in the cycle `done` is high |
| `ready`     | 1  | rsp | media present and initialised |
| `count`     | 32 | rsp | capacity, in 512-byte blocks |
| `buf_we`    | 1  | rsp | write strobe into the sector buffer |
| `buf_addr`  | 9  | rsp | the back end's port address |
| `buf_wdata` | 8  | rsp | the byte being written |

`buf_rdata` is in the *request* even though it is data the back end consumes,
because the sector buffer lives in the target: it is the target answering, and
it travels with everything else the target drives.

Everything is synchronous to `clk_i` and there is no other clock.  A back end
that runs at its own rate - the card does, at 400 kHz and then 25 MHz - divides
it internally, and the interface stays at system rate.

**Why structs and not a SystemVerilog interface**, which is what a bundle with
two directions is for.  Icarus 11 raises a syntax error on any interface port,
so `make lint-icarus` would not run at all.  Yosys 0.23 is worse: it does not
implement interfaces, invents implicitly declared wires for the fields, and
elaborates a netlist in which the instance is simply not connected *without
reporting an error*, so `make synth` would go on passing while checking
nothing.  A packed struct gives the same named type and the same direction
enforcement - a port is `input` or `output`, which is what a modport is for -
and all three tools agree on it.  `scsi_t` was already here for the same
reason.

Two consequences worth knowing.  Neither Icarus nor Yosys accepts a `'{field:
value}` assignment pattern, so a struct is assembled field by field.  And
Verilator treats a packed struct as one signal, so both modules gather their
outputs from ordinary local signals in one place rather than writing struct
fields where the values are produced - a struct written by two processes, or
by one process and one continuous assignment, is multiply driven as far as a
linter is concerned even though no bit of it is.

## The handshake

1. The target puts `lba` and `we` up and pulses `start` for one cycle.  For a
   WRITE, the buffer already holds the data.
2. The back end does whatever it does, for as long as it takes.  There is no
   timeout on this side; the driver's own timeout is the only one.
3. The back end pulses `done` for one cycle, with `err` beside it.  For a
   READ, the buffer holds the data by then.

`start` is a pulse and not a level, so there is no acknowledge and no busy:
the back end owns the transaction from the pulse to `done`.  The target issues
no second `start` until it has seen `done`, and a multi-block command becomes
several of these, one per block, with the SCSI data phase in between.

## Three rules that are easy to get subtly wrong

Each is a real property of the code on both sides, and a back end that breaks
one fails in a way that looks like something else.

* **The buffer read is registered, so `buf_rdata` is one cycle behind
  `buf_addr`.**  A back end must present the address at least one cycle
  before it wants the byte.  `blk_sd` does it with room to spare - it sets the
  address to zero two states before the first data byte leaves, and the comment
  there says why.  Getting this wrong writes the whole block shifted by one
  byte, which a filesystem notices long after the transfer said it succeeded.

* **`err` is a level that is only meaningful in the cycle `done` is high.**
  In `blk_sd` it is a latch, cleared when a transfer starts and set by whatever
  went wrong; between transfers it holds whatever the last one left there.
  Reading it at any other time reads history.  `scsi_targ` samples the two
  together - `assign media_fault = blk_i.done && blk_i.err;` - and a back end
  may not assume it is looked at anywhere else.

* **A request made while `ready` is false must still be answered.**  Not
  ready is not permission to stay silent: `blk_sd`'s dead state answers every
  `start` with `done` and `err` together, so the target reports NOT READY
  with MEDIUM NOT PRESENT and the driver hears something.  A back end that left
  a request hanging would hang the SCSI bus, and the failure would present as a
  driver timeout with no fault anywhere near the card.

## Who owns the buffer

The memory is dual-ported and **there is no arbitration, because none is
needed**: the SCSI side and the back end are never active at the same time.

```systemverilog
always_ff @(posedge clk_i) begin
  if (a_we) mem[a_addr] <= a_wdata;
  if (blk_i.buf_we) mem[blk_i.buf_addr] <= blk_i.buf_wdata;
  a_rdata    <= mem[a_addr];
  sbuf_rdata <= mem[blk_i.buf_addr];
end
```

The two write ports are separate statements in one `always_ff`, so a
simultaneous write to the same address would resolve by source order rather
than by any rule worth relying on.  That is not a latent bug, it is the
consequence of the ownership rule: between `start` and `done` the buffer
belongs to the back end, and outside that window it belongs to the SCSI
side.  **A back end that touched the buffer outside its window would corrupt
data with no error anywhere**, which is the strongest reason the rule is stated
rather than enforced - enforcing it costs a port and a comparator on a
condition that cannot arise unless a back end is already wrong.

## What it does not do, and why

* **No multi-block transfer.**  A READ of thirty-two blocks is thirty-two
  transactions.  The card protocol has CMD18 and CMD25 for runs and `doc/sd.md`
  notes them as a straightforward addition, but the gain would be in the card
  protocol rather than on the SCSI bus, which is the slower side.
* **No queueing and no overlap.**  One outstanding transaction, always.  A
  back end with real seek latency would benefit from a second, and nothing here
  reaches for it because the target has one buffer.
* **No byte enables and no partial blocks.**  Every transfer is all 512 bytes.
* **No block size.**  It is 512 on both sides and in the SCSI target's
  READ CAPACITY.  SCSI2SD-style configurations carry a `bytesPerSector` and
  this does not.
* **No offset and no extent.**  LBA 0 is media block 0 and the media is as
  large as `count` says.  This is the assumption `doc/storage.md` is about.
* **No write protect, no medium change, no eject.**  `ready` is the whole
  of the media state.  Card detect and write protect are switches in the
  socket, which is the board's business rather than this interface's.

## Where it would have to grow

`doc/storage.md` argues that several drives should share one card, addressed as
several SCSI IDs.  Every part of that lands here, and it is worth being precise
about which parts are cheap:

* **An extent is cheap.**  A base added to `lba` and a count reported instead
  of the card's own is a shim at this seam, and needs no change to either
  side's protocol - only to what the numbers mean.  `count` already exists and
  is already what READ CAPACITY reports.  Two more fields in `blk_req_t` would
  do it, and adding a field to a struct changes one declaration rather than
  every port list between here and the top - which is the whole reason these
  are structs.
* **Identity is not, quite.**  A per-drive vendor, product and revision would
  have to arrive from the card, and `scsi_targ` takes them as elaboration
  parameters today.  Those become ports, which is a small change with a wide
  blast radius.
* **Reading the table is the real work**, and it is not on this interface at
  all: something has to read two sectors before any target answers and
  distribute what it finds.  Nothing in the design does anything before the
  targets are live.

Reading the configuration would also want a path to the media that is not a
target's buffer, which is the first thing on this list that the interface as
drawn cannot express.

## What is on either side today

| side | module | notes |
|---|---|---|
| host | `scsi_targ` | one instance per drive, each with its own buffer |
| back end | `blk_sd` | `src/blk_sd.sv` over `src/sd_spi.sv`; see `doc/sd.md` |
| back end | `disk` | `tb/cpp/disk.h`, a C++ model with adjustable latency |

`disk` is the reason the seam exists.  The bulk of the regression drives
`wish5380_wb` against it, and only the thirteen `sd_` tests pay for a card that
has to be initialised at 400 kHz before it will say anything.  Its latency is
adjustable and deliberately not zero, because a back end that answers in the
same cycle would hide a target that forgot to wait for one.

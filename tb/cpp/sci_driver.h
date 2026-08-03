// SPDX-License-Identifier: MIT
//
// A SCSI initiator driver, in software, driving the chip's registers.
//
// It follows `doc/drivers/Linux/NCR5380.c` step for step, because the point of
// the whole project is that a driver written for the real part works against
// this one.  `NCR5380_select` becomes `select()`, `NCR5380_transfer_pio`
// becomes `pio()`, and `NCR5380_information_transfer`'s phase loop becomes
// `execute()`.  Where the driver waits, this waits, and for the same reason.
//
// Everything is programmed I/O.  The chip's DMA mode automates the handshake
// but not the byte count, so a driver still has to poll; pseudo-DMA belongs
// with `wb_5380`, which is what gives it an aperture to poll against.
//
// The accessors block: they pump the simulation until the chip answers, so a
// test reads like the driver it is imitating.  Never call one from inside a
// clock callback.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "ncr5380.h"
#include "sim.h"
#include "util.h"

namespace wtb {

// How the driver reaches the chip.  A function pair rather than a pointer to
// the environment, so the driver knows nothing about Verilator and can later
// be pointed at a Wishbone front end instead.
struct RegPort {
  std::function<void(uint8_t, uint8_t)> write;
  std::function<uint8_t(uint8_t)> read;
  // The pseudo-DMA window, `n` bytes in one bus cycle.  False means the bus
  // errored - which on the Mac is how the hardware says the chip did not
  // produce a byte in time, and is a normal outcome rather than a fault.
  std::function<bool(uint8_t*, size_t)> pdma_read;
  std::function<bool(const uint8_t*, size_t)> pdma_write;
};

class SciDriver {
 public:
  SciDriver(Sim& sim, RegPort port, uint8_t host_id = 7);

  // ---- the pieces ---------------------------------------------------------

  // Pulses RST and waits out the bus clear delay, as `do_reset` does.
  void reset_bus();

  // Arbitrate, then select the target with ATN asserted so the first phase is
  // MESSAGE OUT.  False if arbitration was lost or nothing answered.
  bool select(uint8_t target);

  // One byte at a time in the given phase, stopping early if the target
  // changes phase.  Returns how many bytes moved.  `in` may be null for an
  // output phase and `out` null for an input one.
  size_t pio(uint8_t phase, const uint8_t* out, uint8_t* in, size_t n);

  // The same, through the pseudo-DMA window: the chip automates the REQ/ACK
  // handshake and the aperture waits for DRQ, so the host moves bytes with
  // ordinary loads and stores.  This is `NCR5380_transfer_dma` followed by
  // `macscsi_pdma_recv` or `macscsi_pdma_send`.
  size_t pdma(uint8_t phase, const uint8_t* out, uint8_t* in, size_t n);

  // ---- a whole command ----------------------------------------------------

  struct Result {
    bool ok = false;          // the command ran to COMMAND COMPLETE
    uint8_t status = 0xff;
    uint8_t message = 0xff;
    Bytes data;               // whatever came back in DATA IN
  };

  Result execute(uint8_t target, const Bytes& cdb,
                 const Bytes& data_out = Bytes(), size_t max_in = 0,
                 uint8_t lun = 0);

  // Whether the data phases go through the pseudo-DMA window, and how many
  // bytes each bus cycle moves.  Two is the Mac's `movew`, which is the
  // widest access its driver makes.
  bool use_pdma = false;
  size_t pdma_width = 2;

  const std::string& last_error() const { return err_; }

  // ---- the driver's own waits ---------------------------------------------
  //
  // The selection timeout is the one deliberate departure.  The standard, and
  // Linux, allow 250 ms; this allows one, which is fifty thousand clocks
  // against the twenty-odd the target needs.  A shorter timeout can only make
  // a test stricter, never hide a fault, and it keeps a failing test from
  // simulating a quarter of a second of nothing.
  u64 t_select = 1 * MS;
  // Linux allows a whole second for REQ.  Twenty milliseconds is far stricter
  // and still twenty times the longest legitimate wait here, which is a block
  // fetched from an SD card clocked off a slow system clock.  It was one
  // millisecond until that case turned up, and one millisecond was a
  // testbench that ran out of patience before the hardware did - the opposite
  // of the fault a timeout is meant to catch, and worth remembering.
  u64 t_req = 20 * MS;
  u64 t_arb = sci::T_ARB_DELAY_PS;      // 2.2 us: the chip does not do it
  u64 t_bus_settle = sci::T_BUS_SETTLE_PS;

 private:
  void w(uint8_t reg, uint8_t val) { port_.write(reg, val); }
  uint8_t r(uint8_t reg) { return port_.read(reg); }
  void delay(u64 ps) { sim_.run_ps(ps); }
  bool poll(uint8_t reg, uint8_t mask, uint8_t val, u64 timeout,
            const char* what);

  Sim& sim_;
  RegPort port_;
  uint8_t host_id_;
  uint8_t id_mask_;
  std::string err_;
};

}  // namespace wtb

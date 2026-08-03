// SPDX-License-Identifier: MIT
//
// Per-test environment: a fresh DUT, its clock, and the models around it.
//
// Each test gets its own Env, so nothing leaks from one test to the next and a
// failing test can be rerun on its own with a waveform.

#pragma once

#include <memory>
#include <string>

#include "disk.h"
#include "ncr5380.h"
#include "sci_driver.h"
#include "sd_card.h"
#include "sim.h"
#include "util.h"
#include "wb.h"

class VerilatedContext;
class VerilatedVcdC;
class Vtb_top;

namespace wtb {

// The system clock period in picoseconds, set by the Makefile.  The 5380 is a
// clockless part whose delays come out of gate propagation, so everything the
// RTL counts out - the 400 ns bus free filter, the 800 ns bus clear delay - is
// derived from this number rather than written down.  Build for a slower
// machine with `make test SYS_PERIOD_PS=50000` and both the RTL and the
// testbench follow.
#ifndef SYS_PERIOD_PS
#define SYS_PERIOD_PS 20000
#endif

// Bytes between one register and the next, set by the Makefile's BOARD.  The
// same number gives the RTL its parameter, so the two cannot drift.
#ifndef REG_STRIDE
#define REG_STRIDE 16
#endif

struct EnvConfig {
  u64 sys_period_ps = u64(SYS_PERIOD_PS);
  // The card behind the target, in 512-byte blocks.  Two thousand and
  // forty-eight is one mebibyte, which is enough to exercise multi-block
  // transfers and small enough that the model is free.
  uint32_t disk_blocks = 2048;
  // The SCSI ID the target answers to, and the one the driver arbitrates
  // with.  A host adapter is conventionally 7 and Apple's internal drive 0.
  uint8_t target_id = 0;
  uint8_t host_id = 7;

  // Where the three windows sit inside the slave, stated here independently
  // of `wb_5380`'s parameters so the two have to be kept in step by hand.
  // Sixteen bytes between registers is the Mac's `(reg) << 4`.
  uint32_t reg_base = 0x000;
  uint32_t reg_stride = REG_STRIDE;
  uint32_t hsk_base = 0x100;   // pseudo-DMA that waits for DRQ
  uint32_t dma_base = 0x200;   // pseudo-DMA that does not

  // The card behind the second instance.  A multiple of 1024 blocks, because
  // a version 2 card states its size in half-megabytes and cannot describe
  // anything else.
  uint32_t sd_blocks = 4096;   // two mebibytes
};

class Env {
 public:
  Env(const std::string& test_name, bool trace, EnvConfig cfg = EnvConfig());
  ~Env();

  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;

  // Pulses the global (power on) reset and lets things settle.
  void power_on_reset(int cycles = 8);
  void tick(int cycles = 1);

  Vtb_top* dut() { return dut_.get(); }
  Sim& sim() { return *sim_; }
  Disk& disk() { return *disk_; }
  SciDriver& drv() { return *drv_; }
  WbHost& host() { return *host_; }

  // The second instance: the same design with a real card behind it, which is
  // what the sd_ tests drive.  Everything else uses the fast disk.
  WbHost& sd_host() { return *sd_host_; }
  SciDriver& sd_drv() { return *sd_drv_; }
  SdCard& card() { return *card_; }
  // Register and pseudo-DMA access to that instance.
  void sd_chip_write(uint8_t adr, uint8_t data);
  uint8_t sd_chip_read(uint8_t adr);
  // Polls TEST UNIT READY until the card has finished initialising, the way a
  // driver waits for a drive to spin up.  False if it never does.
  //
  // Thirty milliseconds, because a card takes a good five: one to power up,
  // and the rest clocked out at 400 kHz because that is all it will accept
  // until it is up.  A driver waits far longer than this - Linux allows a
  // drive tens of seconds to spin - so the budget is the test's patience and
  // not the driver's.
  bool wait_card_ready(u64 timeout_ps = 30 * MS);
  Sim::Clock* sysclk() { return sysclk_; }
  const EnvConfig& cfg() const { return cfg_; }
  const std::string& name() const { return name_; }

  // ---- the register file, poked on its own -------------------------------
  //
  // These drive the leaf module wired into tb_top beside the chip, so a test
  // can check the register map without a bus or an engine in the way.  They
  // are blocking: each runs the clock, in the same way a driver's register
  // access blocks until the chip answers.
  void reg_write(uint8_t adr, uint8_t data);
  uint8_t reg_read(uint8_t adr);
  // The same two with DACK asserted, which reaches a data register whatever
  // the address lines say (p. 6).
  void reg_write_dack(uint8_t data);
  uint8_t reg_read_dack();

  // Which strobes the register file raised during the last access.  They are
  // one cycle wide and combinational on the access itself, so there is no way
  // for a test to catch them after the fact; the accessors above sample them
  // while the strobe is still up.
  enum Strobe : uint8_t { S_RPI = 1, S_SDS = 2, S_SDTR = 4, S_SDIR = 8 };
  uint8_t last_strobes() const { return strobes_; }

  // ---- the whole product, through its Wishbone slave ----------------------
  //
  // A register access is one byte in the register window, which is how every
  // driver reaches the chip.  These are what the driver model is built on.
  void chip_write(uint8_t adr, uint8_t data);
  uint8_t chip_read(uint8_t adr);
  // A byte through the pseudo-DMA window that does not wait for DRQ, which is
  // what a DACK cycle used to be before there was a bus to put it on.
  void chip_write_dack(uint8_t data);
  uint8_t chip_read_dack();

  // ---- the pseudo-DMA windows --------------------------------------------
  //
  // `n` is 1, 2 or 4 bytes in one Wishbone cycle, which is a `moveb`, a
  // `movew` or a `movel` on the machine's side.  The Mac's driver only ever
  // uses the first two.  `error` is the bus error the handshaking window
  // raises when the chip does not produce a byte in time - a normal outcome
  // that `mac_scsi.c` catches with an exception fixup table, not a fault.
  struct Pdma {
    Bytes data;
    bool error = false;
  };
  Pdma pdma_read(size_t n, bool handshake = true);
  Pdma pdma_write(const Bytes& b, bool handshake = true);

  // A raw access, for the tests that are about the decode rather than about
  // what is behind it.  Returns whether the slave answered with ERR.
  bool wb_err_on_read(uint32_t byte_adr, uint8_t sel);
  bool wb_err_on_write(uint32_t byte_adr, uint8_t sel, uint32_t data);

  // ---- the peer device on the fabric -------------------------------------
  //
  // Whatever else is on the bus: a target answering a selection, or another
  // initiator arbitrating.  A test drives it signal by signal because that is
  // the level the chip is specified at; `sci_target.h` will wrap it once
  // there is a target worth modelling.
  struct Peer {
    bool rst = false, bsy = false, sel = false, req = false, ack = false;
    bool atn = false, msg = false, cd = false, io = false, dbp = false;
    uint8_t data = 0;

    // Sets MSG, C/D and I/O from a phase in the Target Command Register's
    // encoding, and the data lines with correct odd parity.
    Peer& phase(uint8_t ph);
    Peer& with_data(uint8_t d, bool good_parity = true);
  };

  void drive_peer(const Peer& p);
  const Peer& peer() const { return peer_; }
  // Changes one field and re-drives, for the many tests that toggle a single
  // signal in the middle of a sequence.
  void peer_set(bool Peer::*field, bool v);

  // The bus as every device sees it, in the Current SCSI Bus Status
  // Register's layout, so it can be compared against what a read returns.
  uint8_t bus_csb();
  uint8_t bus_data();
  bool bus_ack();
  bool bus_req();

  // How many clocks a delay of dt_ps takes, rounded up the way the RTL
  // rounds it.  Tests that check the part's timings ask for this rather than
  // writing a number that only holds at one clock.
  uint64_t ticks_for_ps(uint64_t dt_ps) const {
    return (dt_ps + cfg_.sys_period_ps - 1) / cfg_.sys_period_ps;
  }

 private:
  void bind_models();

  std::string name_;
  EnvConfig cfg_;
  std::unique_ptr<VerilatedContext> ctx_;
  std::unique_ptr<Vtb_top> dut_;
  std::unique_ptr<VerilatedVcdC> tfp_;
  std::unique_ptr<Sim> sim_;
  Sim::Clock* sysclk_ = nullptr;
  void sample_strobes();
  uint8_t strobes_ = 0;
  Peer peer_;
  std::unique_ptr<Disk> disk_;
  std::unique_ptr<SciDriver> drv_;
  std::unique_ptr<WbHost> host_;
  std::unique_ptr<WbHost> sd_host_;
  std::unique_ptr<SciDriver> sd_drv_;
  std::unique_ptr<SdCard> card_;
};

}  // namespace wtb

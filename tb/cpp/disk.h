// SPDX-License-Identifier: MIT
//
// The block back end the target talks to, in software.
//
// It stands in for `blk_sd` until that exists, and afterwards it stays: an SD
// card model is the right thing to test the SD layer against, and the wrong
// thing to test SCSI against, because a card that has to be initialised and
// clocked out a bit at a time buries a SCSI failure in a hundred thousand
// clocks of unrelated traffic.
//
// It attaches on the negative edge of the system clock, like every other
// model here, and moves one byte per clock through the target's sector buffer
// port.  The latency is deliberately adjustable: a back end that answers
// instantly hides a target that forgets to wait for it.

#pragma once

#include <cstdint>
#include <vector>

#include "sim.h"
#include "util.h"

namespace wtb {

struct DiskPorts {
  uint8_t* start = nullptr;   // target -> model
  uint8_t* we = nullptr;
  uint32_t* lba = nullptr;
  uint8_t* done = nullptr;    // model -> target
  uint8_t* err = nullptr;
  uint8_t* ready = nullptr;
  uint32_t* count = nullptr;
  uint8_t* buf_we = nullptr;  // the model's port into the sector buffer
  uint16_t* buf_addr = nullptr;   // nine bits, so Verilator makes it short
  uint8_t* buf_wdata = nullptr;
  uint8_t* buf_rdata = nullptr;
};

class Disk {
 public:
  static constexpr size_t BLOCK = 512;

  Disk(Sim& sim, Sim::Clock* clk, DiskPorts ports, uint32_t blocks);

  // ---- backdoor, for setting a test up and checking it afterwards ---------
  Bytes read_block(uint32_t lba) const;
  void write_block(uint32_t lba, const Bytes& data);
  void fill_pattern(uint32_t lba, uint32_t seed);   // random_block, reproducibly

  // ---- configuration ------------------------------------------------------
  // Clocks between the start pulse and the transfer beginning.  An SD card in
  // SPI mode is far slower than this; what matters is that it is not zero.
  void set_latency(int clocks) { latency_ = clocks; }
  void set_ready(bool r) { ready_ = r; }
  // The next access to this block answers with an error.  -1 disarms it.
  void fail_on(int64_t lba) { fail_lba_ = lba; }

  // ---- observation --------------------------------------------------------
  size_t reads() const { return reads_; }
  size_t writes() const { return writes_; }
  uint32_t last_lba() const { return last_lba_; }
  void clear_counts() { reads_ = writes_ = 0; }

 private:
  void tick();

  enum class St { Idle, Wait, Move, Done };

  Sim& sim_;
  DiskPorts p_;
  std::vector<uint8_t> media_;
  uint32_t blocks_;
  St st_ = St::Idle;
  int latency_ = 4;
  int wait_ = 0;
  uint32_t addr_ = 0;
  uint32_t lba_ = 0;
  bool we_ = false;
  bool ready_ = true;
  int64_t fail_lba_ = -1;
  size_t reads_ = 0, writes_ = 0;
  uint32_t last_lba_ = 0;
};

}  // namespace wtb

// SPDX-License-Identifier: MIT
//
// An SD card in SPI mode, in software.
//
// It answers the commands `blk_sd` sends and no others, which is the same
// bargain the RTL makes: this is not a card, it is what a card has to look
// like to the initialisation sequence and to CMD17 and CMD24.
//
// The model is deliberately strict where the RTL could get away with being
// wrong and nobody would notice:
//
//   * it checks the CRC7 on CMD0 and CMD8, because those are the two commands
//     a real card still checks, and a driver that got either constant wrong
//     would work against a lax model and fail against silicon;
//   * it computes a real CRC16 over the data of every block it sends, because
//     the RTL checks it;
//   * it holds MISO low for a while after a write, because a card that
//     answered instantly would hide a controller that forgot to wait.
//
// It watches the SPI pins from the system clock's negative edge and finds the
// card clock's edges itself, so it needs no clock of its own.

#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "sim.h"
#include "util.h"

namespace wtb {

struct SdPorts {
  uint8_t* sclk = nullptr;
  uint8_t* cs_n = nullptr;
  uint8_t* mosi = nullptr;
  uint8_t* miso = nullptr;   // driven by the model
};

class SdCard {
 public:
  static constexpr size_t BLOCK = 512;

  // `blocks` is the capacity.  A card of more than four gibibytes must be high
  // capacity; below that either is legal and real cards of both exist, which
  // is why the version is a parameter and not a constant.
  SdCard(Sim& sim, Sim::Clock* clk, SdPorts ports, uint32_t blocks,
         bool high_capacity = true);

  // ---- backdoor -----------------------------------------------------------
  Bytes read_block(uint32_t lba) const;
  void write_block(uint32_t lba, const Bytes& data);
  void fill_pattern(uint32_t lba, uint32_t seed);

  // ---- what to pretend ----------------------------------------------------
  // No card in the slot: MISO floats high and nothing ever answers.
  void set_present(bool p) { present_ = p; }
  // Which kind of card is in the slot.  Below four gibibytes either is legal
  // and real ones of both exist, and they are addressed differently.  Set it
  // before the reset that starts initialisation.
  void set_high_capacity(bool h) { hc_ = h; }
  // How many idle bytes the card takes to answer a command, and how long it
  // holds the line low after a write.  Neither is zero on real hardware.
  void set_ncr(int bytes) { ncr_ = bytes; }
  void set_busy_bytes(int n) { busy_bytes_ = n; }
  // The next read of this block answers with an error token; the next read of
  // that one answers with good data and a wrong CRC16.  -1 disarms.
  void fail_read_on(int64_t lba) { fail_read_ = lba; }
  void corrupt_read_on(int64_t lba) { corrupt_read_ = lba; }
  void fail_write_on(int64_t lba) { fail_write_ = lba; }

  // ---- observation --------------------------------------------------------
  size_t reads() const { return reads_; }
  size_t writes() const { return writes_; }
  bool initialised() const { return !idle_; }
  // Commands seen, most recent last, for a test that wants to check the
  // initialisation order rather than only its result.
  const std::vector<uint8_t>& command_log() const { return cmds_; }
  void clear_counts() { reads_ = writes_ = 0; }

 private:
  void tick();
  void on_byte(uint8_t rx);
  void push(uint8_t b) { tx_.push_back(b); }
  void push_r1(uint8_t r1);
  void push_block(const Bytes& data, bool bad_crc);
  Bytes csd() const;

  enum class M { Cmd, WrToken, WrData, WrBusy };

  Sim& sim_;
  SdPorts p_;
  std::vector<uint8_t> media_;
  uint32_t blocks_;
  bool hc_;                    // high capacity: addressed by block

  bool prev_clk_ = false;
  int bit_ = 0;
  uint8_t sh_rx_ = 0;
  uint8_t cur_tx_ = 0xff;
  std::deque<uint8_t> tx_;

  M mode_ = M::Cmd;
  std::vector<uint8_t> cmd_;
  bool acmd_ = false;          // the last command was CMD55
  bool idle_ = true;           // still reporting R1 bit 0
  int acmd41_left_ = 3;        // how many more ACMD41s before it comes up
  uint32_t wr_lba_ = 0;
  size_t wr_got_ = 0;
  Bytes wr_buf_;

  bool present_ = true;
  int ncr_ = 2;
  int busy_bytes_ = 4;
  int64_t fail_read_ = -1, corrupt_read_ = -1, fail_write_ = -1;
  size_t reads_ = 0, writes_ = 0;
  std::vector<uint8_t> cmds_;
};

// CRC7 over a command, and CRC16-CCITT over a data block: the two the card
// computes and the two `blk_sd` has to agree with.
uint8_t sd_crc7(const uint8_t* data, size_t len);
uint16_t sd_crc16(const uint8_t* data, size_t len);

}  // namespace wtb

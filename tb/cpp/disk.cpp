// SPDX-License-Identifier: MIT

#include "disk.h"

#include <cstring>

namespace wtb {

Disk::Disk(Sim& sim, Sim::Clock* clk, DiskPorts ports, uint32_t blocks)
    : sim_(sim), p_(ports), media_(size_t(blocks) * BLOCK, 0), blocks_(blocks) {
  *p_.done = 0;
  *p_.err = 0;
  *p_.ready = 1;
  *p_.count = blocks;
  *p_.buf_we = 0;
  *p_.buf_addr = 0;
  *p_.buf_wdata = 0;
  sim_.on_negedge(clk, [this]() { tick(); });
}

Bytes Disk::read_block(uint32_t lba) const {
  if (lba >= blocks_) return Bytes();
  const uint8_t* p = media_.data() + size_t(lba) * BLOCK;
  return Bytes(p, p + BLOCK);
}

void Disk::write_block(uint32_t lba, const Bytes& data) {
  if (lba >= blocks_) return;
  uint8_t* p = media_.data() + size_t(lba) * BLOCK;
  size_t n = data.size() < BLOCK ? data.size() : BLOCK;
  std::memcpy(p, data.data(), n);
}

void Disk::fill_pattern(uint32_t lba, uint32_t seed) {
  write_block(lba, random_block(BLOCK, seed));
}

void Disk::tick() {
  *p_.ready = ready_ ? 1 : 0;
  *p_.count = blocks_;
  *p_.done = 0;
  *p_.err = 0;
  *p_.buf_we = 0;

  switch (st_) {
    case St::Idle:
      if (*p_.start) {
        we_ = (*p_.we != 0);
        lba_ = *p_.lba;
        last_lba_ = lba_;
        addr_ = 0;
        wait_ = latency_;
        st_ = St::Wait;
      }
      break;

    case St::Wait:
      if (wait_-- <= 0) st_ = St::Move;
      break;

    case St::Move: {
      size_t base = size_t(lba_) * BLOCK;
      bool oob = lba_ >= blocks_;
      if (!we_) {
        // Media to buffer: one byte written into the target's sector buffer
        // per clock.
        *p_.buf_we = 1;
        *p_.buf_addr = uint16_t(addr_);
        *p_.buf_wdata = oob ? 0 : media_[base + addr_];
        if (++addr_ == BLOCK) st_ = St::Done;
      } else {
        // Buffer to media.  The buffer's read is registered, so the byte for
        // the address presented last clock arrives now - which is why this
        // runs one step past the end.
        if (addr_ > 0 && !oob) media_[base + addr_ - 1] = *p_.buf_rdata;
        if (addr_ < BLOCK) *p_.buf_addr = uint16_t(addr_);
        if (++addr_ > BLOCK) st_ = St::Done;
      }
      break;
    }

    case St::Done:
      *p_.done = 1;
      *p_.err = (fail_lba_ >= 0 && uint32_t(fail_lba_) == lba_) ? 1 : 0;
      if (!*p_.err) {
        if (we_) writes_++; else reads_++;
      }
      st_ = St::Idle;
      break;
  }
}

}  // namespace wtb

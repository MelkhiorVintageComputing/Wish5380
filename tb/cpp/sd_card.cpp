// SPDX-License-Identifier: MIT

#include "sd_card.h"

#include <cstring>

namespace wtb {

uint8_t sd_crc7(const uint8_t* data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    uint8_t d = data[i];
    for (int b = 7; b >= 0; b--) {
      uint8_t in = uint8_t(((d >> b) & 1) ^ ((crc >> 6) & 1));
      crc = uint8_t((crc << 1) & 0x7f);
      if (in) crc ^= 0x09;
    }
  }
  return crc;
}

uint16_t sd_crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    for (int b = 7; b >= 0; b--) {
      bool in = (((data[i] >> b) & 1) != 0) ^ ((crc & 0x8000) != 0);
      crc = uint16_t(crc << 1);
      if (in) crc ^= 0x1021;
    }
  }
  return crc;
}

SdCard::SdCard(Sim& sim, Sim::Clock* clk, SdPorts ports, uint32_t blocks,
               bool high_capacity)
    : sim_(sim), p_(ports), media_(size_t(blocks) * BLOCK, 0), blocks_(blocks),
      hc_(high_capacity) {
  *p_.miso = 1;
  sim_.on_negedge(clk, [this]() { tick(); });
}

Bytes SdCard::read_block(uint32_t lba) const {
  if (lba >= blocks_) return Bytes();
  const uint8_t* q = media_.data() + size_t(lba) * BLOCK;
  return Bytes(q, q + BLOCK);
}

void SdCard::write_block(uint32_t lba, const Bytes& data) {
  if (lba >= blocks_) return;
  uint8_t* q = media_.data() + size_t(lba) * BLOCK;
  size_t n = data.size() < BLOCK ? data.size() : BLOCK;
  std::memcpy(q, data.data(), n);
}

void SdCard::fill_pattern(uint32_t lba, uint32_t seed) {
  write_block(lba, random_block(BLOCK, seed));
}

// The Card Specific Data register, which is where a host learns the capacity.
// Two layouts, and they state the size in quite different ways.
Bytes SdCard::csd() const {
  Bytes c(16, 0);
  if (hc_) {
    // Version 2: a count of half-megabytes, and nothing else matters.
    uint32_t c_size = blocks_ / 1024 - 1;
    c[0] = 0x40;                       // CSD_STRUCTURE = 1
    c[1] = 0x0e;                       // TAAC
    c[2] = 0x00;                       // NSAC
    c[3] = 0x32;                       // TRAN_SPEED, 25 MHz
    c[4] = 0x5b;                       // command classes
    c[5] = 0x59;                       // ...and READ_BL_LEN = 9
    c[6] = 0x00;
    c[7] = uint8_t((c_size >> 16) & 0x3f);
    c[8] = uint8_t((c_size >> 8) & 0xff);
    c[9] = uint8_t(c_size & 0xff);
    c[10] = 0x7f;
    c[11] = 0x80;
    c[12] = 0x0a;
    c[13] = 0x40;
    c[14] = 0x00;
  } else {
    // Version 1: (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) blocks of 2^READ_BL_LEN.
    // With READ_BL_LEN 9 and C_SIZE_MULT 7 that is (C_SIZE + 1) * 512 blocks.
    uint32_t c_size = blocks_ / 512 - 1;
    uint32_t mult = 7;
    c[0] = 0x00;                       // CSD_STRUCTURE = 0
    c[1] = 0x26;
    c[2] = 0x00;
    c[3] = 0x32;
    c[4] = 0x5b;
    c[5] = 0x59;                       // READ_BL_LEN = 9
    c[6] = uint8_t(0x80 | ((c_size >> 10) & 0x03));
    c[7] = uint8_t((c_size >> 2) & 0xff);
    c[8] = uint8_t(((c_size & 0x03) << 6) | 0x3f);
    c[9] = uint8_t(0x7c | ((mult >> 1) & 0x03));
    c[10] = uint8_t(((mult & 1) << 7) | 0x7f);
    c[11] = 0x80;
    c[12] = 0x0a;
    c[13] = 0x40;
    c[14] = 0x00;
  }
  c[15] = uint8_t((sd_crc7(c.data(), 15) << 1) | 1);
  return c;
}

void SdCard::push_r1(uint8_t r1) {
  for (int i = 0; i < ncr_; i++) push(0xff);
  push(r1);
}

void SdCard::push_block(const Bytes& data, bool bad_crc) {
  for (int i = 0; i < 2; i++) push(0xff);   // the card thinks about it
  push(0xfe);                               // start of a single block
  for (uint8_t b : data) push(b);
  uint16_t crc = sd_crc16(data.data(), data.size());
  if (bad_crc) crc = uint16_t(crc ^ 0x0001);
  push(uint8_t(crc >> 8));
  push(uint8_t(crc & 0xff));
}

void SdCard::tick() {
  if (!present_) {
    *p_.miso = 1;
    return;
  }

  bool clk = (*p_.sclk != 0);
  bool selected = (*p_.cs_n == 0);

  if (!selected) {
    // Deselected: the card lets go of the line and forgets where it was in a
    // byte.  It does not forget its state, which is why the seventy-four
    // idle clocks at power-up do not undo CMD0.
    *p_.miso = 1;
    bit_ = 0;
    prev_clk_ = clk;
    return;
  }

  if (clk && !prev_clk_) {
    // Rising edge: the card samples what the host is driving.
    sh_rx_ = uint8_t((sh_rx_ << 1) | (*p_.mosi ? 1 : 0));
    bit_++;
  } else if (!clk && prev_clk_) {
    // Falling edge: the card moves on, and at the end of a byte decides what
    // to send during the next one.
    if (bit_ >= 8) {
      bit_ = 0;
      on_byte(sh_rx_);
      cur_tx_ = 0xff;
      if (!tx_.empty()) {
        cur_tx_ = tx_.front();
        tx_.pop_front();
      }
    } else {
      cur_tx_ = uint8_t(cur_tx_ << 1);
    }
  }
  prev_clk_ = clk;

  *p_.miso = (cur_tx_ & 0x80) ? 1 : 0;
}

void SdCard::on_byte(uint8_t rx) {
  switch (mode_) {
    case M::WrToken:
      if (rx == 0xfe) {
        mode_ = M::WrData;
        wr_got_ = 0;
        wr_buf_.assign(BLOCK, 0);
      }
      return;

    case M::WrData:
      if (wr_got_ < BLOCK) {
        wr_buf_[wr_got_] = rx;
      }
      wr_got_++;
      if (wr_got_ == BLOCK + 2) {   // the block and its two CRC bytes
        bool bad = (fail_write_ >= 0 && uint32_t(fail_write_) == wr_lba_);
        // The data response token: xxx0sss1, with 010 for accepted and 110
        // for a write error.
        push(bad ? 0x0d : 0x05);
        for (int i = 0; i < busy_bytes_; i++) push(0x00);
        push(0xff);
        if (!bad) {
          if (wr_lba_ < blocks_) {
            std::memcpy(media_.data() + size_t(wr_lba_) * BLOCK, wr_buf_.data(),
                        BLOCK);
          }
          writes_++;
        }
        mode_ = M::Cmd;
      }
      return;

    case M::WrBusy:
      mode_ = M::Cmd;
      return;

    case M::Cmd:
      break;
  }

  // A command begins with a byte whose top two bits are 01.  Anything else
  // between commands is the host clocking the bus and is ignored.
  if (cmd_.empty()) {
    if ((rx & 0xc0) != 0x40) return;
  }
  cmd_.push_back(rx);
  if (cmd_.size() < 6) return;

  uint8_t idx = uint8_t(cmd_[0] & 0x3f);
  uint32_t arg = (uint32_t(cmd_[1]) << 24) | (uint32_t(cmd_[2]) << 16) |
                 (uint32_t(cmd_[3]) << 8) | cmd_[4];
  uint8_t crc_rx = cmd_[5];
  uint8_t crc_want = uint8_t((sd_crc7(cmd_.data(), 5) << 1) | 1);
  bool was_acmd = acmd_;
  acmd_ = false;
  cmds_.push_back(was_acmd ? uint8_t(idx | 0x80) : idx);
  cmd_.clear();

  // CMD0 and CMD8 arrive before the card stops checking CRCs, so a host that
  // sends the wrong constant for either gets nowhere.  Everything after them
  // may carry whatever it likes, and every SPI-mode driver sends a stop bit
  // and nothing else - which is exactly what makes those two constants worth
  // checking here.
  if ((idx == 0 || idx == 8) && crc_rx != crc_want) {
    push_r1(0x09);   // idle, with the CRC error bit
    return;
  }

  switch (was_acmd ? (idx | 0x100) : idx) {
    case 0:  // GO_IDLE_STATE
      idle_ = true;
      acmd41_left_ = 3;
      push_r1(0x01);
      return;

    case 8: {  // SEND_IF_COND
      if (!hc_) {
        // A version 1 card has never heard of it.
        push_r1(0x05);   // idle, illegal command
        return;
      }
      push_r1(0x01);
      push(0x00);
      push(0x00);
      push(uint8_t((arg >> 8) & 0x0f));   // the voltage range, echoed
      push(uint8_t(arg & 0xff));          // the check pattern, echoed
      return;
    }

    case 55:  // APP_CMD
      acmd_ = true;
      push_r1(idle_ ? 0x01 : 0x00);
      return;

    case (41 | 0x100):  // ACMD41, SD_SEND_OP_COND
      if (acmd41_left_ > 0) {
        acmd41_left_--;
        push_r1(0x01);
      } else {
        idle_ = false;
        push_r1(0x00);
      }
      return;

    case 58:  // READ_OCR
      push_r1(idle_ ? 0x01 : 0x00);
      // Card Capacity Status in bit 30, and a plausible voltage window.
      push(uint8_t(hc_ ? 0xc0 : 0x80));
      push(0xff);
      push(0x80);
      push(0x00);
      return;

    case 16:  // SET_BLOCKLEN
      push_r1(arg == 512 ? 0x00 : 0x40);
      return;

    case 9: {  // SEND_CSD
      push_r1(0x00);
      Bytes c = csd();
      for (int i = 0; i < 2; i++) push(0xff);
      push(0xfe);
      for (uint8_t b : c) push(b);
      uint16_t k = sd_crc16(c.data(), c.size());
      push(uint8_t(k >> 8));
      push(uint8_t(k & 0xff));
      return;
    }

    case 17: {  // READ_SINGLE_BLOCK
      uint32_t lba = hc_ ? arg : (arg >> 9);
      if (lba >= blocks_) {
        push_r1(0x40);   // parameter error
        return;
      }
      push_r1(0x00);
      if (fail_read_ >= 0 && uint32_t(fail_read_) == lba) {
        for (int i = 0; i < 2; i++) push(0xff);
        push(0x08);      // a data error token: out of range
        return;
      }
      bool bad = (corrupt_read_ >= 0 && uint32_t(corrupt_read_) == lba);
      push_block(read_block(lba), bad);
      reads_++;
      return;
    }

    case 24: {  // WRITE_BLOCK
      uint32_t lba = hc_ ? arg : (arg >> 9);
      if (lba >= blocks_) {
        push_r1(0x40);
        return;
      }
      push_r1(0x00);
      wr_lba_ = lba;
      mode_ = M::WrToken;
      return;
    }

    default:
      push_r1(uint8_t(idle_ ? 0x05 : 0x04));   // illegal command
      return;
  }
}

}  // namespace wtb

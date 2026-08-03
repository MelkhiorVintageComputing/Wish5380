// SPDX-License-Identifier: MIT

#include "sci_driver.h"

namespace wtb {

SciDriver::SciDriver(Sim& sim, RegPort port, uint8_t host_id)
    : sim_(sim), port_(std::move(port)), host_id_(host_id),
      id_mask_(uint8_t(1u << host_id)) {}

bool SciDriver::poll(uint8_t reg, uint8_t mask, uint8_t val, u64 timeout,
                     const char* what) {
  u64 t0 = sim_.time_ps();
  // Each read costs a clock, so the loop advances time by itself - the same
  // way the driver's own polling loops do.
  do {
    if ((r(reg) & mask) == val) return true;
  } while (sim_.time_ps() - t0 < timeout);
  err_ = std::string("timed out waiting for ") + what;
  return false;
}

// `do_reset` in NCR5380.c: set the phase bits to whatever the bus is showing,
// assert RST for 50 us, drop it, and acknowledge the interrupt it raised.
void SciDriver::reset_bus() {
  w(sci::R_TCR, sci::csb_to_phase(r(sci::R_CSB)));
  w(sci::R_ICR, sci::ICR_RST);
  delay(50 * US);
  w(sci::R_ICR, 0);
  (void)r(sci::R_RPI);
  delay(sci::T_BUS_CLEAR_PS);
}

bool SciDriver::select(uint8_t target) {
  err_.clear();

  // "Set the phase bits to 0, otherwise the NCR5380 won't drive the data bus
  // during SELECTION."
  w(sci::R_TCR, 0);

  // Arbitration.  Initiator Command is deliberately not touched: the chip
  // asserts BSY and the Output Data Register by itself while ARBITRATE is set.
  w(sci::R_ODR, id_mask_);
  w(sci::R_MR, sci::MR_ARB);

  if (!poll(sci::R_ICR, sci::ICR_AIP, sci::ICR_AIP, t_select,
            "arbitration in progress")) {
    w(sci::R_MR, 0);
    return false;
  }

  // The arbitration delay the chip does not implement.
  delay(t_arb);

  // Lost either to another device's SEL, or to a higher ID on the data bus.
  // The second test is the driver's entirely: the chip does not compare IDs.
  uint8_t higher = uint8_t(~((id_mask_ << 1) - 1));
  if ((r(sci::R_ICR) & sci::ICR_LA) || (r(sci::R_CSD) & higher)) {
    w(sci::R_MR, 0);
    err_ = "lost arbitration";
    return false;
  }

  // Take BSY over from the arbitration logic before the Mode Register is
  // cleared, so it never lets go.
  w(sci::R_ICR, uint8_t(sci::ICR_SEL | sci::ICR_BSY));
  delay(1200 * NS);   // bus clear plus bus settle, the driver's udelay(2)

  // Both IDs on the bus, ATN raised while SEL is true and before BSY goes
  // false - "the only way to guarantee that we'll get a MESSAGE OUT phase
  // immediately after selection".
  w(sci::R_ODR, uint8_t(id_mask_ | (1u << target)));
  w(sci::R_ICR, uint8_t(sci::ICR_BSY | sci::ICR_DATA | sci::ICR_ATN |
                        sci::ICR_SEL));
  w(sci::R_MR, 0);
  // "Reselect interrupts must be turned off prior to the dropping of BSY,
  // otherwise we will trigger an interrupt."
  w(sci::R_SER, 0);
  delay(100 * NS);

  // Release BSY and wait for the target to answer with its own.
  w(sci::R_ICR, uint8_t(sci::ICR_DATA | sci::ICR_ATN | sci::ICR_SEL));
  delay(100 * NS);

  if (!poll(sci::R_CSB, sci::CSB_BSY, sci::CSB_BSY, t_select,
            "the target to respond to selection")) {
    w(sci::R_ICR, 0);
    err_ = "selection timeout";
    return false;
  }

  // Two deskew delays after BSY is seen, release SEL and the data bus, but
  // keep ATN so the target knows a message is coming.
  delay(100 * NS);
  w(sci::R_ICR, sci::ICR_ATN);
  return true;
}

size_t SciDriver::pio(uint8_t phase, const uint8_t* out, uint8_t* in,
                      size_t n) {
  // "The NCR5380 chip will only drive the SCSI bus when the phase specified in
  // the appropriate bits of the TARGET COMMAND REGISTER match the STATUS
  // REGISTER."
  w(sci::R_TCR, phase);

  const bool is_in = (phase & sci::TCR_IO) != 0;
  size_t i = 0;
  for (; i < n; i++) {
    if (!poll(sci::R_CSB, uint8_t(sci::CSB_REQ | sci::CSB_BSY),
              uint8_t(sci::CSB_REQ | sci::CSB_BSY), t_req, "REQ")) {
      break;
    }
    // The phase bits are valid once REQ is up; a change ends the transfer.
    if (sci::csb_to_phase(r(sci::R_CSB)) != phase) break;

    // In MESSAGE OUT the initiator drops ATN on the last byte, after REQ has
    // been asserted but before it raises ACK.  That is how the target knows
    // the message is finished.
    uint8_t keep = 0;
    if (phase == sci::PH_MSG_OUT && (i + 1) < n) keep = sci::ICR_ATN;

    if (is_in) {
      in[i] = r(sci::R_CSD);
      w(sci::R_ICR, uint8_t(keep | sci::ICR_ACK));
    } else {
      w(sci::R_ODR, out[i]);
      w(sci::R_ICR, uint8_t(keep | sci::ICR_DATA));
      w(sci::R_ICR, uint8_t(keep | sci::ICR_DATA | sci::ICR_ACK));
    }

    if (!poll(sci::R_CSB, sci::CSB_REQ, 0, t_req, "REQ to be released")) {
      i++;
      break;
    }
    w(sci::R_ICR, keep);
  }
  return i;
}

SciDriver::Result SciDriver::execute(uint8_t target, const Bytes& cdb,
                                     const Bytes& data_out, size_t max_in,
                                     uint8_t lun) {
  Result res;
  err_.clear();

  if (!select(target)) return res;

  size_t out_pos = 0;

  // The phase loop, as NCR5380_information_transfer has it: look at what the
  // target is asking for and give it that, until the bus goes free.  The
  // bound is a runaway guard, not a protocol limit.
  for (int guard = 0; guard < 4096; guard++) {
    if (!poll(sci::R_CSB, sci::CSB_REQ, sci::CSB_REQ, t_req, "a phase")) {
      // No REQ.  If BSY has gone too, the target finished and let go.
      if (!(r(sci::R_CSB) & sci::CSB_BSY)) break;
      err_ = "the target stopped asking but kept the bus";
      return res;
    }

    uint8_t csb = r(sci::R_CSB);
    if (!(csb & sci::CSB_BSY)) break;

    switch (sci::csb_to_phase(csb)) {
      case sci::PH_MSG_OUT: {
        // IDENTIFY, with disconnection not permitted: nothing on this bus
        // reselects, and saying so keeps the nexus simple.
        uint8_t msg = uint8_t(0x80 | (lun & 7));
        if (pio(sci::PH_MSG_OUT, &msg, nullptr, 1) != 1) {
          err_ = "IDENTIFY was not accepted";
          return res;
        }
        break;
      }

      case sci::PH_COMMAND: {
        size_t sent = pio(sci::PH_COMMAND, cdb.data(), nullptr, cdb.size());
        if (sent != cdb.size()) {
          err_ = "the target took only " + std::to_string(sent) + " of " +
                 std::to_string(cdb.size()) + " command bytes";
          return res;
        }
        break;
      }

      case sci::PH_DATA_OUT: {
        size_t left = data_out.size() - out_pos;
        if (left == 0) {
          err_ = "the target asked for data the test did not supply";
          return res;
        }
        out_pos += pio(sci::PH_DATA_OUT, data_out.data() + out_pos, nullptr,
                       left);
        break;
      }

      case sci::PH_DATA_IN: {
        if (max_in == 0) {
          err_ = "the target offered data the test did not expect";
          return res;
        }
        size_t left = max_in - res.data.size();
        if (left == 0) {
          err_ = "the target offered more data than the test allowed for";
          return res;
        }
        Bytes chunk(left);
        size_t got = pio(sci::PH_DATA_IN, nullptr, chunk.data(), left);
        res.data.insert(res.data.end(), chunk.begin(), chunk.begin() + got);
        break;
      }

      case sci::PH_STATUS: {
        uint8_t st = 0;
        if (pio(sci::PH_STATUS, nullptr, &st, 1) != 1) {
          err_ = "no status byte";
          return res;
        }
        res.status = st;
        break;
      }

      case sci::PH_MSG_IN: {
        uint8_t msg = 0;
        if (pio(sci::PH_MSG_IN, nullptr, &msg, 1) != 1) {
          err_ = "no message in";
          return res;
        }
        res.message = msg;
        if (msg == 0x00) {  // COMMAND COMPLETE
          if (!poll(sci::R_CSB, sci::CSB_BSY, 0, t_req,
                    "the bus to go free after COMMAND COMPLETE")) {
            return res;
          }
          res.ok = true;
          return res;
        }
        break;
      }

      default:
        err_ = std::string("the target asked for ") +
               sci::phase_name(sci::csb_to_phase(csb));
        return res;
    }
  }

  err_ = "the command never finished";
  return res;
}

}  // namespace wtb

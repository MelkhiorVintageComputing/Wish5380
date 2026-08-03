// SPDX-License-Identifier: MIT

#include "ncr5380.h"

namespace wtb {
namespace sci {

namespace {

// Appends name when the bit is set, so a failure message reads as the set of
// signals rather than as a number the reader has to decode.
void add(std::string& s, uint8_t v, uint8_t bit, const char* name) {
  if (!(v & bit)) return;
  if (!s.empty()) s += '|';
  s += name;
}

std::string finish(std::string s) { return s.empty() ? std::string("0") : s; }

}  // namespace

const char* phase_name(uint8_t ph) {
  switch (ph & 7) {
    case PH_DATA_OUT: return "DATA OUT";
    case PH_DATA_IN: return "DATA IN";
    case PH_COMMAND: return "COMMAND";
    case PH_STATUS: return "STATUS";
    case PH_UNSPEC_1: return "unspecified(1)";
    case PH_UNSPEC_5: return "unspecified(5)";
    case PH_MSG_OUT: return "MESSAGE OUT";
    default: return "MESSAGE IN";
  }
}

// ICR is printed with its read-side names, because that is what a test that
// prints one has just read.
std::string icr_str(uint8_t v) {
  std::string s;
  add(s, v, ICR_RST, "RST");
  add(s, v, ICR_AIP, "AIP");
  add(s, v, ICR_LA, "LA");
  add(s, v, ICR_ACK, "ACK");
  add(s, v, ICR_BSY, "BSY");
  add(s, v, ICR_SEL, "SEL");
  add(s, v, ICR_ATN, "ATN");
  add(s, v, ICR_DATA, "DATA");
  return finish(std::move(s));
}

std::string mr_str(uint8_t v) {
  std::string s;
  add(s, v, MR_BLOCK_DMA, "BLOCK");
  add(s, v, MR_TARGET, "TARGET");
  add(s, v, MR_PAR_CHK, "PARCHK");
  add(s, v, MR_PAR_INTR, "PARINT");
  add(s, v, MR_EOP_INTR, "EOPINT");
  add(s, v, MR_MON_BSY, "MONBSY");
  add(s, v, MR_DMA, "DMA");
  add(s, v, MR_ARB, "ARB");
  return finish(std::move(s));
}

std::string csb_str(uint8_t v) {
  std::string s;
  add(s, v, CSB_RST, "RST");
  add(s, v, CSB_BSY, "BSY");
  add(s, v, CSB_REQ, "REQ");
  add(s, v, CSB_MSG, "MSG");
  add(s, v, CSB_CD, "C/D");
  add(s, v, CSB_IO, "I/O");
  add(s, v, CSB_SEL, "SEL");
  add(s, v, CSB_DBP, "DBP");
  s += " [";
  s += phase_name(csb_to_phase(v));
  s += ']';
  return s;
}

std::string bsr_str(uint8_t v) {
  std::string s;
  add(s, v, BSR_END_DMA, "ENDDMA");
  add(s, v, BSR_DRQ, "DRQ");
  add(s, v, BSR_PAR_ERR, "PARERR");
  add(s, v, BSR_IRQ, "IRQ");
  add(s, v, BSR_PHASE_MATCH, "PHASEMATCH");
  add(s, v, BSR_BUSY_ERR, "BUSYERR");
  add(s, v, BSR_ATN, "ATN");
  add(s, v, BSR_ACK, "ACK");
  return finish(std::move(s));
}

}  // namespace sci
}  // namespace wtb

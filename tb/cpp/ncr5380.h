// SPDX-License-Identifier: MIT
//
// The NCR 5380 register model, as the testbench understands it.
//
// This file is written from `doc/NCR5380_design_manual_Mar86.pdf` section 6
// and appendix A7, and from nothing else.  It is deliberately independent of
// `src/wish5380_pkg.sv`: the testbench is not allowed to derive its
// expectations from the RTL, or the two would quietly agree with each other
// and with no real chip.  `tb/cpp/tests/test_layout.cpp` is what pins them
// together, and to the two driver headers as well.
//
// Page references are to the printed page numbers of that manual, which are
// two less than the PDF page numbers.
//
// Naming follows the datasheet, not either driver.  Where the drivers use a
// different name for the same thing it is given in a comment, because a reader
// arriving from `NCR5380.c` or `ncr5380sbc.c` will be looking for theirs.

#pragma once

#include <cstdint>
#include <string>

namespace wtb {
namespace sci {

// ---------------------------------------------------------------------------
// The eight registers (p. 10).
//
// A2..A0 select one of eight addresses; read and write are different registers
// at five of them.  Three of the write side are strobes - the data written is
// meaningless and only the act of writing counts - and one of the read side is
// a strobe too, which is why a driver reading register 7 to acknowledge an
// interrupt must never be "optimised" into a peek.
// ---------------------------------------------------------------------------

enum Reg : uint8_t {
  R_CSD = 0,   // r   Current SCSI Data
  R_ODR = 0,   // w   Output Data
  R_ICR = 1,   // rw  Initiator Command
  R_MR = 2,    // rw  Mode
  R_TCR = 3,   // rw  Target Command
  R_CSB = 4,   // r   Current SCSI Bus Status
  R_SER = 4,   // w   Select Enable
  R_BSR = 5,   // r   Bus and Status
  R_SDS = 5,   // w   Start DMA Send            (strobe)
  R_IDR = 6,   // r   Input Data
  R_SDTR = 6,  // w   Start DMA Target Receive  (strobe)
  R_RPI = 7,   // r   Reset Parity/Interrupt    (strobe)
  R_SDIR = 7,  // w   Start DMA Initiator Receive (strobe)
  N_REGS = 8
};

// ---------------------------------------------------------------------------
// Register 1, Initiator Command (pp. 11-12).
//
// Read and write disagree on bits 6 and 5, which is the one place in the chip
// where a read/modify/write of a register does not give back what was put in.
// Both drivers know this: NetBSD's SCI_ICMD_RMASK is 0x1f and Linux keeps a
// shadow copy in hostdata->..., because reading ICR to modify it would turn
// AIP into TEST MODE and LA into DIFF ENBL.
// ---------------------------------------------------------------------------

constexpr uint8_t ICR_RST = 0x80;   // rw  assert RST
constexpr uint8_t ICR_AIP = 0x40;   // r   arbitration in progress
constexpr uint8_t ICR_TEST = 0x40;  // w   test mode: float all output drivers
constexpr uint8_t ICR_LA = 0x20;    // r   lost arbitration
constexpr uint8_t ICR_DIFF = 0x20;  // w   differential enable (NCR 5381 only)
constexpr uint8_t ICR_ACK = 0x10;   // rw  assert ACK
constexpr uint8_t ICR_BSY = 0x08;   // rw  assert BSY
constexpr uint8_t ICR_SEL = 0x04;   // rw  assert SEL
constexpr uint8_t ICR_ATN = 0x02;   // rw  assert ATN
constexpr uint8_t ICR_DATA = 0x01;  // rw  assert data bus

// The bits that read back as what was written.  A read/modify/write outside
// this mask is a bug in the driver, not in the chip.
constexpr uint8_t ICR_RW_MASK = 0x9f;
// What the drivers actually preserve: the same, less RST, because leaving RST
// set by accident resets the bus.
constexpr uint8_t ICR_RMASK = 0x1f;

// ---------------------------------------------------------------------------
// Register 2, Mode (p. 13).
// ---------------------------------------------------------------------------

constexpr uint8_t MR_BLOCK_DMA = 0x80;  // block mode DMA handshake
constexpr uint8_t MR_TARGET = 0x40;     // target role (initiator when clear)
constexpr uint8_t MR_PAR_CHK = 0x20;    // enable parity checking
constexpr uint8_t MR_PAR_INTR = 0x10;   // interrupt on a parity error
constexpr uint8_t MR_EOP_INTR = 0x08;   // interrupt on EOP during DMA
constexpr uint8_t MR_MON_BSY = 0x04;    // interrupt on unexpected loss of BSY
constexpr uint8_t MR_DMA = 0x02;        // DMA mode
constexpr uint8_t MR_ARB = 0x01;        // start arbitration

// ---------------------------------------------------------------------------
// Register 3, Target Command (p. 14).
//
// Bits 6..4 are unimplemented and read as zero (appendix A7 draws them as
// three zeroes on the read side and three don't-cares on the write side).
// Bit 7 is the 53C80's LAST BYTE SENT and reads as zero on the NMOS 5380.
// ---------------------------------------------------------------------------

constexpr uint8_t TCR_LAST_BYTE = 0x80;  // r  53C80 only: last byte sent
constexpr uint8_t TCR_REQ = 0x08;        // rw assert REQ
constexpr uint8_t TCR_MSG = 0x04;        // rw assert MSG
constexpr uint8_t TCR_CD = 0x02;         // rw assert C/D
constexpr uint8_t TCR_IO = 0x01;         // rw assert I/O
constexpr uint8_t TCR_PHASE_MASK = 0x07;

// ---------------------------------------------------------------------------
// Register 4 read, Current SCSI Bus Status (p. 15).
//
// A one means the signal is asserted on the bus, whoever is driving it,
// including this chip itself.  The bus is active low; the register is not.
// ---------------------------------------------------------------------------

constexpr uint8_t CSB_RST = 0x80;
constexpr uint8_t CSB_BSY = 0x40;
constexpr uint8_t CSB_REQ = 0x20;
constexpr uint8_t CSB_MSG = 0x10;
constexpr uint8_t CSB_CD = 0x08;
constexpr uint8_t CSB_IO = 0x04;
constexpr uint8_t CSB_SEL = 0x02;
constexpr uint8_t CSB_DBP = 0x01;
constexpr uint8_t CSB_PHASE_MASK = CSB_MSG | CSB_CD | CSB_IO;  // 0x1c

// ---------------------------------------------------------------------------
// Register 5 read, Bus and Status (pp. 15-16).
// ---------------------------------------------------------------------------

constexpr uint8_t BSR_END_DMA = 0x80;   // end of DMA transfer
constexpr uint8_t BSR_DRQ = 0x40;       // mirror of the DRQ pin
constexpr uint8_t BSR_PAR_ERR = 0x20;   // parity error latch
constexpr uint8_t BSR_IRQ = 0x10;       // mirror of the IRQ pin
constexpr uint8_t BSR_PHASE_MATCH = 0x08;
constexpr uint8_t BSR_BUSY_ERR = 0x04;  // unexpected loss of BSY
constexpr uint8_t BSR_ATN = 0x02;       // ATN as seen on the bus
constexpr uint8_t BSR_ACK = 0x01;       // ACK as seen on the bus

// The three that reading register 7 clears (p. 17).  Note that END OF DMA is
// not among them: it is cleared by resetting MR_DMA instead.
constexpr uint8_t BSR_RPI_CLEARS = BSR_PAR_ERR | BSR_IRQ | BSR_BUSY_ERR;

// ---------------------------------------------------------------------------
// SCSI information transfer phases.
//
// The datasheet tabulates these as the three ASSERT bits of the Target Command
// Register (p. 14), so that is the encoding used here: bit 2 MSG, bit 1 C/D,
// bit 0 I/O.  The Current SCSI Bus Status Register carries the same three bits
// two positions higher up, which is why Linux writes PHASE_SR_TO_TCR(p) as
// `p >> 2` and NetBSD writes SCI_BUS_PHASE(x) as `(x >> 2) & 7`.
//
// Two of the eight are marked Unspecified in the table and are not used by any
// SCSI-1 device; they are named here so a test can drive one deliberately.
// ---------------------------------------------------------------------------

enum Phase : uint8_t {
  PH_DATA_OUT = 0,     // I/O=0 C/D=0 MSG=0
  PH_UNSPEC_1 = 4,     //           MSG=1
  PH_COMMAND = 2,      //      C/D=1
  PH_MSG_OUT = 6,      //      C/D=1 MSG=1
  PH_DATA_IN = 1,      // I/O=1
  PH_UNSPEC_5 = 5,     // I/O=1       MSG=1
  PH_STATUS = 3,       // I/O=1 C/D=1
  PH_MSG_IN = 7,       // I/O=1 C/D=1 MSG=1
};

// The same phase as it appears in the Current SCSI Bus Status Register.
constexpr uint8_t phase_to_csb(uint8_t ph) { return uint8_t(ph << 2); }
constexpr uint8_t csb_to_phase(uint8_t csb) { return uint8_t((csb >> 2) & 7); }

const char* phase_name(uint8_t ph);
std::string icr_str(uint8_t v);
std::string mr_str(uint8_t v);
std::string csb_str(uint8_t v);
std::string bsr_str(uint8_t v);

// ---------------------------------------------------------------------------
// Timings the part defines, in picoseconds.
//
// The 5380 is a clockless device (p. 18): these come out of gate delays in the
// silicon and so vary from part to part.  The datasheet gives the numbers it
// guarantees, and those are what the RTL counts out and what the testbench
// checks it against.  Arbitration delay is the exception - the chip does not
// implement it, the driver does - and it is here because the driver model
// needs it.
// ---------------------------------------------------------------------------

constexpr uint64_t T_BUS_FREE_PS = 400'000;    // BSY inactive this long => free
constexpr uint64_t T_BUS_SETTLE_PS = 400'000;  // p. 19, before SEL is believed
constexpr uint64_t T_BUS_CLEAR_PS = 800'000;   // p. 21, releasing after RST
constexpr uint64_t T_ARB_DELAY_PS = 2'200'000; // p. 18, the driver's to keep
constexpr uint64_t T_RESET_MIN_PS = 200'000;   // p. 23, chip reset pulse width
constexpr uint64_t T_EOP_MIN_PS = 100'000;     // p. 25, EOP recognition width

}  // namespace sci
}  // namespace wtb

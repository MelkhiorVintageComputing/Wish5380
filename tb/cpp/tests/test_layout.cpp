// SPDX-License-Identifier: MIT
//
// Our register model against the two independent ones.
//
// `tb/cpp/ncr5380.h` is written from the datasheet.  So is `src/wish5380_pkg.sv`,
// separately.  Neither is checked by simply agreeing with the other, because
// two files copied from the same reading of a scan agree just as happily when
// the reading was wrong.
//
// So this test transcribes the three driver headers - NetBSD's
// `doc/drivers/NetBSD/ncr5380reg.h`, Linux's `doc/drivers/Linux/NCR5380.h` and
// Sun's `doc/drivers/SunOS34/sundev/sireg.h` - by hand, one more time, and
// checks all four against each other.  The drivers are the useful cross-check
// because they are the software that actually ran against the silicon: a wrong
// bit number in any of them would have stopped a real machine from booting in
// 1988.  Sun's is the one written by the people who put the part on a board,
// and it is the only one that writes both phase encodings out in full.
//
// The RTL is pinned separately, by `test_regs.cpp` driving the register file
// with the names from `ncr5380.h` and checking what comes back.  Between the
// two, a constant cannot move in one place only.
//
// This file exists before any engine, on purpose.  The sibling project
// Wish82586 wrote its command unit first and had to redo it when the layout
// turned out to be wrong.

#include "test.h"

using namespace wtb;

// ---------------------------------------------------------------------------
// Transcribed from doc/drivers/NetBSD/ncr5380reg.h.  Names and values are
// theirs; nothing here is derived from ncr5380.h.
// ---------------------------------------------------------------------------
namespace netbsd {

constexpr int sci_r0 = 0, sci_r1 = 1, sci_r2 = 2, sci_r3 = 3;
constexpr int sci_r4 = 4, sci_r5 = 5, sci_r6 = 6, sci_r7 = 7;

constexpr uint8_t SCI_ICMD_DATA = 0x01;
constexpr uint8_t SCI_ICMD_ATN = 0x02;
constexpr uint8_t SCI_ICMD_SEL = 0x04;
constexpr uint8_t SCI_ICMD_BSY = 0x08;
constexpr uint8_t SCI_ICMD_ACK = 0x10;
constexpr uint8_t SCI_ICMD_LST = 0x20;
constexpr uint8_t SCI_ICMD_DIFF = SCI_ICMD_LST;
constexpr uint8_t SCI_ICMD_AIP = 0x40;
constexpr uint8_t SCI_ICMD_TEST = SCI_ICMD_AIP;
constexpr uint8_t SCI_ICMD_RST = 0x80;
constexpr uint8_t SCI_ICMD_RMASK = 0x1F;

constexpr uint8_t SCI_MODE_ARB = 0x01;
constexpr uint8_t SCI_MODE_DMA = 0x02;
constexpr uint8_t SCI_MODE_MONBSY = 0x04;
constexpr uint8_t SCI_MODE_DMA_IE = 0x08;
constexpr uint8_t SCI_MODE_PERR_IE = 0x10;
constexpr uint8_t SCI_MODE_PAR_CHK = 0x20;
constexpr uint8_t SCI_MODE_TARGET = 0x40;
constexpr uint8_t SCI_MODE_BLOCKDMA = 0x80;

constexpr uint8_t SCI_TCMD_IO = 0x01;
constexpr uint8_t SCI_TCMD_CD = 0x02;
constexpr uint8_t SCI_TCMD_MSG = 0x04;
constexpr uint8_t SCI_TCMD_PHASE_MASK = 0x07;
constexpr uint8_t SCI_TCMD_REQ = 0x08;
constexpr uint8_t SCI_TCMD_LAST_SENT = 0x80;

constexpr uint8_t SCI_BUS_DBP = 0x01;
constexpr uint8_t SCI_BUS_SEL = 0x02;
constexpr uint8_t SCI_BUS_IO = 0x04;
constexpr uint8_t SCI_BUS_CD = 0x08;
constexpr uint8_t SCI_BUS_MSG = 0x10;
constexpr uint8_t SCI_BUS_REQ = 0x20;
constexpr uint8_t SCI_BUS_BSY = 0x40;
constexpr uint8_t SCI_BUS_RST = 0x80;

constexpr uint8_t SCI_CSR_ACK = 0x01;
constexpr uint8_t SCI_CSR_ATN = 0x02;
constexpr uint8_t SCI_CSR_DISC = 0x04;
constexpr uint8_t SCI_CSR_PHASE_MATCH = 0x08;
constexpr uint8_t SCI_CSR_INT = 0x10;
constexpr uint8_t SCI_CSR_PERR = 0x20;
constexpr uint8_t SCI_CSR_DREQ = 0x40;
constexpr uint8_t SCI_CSR_DONE = 0x80;

// SCI_BUS_PHASE(x) as the header writes it.
constexpr uint8_t bus_phase(uint8_t x) { return uint8_t((x >> 2) & 7); }

}  // namespace netbsd

// ---------------------------------------------------------------------------
// Transcribed from doc/drivers/Linux/NCR5380.h.
// ---------------------------------------------------------------------------
namespace linux_ {

constexpr int OUTPUT_DATA_REG = 0;
constexpr int CURRENT_SCSI_DATA_REG = 0;
constexpr int INITIATOR_COMMAND_REG = 1;
constexpr int MODE_REG = 2;
constexpr int TARGET_COMMAND_REG = 3;
constexpr int STATUS_REG = 4;
constexpr int SELECT_ENABLE_REG = 4;
constexpr int BUS_AND_STATUS_REG = 5;
constexpr int START_DMA_SEND_REG = 5;
constexpr int INPUT_DATA_REG = 6;
constexpr int START_DMA_TARGET_RECEIVE_REG = 6;
constexpr int RESET_PARITY_INTERRUPT_REG = 7;
constexpr int START_DMA_INITIATOR_RECEIVE_REG = 7;

constexpr uint8_t ICR_ASSERT_RST = 0x80;
constexpr uint8_t ICR_ARBITRATION_PROGRESS = 0x40;
constexpr uint8_t ICR_TRI_STATE = 0x40;
constexpr uint8_t ICR_ARBITRATION_LOST = 0x20;
constexpr uint8_t ICR_DIFF_ENABLE = 0x20;
constexpr uint8_t ICR_ASSERT_ACK = 0x10;
constexpr uint8_t ICR_ASSERT_BSY = 0x08;
constexpr uint8_t ICR_ASSERT_SEL = 0x04;
constexpr uint8_t ICR_ASSERT_ATN = 0x02;
constexpr uint8_t ICR_ASSERT_DATA = 0x01;

constexpr uint8_t MR_BLOCK_DMA_MODE = 0x80;
constexpr uint8_t MR_TARGET = 0x40;
constexpr uint8_t MR_ENABLE_PAR_CHECK = 0x20;
constexpr uint8_t MR_ENABLE_PAR_INTR = 0x10;
constexpr uint8_t MR_ENABLE_EOP_INTR = 0x08;
constexpr uint8_t MR_MONITOR_BSY = 0x04;
constexpr uint8_t MR_DMA_MODE = 0x02;
constexpr uint8_t MR_ARBITRATE = 0x01;

constexpr uint8_t TCR_LAST_BYTE_SENT = 0x80;
constexpr uint8_t TCR_ASSERT_REQ = 0x08;
constexpr uint8_t TCR_ASSERT_MSG = 0x04;
constexpr uint8_t TCR_ASSERT_CD = 0x02;
constexpr uint8_t TCR_ASSERT_IO = 0x01;

constexpr uint8_t SR_RST = 0x80;
constexpr uint8_t SR_BSY = 0x40;
constexpr uint8_t SR_REQ = 0x20;
constexpr uint8_t SR_MSG = 0x10;
constexpr uint8_t SR_CD = 0x08;
constexpr uint8_t SR_IO = 0x04;
constexpr uint8_t SR_SEL = 0x02;
constexpr uint8_t SR_DBP = 0x01;

constexpr uint8_t BASR_END_DMA_TRANSFER = 0x80;
constexpr uint8_t BASR_DRQ = 0x40;
constexpr uint8_t BASR_PARITY_ERROR = 0x20;
constexpr uint8_t BASR_IRQ = 0x10;
constexpr uint8_t BASR_PHASE_MATCH = 0x08;
constexpr uint8_t BASR_BUSY_ERROR = 0x04;
constexpr uint8_t BASR_ATN = 0x02;
constexpr uint8_t BASR_ACK = 0x01;

constexpr uint8_t PHASE_MASK = SR_MSG | SR_CD | SR_IO;
constexpr uint8_t PHASE_DATAOUT = 0;
constexpr uint8_t PHASE_DATAIN = SR_IO;
constexpr uint8_t PHASE_CMDOUT = SR_CD;
constexpr uint8_t PHASE_STATIN = SR_CD | SR_IO;
constexpr uint8_t PHASE_MSGOUT = SR_MSG | SR_CD;
constexpr uint8_t PHASE_MSGIN = SR_MSG | SR_CD | SR_IO;

constexpr uint8_t PHASE_SR_TO_TCR(uint8_t phase) { return uint8_t(phase >> 2); }

}  // namespace linux_

// ---------------------------------------------------------------------------
// Transcribed from doc/drivers/SunOS34/sundev/sireg.h - the oldest of the
// three, written in 1986 by the company that put the part on a board.  It
// names no register numbers: it declares two structs of eight `u_char`, one
// for reading and one for writing, and the addresses are the field order.
// The offsets below are that order counted out.
// ---------------------------------------------------------------------------
namespace sunos {

constexpr int sbc_cdr = 0, sbc_icr = 1, sbc_mr = 2, sbc_tcr = 3;
constexpr int sbc_cbsr = 4, sbc_bsr = 5, sbc_idr = 6, sbc_clr = 7;

constexpr int sbc_odr = 0, sbc_ser = 4, sbc_send = 5, sbc_trcv = 6;
constexpr int sbc_ircv = 7;

constexpr uint8_t SBC_ICR_RST = 0x80;
constexpr uint8_t SBC_ICR_AIP = 0x40;
constexpr uint8_t SBC_ICR_TEST = 0x40;
constexpr uint8_t SBC_ICR_LA = 0x20;
constexpr uint8_t SBC_ICR_DE = 0x20;
constexpr uint8_t SBC_ICR_ACK = 0x10;
constexpr uint8_t SBC_ICR_BUSY = 0x08;
constexpr uint8_t SBC_ICR_SEL = 0x04;
constexpr uint8_t SBC_ICR_ATN = 0x02;
constexpr uint8_t SBC_ICR_DATA = 0x01;

constexpr uint8_t SBC_MR_BDMA = 0x80;
constexpr uint8_t SBC_MR_TRG = 0x40;
constexpr uint8_t SBC_MR_EPC = 0x20;
constexpr uint8_t SBC_MR_EPI = 0x10;
constexpr uint8_t SBC_MR_EEI = 0x08;
constexpr uint8_t SBC_MR_MBSY = 0x04;
constexpr uint8_t SBC_MR_DMA = 0x02;
constexpr uint8_t SBC_MR_ARB = 0x01;

constexpr uint8_t SBC_TCR_REQ = 0x08;
constexpr uint8_t SBC_TCR_MSG = 0x04;
constexpr uint8_t SBC_TCR_CD = 0x02;
constexpr uint8_t SBC_TCR_IO = 0x01;

constexpr uint8_t TCR_COMMAND = SBC_TCR_CD;
constexpr uint8_t TCR_MSG_OUT = SBC_TCR_MSG | SBC_TCR_CD;
constexpr uint8_t TCR_DATA_OUT = 0;
constexpr uint8_t TCR_STATUS = SBC_TCR_CD | SBC_TCR_IO;
constexpr uint8_t TCR_MSG_IN = SBC_TCR_MSG | SBC_TCR_CD | SBC_TCR_IO;
constexpr uint8_t TCR_DATA_IN = SBC_TCR_IO;
constexpr uint8_t TCR_UNSPECIFIED = SBC_TCR_MSG;

constexpr uint8_t SBC_CBSR_RST = 0x80;
constexpr uint8_t SBC_CBSR_BSY = 0x40;
constexpr uint8_t SBC_CBSR_REQ = 0x20;
constexpr uint8_t SBC_CBSR_MSG = 0x10;
constexpr uint8_t SBC_CBSR_CD = 0x08;
constexpr uint8_t SBC_CBSR_IO = 0x04;
constexpr uint8_t SBC_CBSR_SEL = 0x02;
constexpr uint8_t SBC_CBSR_DBP = 0x01;

constexpr uint8_t CBSR_PHASE_BITS = SBC_CBSR_CD | SBC_CBSR_MSG | SBC_CBSR_IO;
constexpr uint8_t PHASE_COMMAND = SBC_CBSR_CD;
constexpr uint8_t PHASE_MSG_OUT = SBC_CBSR_MSG | SBC_CBSR_CD;
constexpr uint8_t PHASE_DATA_OUT = 0;
constexpr uint8_t PHASE_STATUS = SBC_CBSR_CD | SBC_CBSR_IO;
constexpr uint8_t PHASE_MSG_IN = SBC_CBSR_MSG | SBC_CBSR_CD | SBC_CBSR_IO;
constexpr uint8_t PHASE_DATA_IN = SBC_CBSR_IO;

constexpr uint8_t SBC_BSR_EDMA = 0x80;
constexpr uint8_t SBC_BSR_RDMA = 0x40;
constexpr uint8_t SBC_BSR_PERR = 0x20;
constexpr uint8_t SBC_BSR_INTR = 0x10;
constexpr uint8_t SBC_BSR_PMTCH = 0x08;
constexpr uint8_t SBC_BSR_BERR = 0x04;
constexpr uint8_t SBC_BSR_ATN = 0x02;
constexpr uint8_t SBC_BSR_ACK = 0x01;

}  // namespace sunos

// ---------------------------------------------------------------------------

TEST(layout_register_addresses) {
  (void)env;
  CHECK_EQ(int(sci::R_CSD), netbsd::sci_r0);
  CHECK_EQ(int(sci::R_ODR), netbsd::sci_r0);
  CHECK_EQ(int(sci::R_ICR), netbsd::sci_r1);
  CHECK_EQ(int(sci::R_MR), netbsd::sci_r2);
  CHECK_EQ(int(sci::R_TCR), netbsd::sci_r3);
  CHECK_EQ(int(sci::R_CSB), netbsd::sci_r4);
  CHECK_EQ(int(sci::R_SER), netbsd::sci_r4);
  CHECK_EQ(int(sci::R_BSR), netbsd::sci_r5);
  CHECK_EQ(int(sci::R_SDS), netbsd::sci_r5);
  CHECK_EQ(int(sci::R_IDR), netbsd::sci_r6);
  CHECK_EQ(int(sci::R_SDTR), netbsd::sci_r6);
  CHECK_EQ(int(sci::R_RPI), netbsd::sci_r7);
  CHECK_EQ(int(sci::R_SDIR), netbsd::sci_r7);

  CHECK_EQ(int(sci::R_CSD), linux_::CURRENT_SCSI_DATA_REG);
  CHECK_EQ(int(sci::R_ODR), linux_::OUTPUT_DATA_REG);
  CHECK_EQ(int(sci::R_ICR), linux_::INITIATOR_COMMAND_REG);
  CHECK_EQ(int(sci::R_MR), linux_::MODE_REG);
  CHECK_EQ(int(sci::R_TCR), linux_::TARGET_COMMAND_REG);
  CHECK_EQ(int(sci::R_CSB), linux_::STATUS_REG);
  CHECK_EQ(int(sci::R_SER), linux_::SELECT_ENABLE_REG);
  CHECK_EQ(int(sci::R_BSR), linux_::BUS_AND_STATUS_REG);
  CHECK_EQ(int(sci::R_SDS), linux_::START_DMA_SEND_REG);
  CHECK_EQ(int(sci::R_IDR), linux_::INPUT_DATA_REG);
  CHECK_EQ(int(sci::R_SDTR), linux_::START_DMA_TARGET_RECEIVE_REG);
  CHECK_EQ(int(sci::R_RPI), linux_::RESET_PARITY_INTERRUPT_REG);
  CHECK_EQ(int(sci::R_SDIR), linux_::START_DMA_INITIATOR_RECEIVE_REG);

  CHECK_EQ(int(sci::R_CSD), sunos::sbc_cdr);
  CHECK_EQ(int(sci::R_ODR), sunos::sbc_odr);
  CHECK_EQ(int(sci::R_ICR), sunos::sbc_icr);
  CHECK_EQ(int(sci::R_MR), sunos::sbc_mr);
  CHECK_EQ(int(sci::R_TCR), sunos::sbc_tcr);
  CHECK_EQ(int(sci::R_CSB), sunos::sbc_cbsr);
  CHECK_EQ(int(sci::R_SER), sunos::sbc_ser);
  CHECK_EQ(int(sci::R_BSR), sunos::sbc_bsr);
  CHECK_EQ(int(sci::R_SDS), sunos::sbc_send);
  CHECK_EQ(int(sci::R_IDR), sunos::sbc_idr);
  CHECK_EQ(int(sci::R_SDTR), sunos::sbc_trcv);
  CHECK_EQ(int(sci::R_RPI), sunos::sbc_clr);
  CHECK_EQ(int(sci::R_SDIR), sunos::sbc_ircv);
}

TEST(layout_initiator_command) {
  (void)env;
  CHECK_EQ(sci::ICR_RST, netbsd::SCI_ICMD_RST);
  CHECK_EQ(sci::ICR_AIP, netbsd::SCI_ICMD_AIP);
  CHECK_EQ(sci::ICR_TEST, netbsd::SCI_ICMD_TEST);
  CHECK_EQ(sci::ICR_LA, netbsd::SCI_ICMD_LST);
  CHECK_EQ(sci::ICR_DIFF, netbsd::SCI_ICMD_DIFF);
  CHECK_EQ(sci::ICR_ACK, netbsd::SCI_ICMD_ACK);
  CHECK_EQ(sci::ICR_BSY, netbsd::SCI_ICMD_BSY);
  CHECK_EQ(sci::ICR_SEL, netbsd::SCI_ICMD_SEL);
  CHECK_EQ(sci::ICR_ATN, netbsd::SCI_ICMD_ATN);
  CHECK_EQ(sci::ICR_DATA, netbsd::SCI_ICMD_DATA);
  CHECK_EQ(sci::ICR_RMASK, netbsd::SCI_ICMD_RMASK);

  CHECK_EQ(sci::ICR_RST, linux_::ICR_ASSERT_RST);
  CHECK_EQ(sci::ICR_AIP, linux_::ICR_ARBITRATION_PROGRESS);
  CHECK_EQ(sci::ICR_TEST, linux_::ICR_TRI_STATE);
  CHECK_EQ(sci::ICR_LA, linux_::ICR_ARBITRATION_LOST);
  CHECK_EQ(sci::ICR_DIFF, linux_::ICR_DIFF_ENABLE);
  CHECK_EQ(sci::ICR_ACK, linux_::ICR_ASSERT_ACK);
  CHECK_EQ(sci::ICR_BSY, linux_::ICR_ASSERT_BSY);
  CHECK_EQ(sci::ICR_SEL, linux_::ICR_ASSERT_SEL);
  CHECK_EQ(sci::ICR_ATN, linux_::ICR_ASSERT_ATN);
  CHECK_EQ(sci::ICR_DATA, linux_::ICR_ASSERT_DATA);

  CHECK_EQ(sci::ICR_RST, sunos::SBC_ICR_RST);
  CHECK_EQ(sci::ICR_AIP, sunos::SBC_ICR_AIP);
  CHECK_EQ(sci::ICR_TEST, sunos::SBC_ICR_TEST);
  CHECK_EQ(sci::ICR_LA, sunos::SBC_ICR_LA);
  CHECK_EQ(sci::ICR_DIFF, sunos::SBC_ICR_DE);
  CHECK_EQ(sci::ICR_ACK, sunos::SBC_ICR_ACK);
  CHECK_EQ(sci::ICR_BSY, sunos::SBC_ICR_BUSY);
  CHECK_EQ(sci::ICR_SEL, sunos::SBC_ICR_SEL);
  CHECK_EQ(sci::ICR_ATN, sunos::SBC_ICR_ATN);
  CHECK_EQ(sci::ICR_DATA, sunos::SBC_ICR_DATA);

  // The two positions where read and write are different registers.  If these
  // ever came out equal, the read/modify/write both drivers avoid would be
  // safe, and it is not.
  CHECK_EQ(sci::ICR_AIP, sci::ICR_TEST);
  CHECK_EQ(sci::ICR_LA, sci::ICR_DIFF);
  // ...and so those two bits are exactly what ICR_RW_MASK leaves out.
  CHECK_EQ(uint8_t(~sci::ICR_RW_MASK & 0xff), uint8_t(sci::ICR_AIP | sci::ICR_LA));
  // The mask the drivers preserve is the read/write one without RST.
  CHECK_EQ(sci::ICR_RMASK, uint8_t(sci::ICR_RW_MASK & ~sci::ICR_RST));
}

TEST(layout_mode) {
  (void)env;
  CHECK_EQ(sci::MR_BLOCK_DMA, netbsd::SCI_MODE_BLOCKDMA);
  CHECK_EQ(sci::MR_TARGET, netbsd::SCI_MODE_TARGET);
  CHECK_EQ(sci::MR_PAR_CHK, netbsd::SCI_MODE_PAR_CHK);
  CHECK_EQ(sci::MR_PAR_INTR, netbsd::SCI_MODE_PERR_IE);
  CHECK_EQ(sci::MR_EOP_INTR, netbsd::SCI_MODE_DMA_IE);
  CHECK_EQ(sci::MR_MON_BSY, netbsd::SCI_MODE_MONBSY);
  CHECK_EQ(sci::MR_DMA, netbsd::SCI_MODE_DMA);
  CHECK_EQ(sci::MR_ARB, netbsd::SCI_MODE_ARB);

  CHECK_EQ(sci::MR_BLOCK_DMA, linux_::MR_BLOCK_DMA_MODE);
  CHECK_EQ(sci::MR_TARGET, linux_::MR_TARGET);
  CHECK_EQ(sci::MR_PAR_CHK, linux_::MR_ENABLE_PAR_CHECK);
  CHECK_EQ(sci::MR_PAR_INTR, linux_::MR_ENABLE_PAR_INTR);
  CHECK_EQ(sci::MR_EOP_INTR, linux_::MR_ENABLE_EOP_INTR);
  CHECK_EQ(sci::MR_MON_BSY, linux_::MR_MONITOR_BSY);
  CHECK_EQ(sci::MR_DMA, linux_::MR_DMA_MODE);
  CHECK_EQ(sci::MR_ARB, linux_::MR_ARBITRATE);

  CHECK_EQ(sci::MR_BLOCK_DMA, sunos::SBC_MR_BDMA);
  CHECK_EQ(sci::MR_TARGET, sunos::SBC_MR_TRG);
  CHECK_EQ(sci::MR_PAR_CHK, sunos::SBC_MR_EPC);
  CHECK_EQ(sci::MR_PAR_INTR, sunos::SBC_MR_EPI);
  CHECK_EQ(sci::MR_EOP_INTR, sunos::SBC_MR_EEI);
  CHECK_EQ(sci::MR_MON_BSY, sunos::SBC_MR_MBSY);
  CHECK_EQ(sci::MR_DMA, sunos::SBC_MR_DMA);
  CHECK_EQ(sci::MR_ARB, sunos::SBC_MR_ARB);

  // Every bit of Mode is implemented; nothing reads back as zero.
  CHECK_EQ(uint8_t(sci::MR_BLOCK_DMA | sci::MR_TARGET | sci::MR_PAR_CHK |
                   sci::MR_PAR_INTR | sci::MR_EOP_INTR | sci::MR_MON_BSY |
                   sci::MR_DMA | sci::MR_ARB),
           uint8_t(0xff));
}

TEST(layout_target_command) {
  (void)env;
  CHECK_EQ(sci::TCR_LAST_BYTE, netbsd::SCI_TCMD_LAST_SENT);
  CHECK_EQ(sci::TCR_REQ, netbsd::SCI_TCMD_REQ);
  CHECK_EQ(sci::TCR_MSG, netbsd::SCI_TCMD_MSG);
  CHECK_EQ(sci::TCR_CD, netbsd::SCI_TCMD_CD);
  CHECK_EQ(sci::TCR_IO, netbsd::SCI_TCMD_IO);
  CHECK_EQ(sci::TCR_PHASE_MASK, netbsd::SCI_TCMD_PHASE_MASK);

  CHECK_EQ(sci::TCR_LAST_BYTE, linux_::TCR_LAST_BYTE_SENT);
  CHECK_EQ(sci::TCR_REQ, linux_::TCR_ASSERT_REQ);
  CHECK_EQ(sci::TCR_MSG, linux_::TCR_ASSERT_MSG);
  CHECK_EQ(sci::TCR_CD, linux_::TCR_ASSERT_CD);
  CHECK_EQ(sci::TCR_IO, linux_::TCR_ASSERT_IO);

  CHECK_EQ(sci::TCR_REQ, sunos::SBC_TCR_REQ);
  CHECK_EQ(sci::TCR_MSG, sunos::SBC_TCR_MSG);
  CHECK_EQ(sci::TCR_CD, sunos::SBC_TCR_CD);
  CHECK_EQ(sci::TCR_IO, sunos::SBC_TCR_IO);
  // Sun's header does not carry LAST BYTE SENT at all.  That bit is the
  // 53C80's (p. 54) and this header predates the CMOS part.

  // The phase lives in the bottom three bits; REQ sits just above them.
  CHECK_EQ(sci::TCR_PHASE_MASK,
           uint8_t(sci::TCR_MSG | sci::TCR_CD | sci::TCR_IO));
  CHECK_EQ(uint8_t(sci::TCR_PHASE_MASK & sci::TCR_REQ), uint8_t(0));
}

TEST(layout_current_scsi_bus_status) {
  (void)env;
  CHECK_EQ(sci::CSB_RST, netbsd::SCI_BUS_RST);
  CHECK_EQ(sci::CSB_BSY, netbsd::SCI_BUS_BSY);
  CHECK_EQ(sci::CSB_REQ, netbsd::SCI_BUS_REQ);
  CHECK_EQ(sci::CSB_MSG, netbsd::SCI_BUS_MSG);
  CHECK_EQ(sci::CSB_CD, netbsd::SCI_BUS_CD);
  CHECK_EQ(sci::CSB_IO, netbsd::SCI_BUS_IO);
  CHECK_EQ(sci::CSB_SEL, netbsd::SCI_BUS_SEL);
  CHECK_EQ(sci::CSB_DBP, netbsd::SCI_BUS_DBP);

  CHECK_EQ(sci::CSB_RST, linux_::SR_RST);
  CHECK_EQ(sci::CSB_BSY, linux_::SR_BSY);
  CHECK_EQ(sci::CSB_REQ, linux_::SR_REQ);
  CHECK_EQ(sci::CSB_MSG, linux_::SR_MSG);
  CHECK_EQ(sci::CSB_CD, linux_::SR_CD);
  CHECK_EQ(sci::CSB_IO, linux_::SR_IO);
  CHECK_EQ(sci::CSB_SEL, linux_::SR_SEL);
  CHECK_EQ(sci::CSB_DBP, linux_::SR_DBP);

  CHECK_EQ(sci::CSB_RST, sunos::SBC_CBSR_RST);
  CHECK_EQ(sci::CSB_BSY, sunos::SBC_CBSR_BSY);
  CHECK_EQ(sci::CSB_REQ, sunos::SBC_CBSR_REQ);
  CHECK_EQ(sci::CSB_MSG, sunos::SBC_CBSR_MSG);
  CHECK_EQ(sci::CSB_CD, sunos::SBC_CBSR_CD);
  CHECK_EQ(sci::CSB_IO, sunos::SBC_CBSR_IO);
  CHECK_EQ(sci::CSB_SEL, sunos::SBC_CBSR_SEL);
  CHECK_EQ(sci::CSB_DBP, sunos::SBC_CBSR_DBP);

  CHECK_EQ(sci::CSB_PHASE_MASK, linux_::PHASE_MASK);
  CHECK_EQ(sci::CSB_PHASE_MASK, sunos::CBSR_PHASE_BITS);
}

TEST(layout_bus_and_status) {
  (void)env;
  CHECK_EQ(sci::BSR_END_DMA, netbsd::SCI_CSR_DONE);
  CHECK_EQ(sci::BSR_DRQ, netbsd::SCI_CSR_DREQ);
  CHECK_EQ(sci::BSR_PAR_ERR, netbsd::SCI_CSR_PERR);
  CHECK_EQ(sci::BSR_IRQ, netbsd::SCI_CSR_INT);
  CHECK_EQ(sci::BSR_PHASE_MATCH, netbsd::SCI_CSR_PHASE_MATCH);
  // NetBSD calls the unexpected loss of BSY "disconnected", which is what it
  // means to an initiator whose target has gone away.
  CHECK_EQ(sci::BSR_BUSY_ERR, netbsd::SCI_CSR_DISC);
  CHECK_EQ(sci::BSR_ATN, netbsd::SCI_CSR_ATN);
  CHECK_EQ(sci::BSR_ACK, netbsd::SCI_CSR_ACK);

  CHECK_EQ(sci::BSR_END_DMA, linux_::BASR_END_DMA_TRANSFER);
  CHECK_EQ(sci::BSR_DRQ, linux_::BASR_DRQ);
  CHECK_EQ(sci::BSR_PAR_ERR, linux_::BASR_PARITY_ERROR);
  CHECK_EQ(sci::BSR_IRQ, linux_::BASR_IRQ);
  CHECK_EQ(sci::BSR_PHASE_MATCH, linux_::BASR_PHASE_MATCH);
  CHECK_EQ(sci::BSR_BUSY_ERR, linux_::BASR_BUSY_ERROR);
  CHECK_EQ(sci::BSR_ATN, linux_::BASR_ATN);
  CHECK_EQ(sci::BSR_ACK, linux_::BASR_ACK);

  CHECK_EQ(sci::BSR_END_DMA, sunos::SBC_BSR_EDMA);
  CHECK_EQ(sci::BSR_DRQ, sunos::SBC_BSR_RDMA);
  CHECK_EQ(sci::BSR_PAR_ERR, sunos::SBC_BSR_PERR);
  CHECK_EQ(sci::BSR_IRQ, sunos::SBC_BSR_INTR);
  CHECK_EQ(sci::BSR_PHASE_MATCH, sunos::SBC_BSR_PMTCH);
  CHECK_EQ(sci::BSR_BUSY_ERR, sunos::SBC_BSR_BERR);
  CHECK_EQ(sci::BSR_ATN, sunos::SBC_BSR_ATN);
  CHECK_EQ(sci::BSR_ACK, sunos::SBC_BSR_ACK);

  // Reading register 7 clears three bits and not the fourth: END OF DMA goes
  // away when MR_DMA is reset instead (p. 16), and a driver that expected the
  // acknowledge to clear it would spin.
  CHECK_EQ(sci::BSR_RPI_CLEARS,
           uint8_t(sci::BSR_PAR_ERR | sci::BSR_IRQ | sci::BSR_BUSY_ERR));
  CHECK_EQ(uint8_t(sci::BSR_RPI_CLEARS & sci::BSR_END_DMA), uint8_t(0));
}

TEST(layout_phases) {
  (void)env;
  // Our phase constants are in the Target Command Register's encoding.  Linux
  // states its phases in the Current SCSI Bus Status Register's and converts
  // with a shift of two; that shift is the whole relationship between the two
  // registers and is worth checking directly.
  CHECK_EQ(sci::PH_DATA_OUT, linux_::PHASE_SR_TO_TCR(linux_::PHASE_DATAOUT));
  CHECK_EQ(sci::PH_DATA_IN, linux_::PHASE_SR_TO_TCR(linux_::PHASE_DATAIN));
  CHECK_EQ(sci::PH_COMMAND, linux_::PHASE_SR_TO_TCR(linux_::PHASE_CMDOUT));
  CHECK_EQ(sci::PH_STATUS, linux_::PHASE_SR_TO_TCR(linux_::PHASE_STATIN));
  CHECK_EQ(sci::PH_MSG_OUT, linux_::PHASE_SR_TO_TCR(linux_::PHASE_MSGOUT));
  CHECK_EQ(sci::PH_MSG_IN, linux_::PHASE_SR_TO_TCR(linux_::PHASE_MSGIN));

  // Sun's header is the only one that writes both encodings out in full - the
  // Target Command values a driver writes, and the Current SCSI Bus Status
  // values it compares against - so it checks the shift of two from both ends
  // rather than assuming it.
  CHECK_EQ(sci::PH_DATA_OUT, sunos::TCR_DATA_OUT);
  CHECK_EQ(sci::PH_DATA_IN, sunos::TCR_DATA_IN);
  CHECK_EQ(sci::PH_COMMAND, sunos::TCR_COMMAND);
  CHECK_EQ(sci::PH_STATUS, sunos::TCR_STATUS);
  CHECK_EQ(sci::PH_MSG_OUT, sunos::TCR_MSG_OUT);
  CHECK_EQ(sci::PH_MSG_IN, sunos::TCR_MSG_IN);
  CHECK_EQ(sci::PH_UNSPEC_1, sunos::TCR_UNSPECIFIED);

  CHECK_EQ(sci::phase_to_csb(sci::PH_DATA_OUT), sunos::PHASE_DATA_OUT);
  CHECK_EQ(sci::phase_to_csb(sci::PH_DATA_IN), sunos::PHASE_DATA_IN);
  CHECK_EQ(sci::phase_to_csb(sci::PH_COMMAND), sunos::PHASE_COMMAND);
  CHECK_EQ(sci::phase_to_csb(sci::PH_STATUS), sunos::PHASE_STATUS);
  CHECK_EQ(sci::phase_to_csb(sci::PH_MSG_OUT), sunos::PHASE_MSG_OUT);
  CHECK_EQ(sci::phase_to_csb(sci::PH_MSG_IN), sunos::PHASE_MSG_IN);

  // NetBSD extracts the phase from the same register the same way.
  CHECK_EQ(sci::PH_COMMAND, netbsd::bus_phase(netbsd::SCI_BUS_CD));
  CHECK_EQ(sci::PH_MSG_IN, netbsd::bus_phase(uint8_t(
                               netbsd::SCI_BUS_MSG | netbsd::SCI_BUS_CD |
                               netbsd::SCI_BUS_IO)));

  // The datasheet's table (p. 14), read straight off: the three ASSERT bits
  // are I/O, C/D, MSG from the bottom up.
  CHECK_EQ(sci::PH_DATA_OUT, uint8_t(0));
  CHECK_EQ(sci::PH_DATA_IN, sci::TCR_IO);
  CHECK_EQ(sci::PH_COMMAND, sci::TCR_CD);
  CHECK_EQ(sci::PH_STATUS, uint8_t(sci::TCR_IO | sci::TCR_CD));
  CHECK_EQ(sci::PH_MSG_OUT, uint8_t(sci::TCR_CD | sci::TCR_MSG));
  CHECK_EQ(sci::PH_MSG_IN, uint8_t(sci::TCR_IO | sci::TCR_CD | sci::TCR_MSG));
  // The two the table calls Unspecified.
  CHECK_EQ(sci::PH_UNSPEC_1, sci::TCR_MSG);
  CHECK_EQ(sci::PH_UNSPEC_5, uint8_t(sci::TCR_IO | sci::TCR_MSG));

  // The two conversions are each other's inverse over all eight phases,
  // including the unspecified pair.
  for (uint8_t ph = 0; ph < 8; ph++) {
    CHECK_EQ(sci::csb_to_phase(sci::phase_to_csb(ph)), ph);
    CHECK_EQ(uint8_t(sci::phase_to_csb(ph) & ~sci::CSB_PHASE_MASK), uint8_t(0));
  }
}

// SPDX-License-Identifier: MIT
//
// The eight registers, driven with the names from `tb/cpp/ncr5380.h`.
//
// `test_layout.cpp` checks those names against the two driver headers.  This
// file checks the RTL against the names, so a constant cannot move in
// `src/wish5380_pkg.sv` alone without something going red.

#include "test.h"

using namespace wtb;

TEST(reg_reset_clears_the_storage_registers) {
  env.power_on_reset();
  CHECK_EQ(env.dut()->rg_odr_o, 0);
  CHECK_EQ(env.dut()->rg_icr_o, 0);
  CHECK_EQ(env.dut()->rg_mr_o, 0);
  CHECK_EQ(env.dut()->rg_tcr_o, 0);
  CHECK_EQ(env.dut()->rg_ser_o, 0);
  CHECK_EQ(env.reg_read(sci::R_MR), 0);
  CHECK_EQ(env.reg_read(sci::R_TCR), 0);
}

TEST(reg_mode_holds_every_bit) {
  env.power_on_reset();
  // Every bit of Mode is implemented, so the register is a plain byte.
  for (int i = 0; i < 8; i++) {
    uint8_t v = uint8_t(1u << i);
    env.reg_write(sci::R_MR, v);
    CHECK_EQ(env.reg_read(sci::R_MR), v);
    CHECK_EQ(env.dut()->rg_mr_o, v);
  }
  env.reg_write(sci::R_MR, 0xff);
  CHECK_EQ(env.reg_read(sci::R_MR), 0xff);
  env.reg_write(sci::R_MR, 0x00);
  CHECK_EQ(env.reg_read(sci::R_MR), 0x00);
}

TEST(reg_target_command_reads_back_four_bits) {
  env.power_on_reset();
  // Bits 7 and 6:4 are not implemented on the NMOS part and read as zero
  // (appendix A7).  Bit 7 is the 53C80's LAST BYTE SENT.
  env.reg_write(sci::R_TCR, 0xff);
  CHECK_EQ(env.reg_read(sci::R_TCR), 0x0f);
  CHECK_EQ(uint8_t(env.reg_read(sci::R_TCR) & sci::TCR_LAST_BYTE), 0);

  for (uint8_t ph = 0; ph < 8; ph++) {
    env.reg_write(sci::R_TCR, ph);
    CHECK_EQ(uint8_t(env.reg_read(sci::R_TCR) & sci::TCR_PHASE_MASK), ph);
  }
  env.reg_write(sci::R_TCR, sci::TCR_REQ | sci::PH_STATUS);
  CHECK_EQ(env.reg_read(sci::R_TCR), uint8_t(sci::TCR_REQ | sci::PH_STATUS));
}

TEST(reg_initiator_command_reads_arbitration_not_what_was_written) {
  env.power_on_reset();

  // TEST MODE and DIFF ENBL go in at bits 6 and 5; AIP and LA come back from
  // the arbitration logic at the same two positions (p. 12).  This is the one
  // place in the chip where read/modify/write loses what was there, and both
  // reference drivers work around it - NetBSD masks with 0x1f, Linux keeps a
  // shadow copy - so the fault has to be reproduced, not fixed.
  env.dut()->rg_aip_i = 0;
  env.dut()->rg_la_i = 0;
  env.reg_write(sci::R_ICR, uint8_t(sci::ICR_TEST | sci::ICR_DIFF));
  CHECK_EQ(uint8_t(env.reg_read(sci::R_ICR) & (sci::ICR_AIP | sci::ICR_LA)), 0);
  // ...but the engine still sees what was written there.
  CHECK_EQ(uint8_t(env.dut()->rg_icr_o & (sci::ICR_TEST | sci::ICR_DIFF)),
           uint8_t(sci::ICR_TEST | sci::ICR_DIFF));

  env.dut()->rg_aip_i = 1;
  env.dut()->rg_la_i = 1;
  env.reg_write(sci::R_ICR, 0x00);
  CHECK_EQ(uint8_t(env.reg_read(sci::R_ICR) & (sci::ICR_AIP | sci::ICR_LA)),
           uint8_t(sci::ICR_AIP | sci::ICR_LA));

  // The other six bits are ordinary storage and do read back.
  env.dut()->rg_aip_i = 0;
  env.dut()->rg_la_i = 0;
  for (uint8_t bit : {sci::ICR_RST, sci::ICR_ACK, sci::ICR_BSY, sci::ICR_SEL,
                      sci::ICR_ATN, sci::ICR_DATA}) {
    env.reg_write(sci::R_ICR, bit);
    CHECK_EQ(env.reg_read(sci::R_ICR), bit);
  }
  env.reg_write(sci::R_ICR, sci::ICR_RW_MASK);
  CHECK_EQ(env.reg_read(sci::R_ICR), sci::ICR_RW_MASK);
}

TEST(reg_windows_show_what_the_engine_drives) {
  env.power_on_reset();

  env.dut()->rg_csd_i = 0xa5;
  env.dut()->rg_csb_i = uint8_t(sci::CSB_BSY | sci::CSB_REQ |
                                sci::phase_to_csb(sci::PH_COMMAND));
  env.dut()->rg_idr_i = 0x5a;
  env.sim().eval();

  CHECK_EQ(env.reg_read(sci::R_CSD), 0xa5);
  CHECK_EQ(env.reg_read(sci::R_IDR), 0x5a);
  uint8_t csb = env.reg_read(sci::R_CSB);
  CHECK_EQ(csb, env.dut()->rg_csb_i);
  CHECK_EQ(sci::csb_to_phase(csb), uint8_t(sci::PH_COMMAND));

  // None of the three is storage: change what the engine drives and the same
  // read gives the new value without anything being written.
  env.dut()->rg_csd_i = 0x3c;
  env.sim().eval();
  CHECK_EQ(env.reg_read(sci::R_CSD), 0x3c);
}

TEST(reg_bus_and_status_assembles_its_eight_sources) {
  env.power_on_reset();

  struct Src {
    uint8_t* sig;
    uint8_t bit;
  } srcs[] = {
      {&env.dut()->rg_end_dma_i, sci::BSR_END_DMA},
      {&env.dut()->rg_drq_i, sci::BSR_DRQ},
      {&env.dut()->rg_par_err_i, sci::BSR_PAR_ERR},
      {&env.dut()->rg_irq_i, sci::BSR_IRQ},
      {&env.dut()->rg_phase_match_i, sci::BSR_PHASE_MATCH},
      {&env.dut()->rg_busy_err_i, sci::BSR_BUSY_ERR},
      {&env.dut()->rg_atn_i, sci::BSR_ATN},
      {&env.dut()->rg_ack_i, sci::BSR_ACK},
  };

  // One at a time, so a pair of crossed wires cannot cancel out.
  for (const Src& s : srcs) {
    *s.sig = 1;
    env.sim().eval();
    uint8_t got = env.reg_read(sci::R_BSR);
    CHECK_MSG(got == s.bit, "expected only " + sci::bsr_str(s.bit) +
                                ", read " + sci::bsr_str(got));
    *s.sig = 0;
    env.sim().eval();
  }

  for (const Src& s : srcs) *s.sig = 1;
  env.sim().eval();
  CHECK_EQ(env.reg_read(sci::R_BSR), 0xff);
}

TEST(reg_dack_reaches_the_data_registers_whatever_the_address) {
  env.power_on_reset();
  env.dut()->rg_idr_i = 0x77;
  env.sim().eval();

  // The DMA controller has no address lines to offer (p. 6), so an access
  // with DACK asserted must not decode the ones that happen to be sitting on
  // the pins.  Parking them on register 7 - the interrupt acknowledge - is
  // the case that would do visible damage if the decode leaked through.
  env.dut()->rg_adr_i = sci::R_RPI;
  env.sim().eval();
  CHECK_EQ(env.reg_read_dack(), 0x77);
  CHECK_EQ(env.last_strobes(), 0);

  env.dut()->rg_adr_i = sci::R_SDS;
  env.sim().eval();
  env.reg_write_dack(0xc3);
  CHECK_EQ(env.dut()->rg_odr_o, 0xc3);
  CHECK_EQ(env.last_strobes(), 0);
}

TEST(reg_strobes_fire_on_the_access_and_hold_nothing) {
  env.power_on_reset();

  env.reg_write(sci::R_SDS, 0xff);
  CHECK_EQ(env.last_strobes(), uint8_t(Env::S_SDS));
  env.reg_write(sci::R_SDTR, 0x00);
  CHECK_EQ(env.last_strobes(), uint8_t(Env::S_SDTR));
  env.reg_write(sci::R_SDIR, 0x55);
  CHECK_EQ(env.last_strobes(), uint8_t(Env::S_SDIR));
  (void)env.reg_read(sci::R_RPI);
  CHECK_EQ(env.last_strobes(), uint8_t(Env::S_RPI));

  // The data written to a start-DMA register is meaningless (p. 16), so
  // nothing may have kept it: none of the three is storage, and in
  // particular none of them is the Output Data Register in disguise.
  CHECK_EQ(env.dut()->rg_odr_o, 0);

  // Reading 5 or 6 is a status read and not a strobe; writing 4 or below is a
  // register write and not a strobe.
  (void)env.reg_read(sci::R_BSR);
  CHECK_EQ(env.last_strobes(), 0);
  (void)env.reg_read(sci::R_IDR);
  CHECK_EQ(env.last_strobes(), 0);
  env.reg_write(sci::R_SER, 0xaa);
  CHECK_EQ(env.last_strobes(), 0);
  CHECK_EQ(env.dut()->rg_ser_o, 0xaa);
}

TEST(reg_write_only_registers_do_not_read_back) {
  env.power_on_reset();

  // Register 0 write is Output Data and register 0 read is Current SCSI Data:
  // a different register at the same address.  Writing one must not be
  // visible in the other, which is what would happen if the two were
  // accidentally the same storage.
  env.dut()->rg_csd_i = 0x11;
  env.sim().eval();
  env.reg_write(sci::R_ODR, 0xee);
  CHECK_EQ(env.dut()->rg_odr_o, 0xee);
  CHECK_EQ(env.reg_read(sci::R_CSD), 0x11);

  // The same for register 4: Select Enable in, Current SCSI Bus Status out.
  env.dut()->rg_csb_i = 0x22;
  env.sim().eval();
  env.reg_write(sci::R_SER, 0xdd);
  CHECK_EQ(env.dut()->rg_ser_o, 0xdd);
  CHECK_EQ(env.reg_read(sci::R_CSB), 0x22);
}

TEST(reg_scsi_reset_clears_everything_but_assert_rst) {
  env.power_on_reset();

  env.reg_write(sci::R_MR, 0xff);
  env.reg_write(sci::R_TCR, 0x0f);
  env.reg_write(sci::R_SER, 0xaa);
  env.reg_write(sci::R_ODR, 0x55);
  env.reg_write(sci::R_ICR, uint8_t(sci::ICR_RST | sci::ICR_ATN | sci::ICR_ACK));

  // An SCSI bus reset clears the internal logic and registers "except for the
  // IRQ interrupt latch and the ASSERT RST bit" (p. 23).  A driver that has
  // just set ASSERT RST to pulse the bus must not have its own pulse switched
  // off underneath it.
  env.dut()->rg_sclr_i = 1;
  env.tick(1);
  env.dut()->rg_sclr_i = 0;
  env.tick(1);

  CHECK_EQ(env.dut()->rg_mr_o, 0);
  CHECK_EQ(env.dut()->rg_tcr_o, 0);
  CHECK_EQ(env.dut()->rg_ser_o, 0);
  CHECK_EQ(env.dut()->rg_odr_o, 0);
  CHECK_EQ(env.dut()->rg_icr_o, sci::ICR_RST);

  // ...and once the driver drops ASSERT RST, that goes too.
  env.reg_write(sci::R_ICR, 0);
  CHECK_EQ(env.dut()->rg_icr_o, 0);
}

// SPDX-License-Identifier: MIT
//
// The SCSI engine: what the chip drives, what it reports, the arbitration it
// does on its own, the handshake it automates in DMA mode, and the six things
// that make it interrupt.
//
// These drive the whole part through its register port and watch the bus,
// which is the only view a driver has.  The peer on the fabric stands in for
// whatever else is on the bus - a target answering a selection, or another
// initiator arbitrating.

#include "test.h"

using namespace wtb;

namespace {

// Brings the chip up the way a driver finds it and gives the peer a
// connection: BSY asserted, which most of the chip's behaviour is
// conditional on.
void connected(Env& env) {
  env.power_on_reset();
  Env::Peer p;
  p.bsy = true;
  env.drive_peer(p);
  env.tick(2);
}

}  // namespace

// ---------------------------------------------------------------------------
// What the chip drives
// ---------------------------------------------------------------------------

TEST(bus_reset_drives_nothing) {
  env.power_on_reset();
  CHECK_EQ(env.bus_csb(), 0);
  CHECK_EQ(env.bus_data(), 0);
  CHECK_EQ(env.chip_read(sci::R_CSB), 0);
}

TEST(bus_initiator_command_reaches_the_bus) {
  env.power_on_reset();

  env.chip_write(sci::R_ICR, sci::ICR_BSY);
  CHECK_EQ(env.bus_csb(), sci::CSB_BSY);
  env.chip_write(sci::R_ICR, sci::ICR_SEL);
  CHECK_EQ(env.bus_csb(), sci::CSB_SEL);

  // ATN and ACK are not in the Current SCSI Bus Status Register; they are
  // bits 1 and 0 of Bus and Status instead (p. 16).
  env.chip_write(sci::R_ICR, sci::ICR_ATN);
  CHECK_EQ(uint8_t(env.chip_read(sci::R_BSR) & sci::BSR_ATN), sci::BSR_ATN);
  env.chip_write(sci::R_ICR, sci::ICR_ACK);
  CHECK_EQ(uint8_t(env.chip_read(sci::R_BSR) & sci::BSR_ACK), sci::BSR_ACK);

  env.chip_write(sci::R_ICR, 0);
  CHECK_EQ(env.bus_csb(), 0);
}

TEST(bus_roles_decide_which_signals_may_be_asserted) {
  env.power_on_reset();

  // "In order for the signals ATN and ACK to be asserted on the SCSI bus, the
  // TARGETMODE bit must be reset (0).  In order for the signals C/D, I/O, MSG
  // and REQ to be asserted on the SCSI bus, the TARGETMODE bit must be set
  // (1)" (p. 13).
  env.chip_write(sci::R_TCR,
                 uint8_t(sci::TCR_REQ | sci::TCR_MSG | sci::TCR_CD | sci::TCR_IO));
  env.chip_write(sci::R_ICR, uint8_t(sci::ICR_ATN | sci::ICR_ACK));

  // As an initiator: the target's four signals stay off the bus, ATN and ACK
  // reach it.
  CHECK_EQ(env.bus_csb(), 0);
  uint8_t bsr = env.chip_read(sci::R_BSR);
  CHECK_EQ(uint8_t(bsr & (sci::BSR_ATN | sci::BSR_ACK)),
           uint8_t(sci::BSR_ATN | sci::BSR_ACK));

  // As a target: exactly the other way round.
  env.chip_write(sci::R_MR, sci::MR_TARGET);
  CHECK_EQ(env.bus_csb(), uint8_t(sci::CSB_REQ | sci::CSB_MSG | sci::CSB_CD |
                                  sci::CSB_IO));
  bsr = env.chip_read(sci::R_BSR);
  CHECK_EQ(uint8_t(bsr & (sci::BSR_ATN | sci::BSR_ACK)), 0);
}

TEST(bus_data_needs_assert_data_and_a_matching_phase) {
  env.power_on_reset();
  env.chip_write(sci::R_ODR, 0xa5);

  // Nothing yet: ASSERT DATA BUS is clear.
  CHECK_EQ(env.bus_data(), 0);

  // DATA OUT, which is what the bus reads as when nobody is driving the phase
  // lines, and the Target Command Register agrees.
  env.chip_write(sci::R_TCR, sci::PH_DATA_OUT);
  env.chip_write(sci::R_ICR, sci::ICR_DATA);
  CHECK_EQ(env.bus_data(), 0xa5);

  // The target asks for DATA IN instead.  The phase no longer matches and I/O
  // is true, either of which is enough to take the chip off the data lines
  // (p. 12).
  Env::Peer p;
  p.bsy = true;
  p.phase(sci::PH_DATA_IN);
  env.drive_peer(p);
  CHECK_EQ(env.bus_data(), 0);

  // Agreeing with it is still not enough: the datasheet requires I/O to be
  // false for an initiator to drive, and in DATA IN the target is the one
  // sourcing data.
  env.chip_write(sci::R_TCR, sci::PH_DATA_IN);
  CHECK(env.chip_read(sci::R_BSR) & sci::BSR_PHASE_MATCH);
  CHECK_EQ(env.bus_data(), 0);
}

TEST(bus_test_mode_removes_the_chip_from_the_bus) {
  env.power_on_reset();
  env.chip_write(sci::R_ODR, 0xff);
  env.chip_write(sci::R_TCR, sci::PH_DATA_OUT);
  env.chip_write(sci::R_ICR, uint8_t(sci::ICR_BSY | sci::ICR_SEL | sci::ICR_DATA));
  // The parity bit is on the bus with the data: 0xff has an even number of
  // ones, so odd parity sets it.
  CHECK_EQ(env.bus_csb(), uint8_t(sci::CSB_BSY | sci::CSB_SEL | sci::CSB_DBP));
  CHECK_EQ(env.bus_data(), 0xff);

  // "This bit may be written during a test environment to disable all output
  // drivers, effectively removing the NCR 5380 from the circuit" (p. 12).
  env.chip_write(sci::R_ICR,
                 uint8_t(sci::ICR_TEST | sci::ICR_BSY | sci::ICR_SEL | sci::ICR_DATA));
  CHECK_EQ(env.bus_csb(), 0);
  CHECK_EQ(env.bus_data(), 0);
}

TEST(bus_status_register_is_a_window_on_the_whole_bus) {
  env.power_on_reset();

  Env::Peer p;
  p.bsy = true;
  p.req = true;
  p.phase(sci::PH_COMMAND);
  p.with_data(0x3c);
  env.drive_peer(p);

  uint8_t csb = env.chip_read(sci::R_CSB);
  CHECK_EQ(sci::csb_to_phase(csb), uint8_t(sci::PH_COMMAND));
  CHECK(csb & sci::CSB_BSY);
  CHECK(csb & sci::CSB_REQ);
  CHECK_EQ(env.chip_read(sci::R_CSD), 0x3c);

  // And it shows this chip's own contribution too, which is what both drivers
  // rely on when they check that arbitration was won.
  env.chip_write(sci::R_ICR, sci::ICR_SEL);
  CHECK(env.chip_read(sci::R_CSB) & sci::CSB_SEL);
}

// ---------------------------------------------------------------------------
// Arbitration
// ---------------------------------------------------------------------------

TEST(bus_arbitration_waits_out_the_bus_free_filter) {
  env.power_on_reset();

  // Somebody else holds the bus.
  Env::Peer p;
  p.bsy = true;
  env.drive_peer(p);
  env.tick(4);

  // The sequence Linux's NCR5380_select uses: the ID into Output Data, then
  // MR_ARBITRATE, and Initiator Command is not touched at all.
  env.chip_write(sci::R_ODR, 0x80);
  env.chip_write(sci::R_MR, sci::MR_ARB);
  env.tick(30);
  CHECK_MSG(!(env.chip_read(sci::R_ICR) & sci::ICR_AIP),
            "arbitration started while BSY was still asserted");

  // "If BSY remains inactive for at least 400 nsec then the SCSI bus is
  // considered free and arbitration may begin" (p. 18).  AIP is watched
  // through the BSY the chip itself asserts, because reading a register would
  // cost the clocks being counted.
  env.peer_set(&Env::Peer::bsy, false);
  uint64_t want = env.ticks_for_ps(sci::T_BUS_FREE_PS);
  uint64_t at = 0;
  for (uint64_t i = 1; i <= want + 8; i++) {
    env.tick(1);
    if (env.bus_csb() & sci::CSB_BSY) {
      at = i;
      break;
    }
  }
  CHECK_MSG(at != 0, "arbitration never started");
  CHECK_MSG(at >= want, "arbitration started after " + std::to_string(at) +
                            " clocks, before the " + std::to_string(want) +
                            " the bus free filter needs");
  CHECK_MSG(at <= want + 4, "arbitration started late, after " +
                                std::to_string(at) + " clocks");

  // Asserted BSY and its ID, and says so (p. 12).
  CHECK(env.chip_read(sci::R_ICR) & sci::ICR_AIP);
  CHECK_EQ(env.bus_data(), 0x80);
  CHECK(env.chip_read(sci::R_CSB) & sci::CSB_BSY);
}

TEST(bus_arbitration_drives_the_id_without_assert_data_bus) {
  env.power_on_reset();
  env.chip_write(sci::R_ODR, 0x40);
  env.chip_write(sci::R_MR, sci::MR_ARB);
  env.tick(int(env.ticks_for_ps(sci::T_BUS_FREE_PS) + 4));

  // ASSERT DATA BUS was never written, and the ID is on the bus anyway: the
  // chip asserts it itself for the duration of arbitration (p. 12).  Linux
  // depends on this - it writes Initiator Command only after arbitration is
  // won.
  CHECK_EQ(uint8_t(env.chip_read(sci::R_ICR) & sci::ICR_DATA), 0);
  CHECK_EQ(env.bus_data(), 0x40);

  // AIP "will remain active until the ARBITRATE bit is reset".
  CHECK(env.chip_read(sci::R_ICR) & sci::ICR_AIP);
  env.chip_write(sci::R_MR, 0);
  CHECK_EQ(uint8_t(env.chip_read(sci::R_ICR) & sci::ICR_AIP), 0);
  CHECK_EQ(env.bus_data(), 0);
}

TEST(bus_arbitration_is_lost_only_to_another_devices_sel) {
  env.power_on_reset();
  env.chip_write(sci::R_ODR, 0x04);
  env.chip_write(sci::R_MR, sci::MR_ARB);
  env.tick(int(env.ticks_for_ps(sci::T_BUS_FREE_PS) + 4));
  CHECK(env.chip_read(sci::R_ICR) & sci::ICR_AIP);
  CHECK_EQ(uint8_t(env.chip_read(sci::R_ICR) & sci::ICR_LA), 0);

  // Our own SEL does not count.  A driver asserts it while still arbitrating:
  // Linux writes ICR_ASSERT_SEL | ICR_ASSERT_BSY and only afterwards clears
  // the Mode Register.
  env.chip_write(sci::R_ICR, uint8_t(sci::ICR_SEL | sci::ICR_BSY));
  env.tick(4);
  CHECK_MSG(!(env.chip_read(sci::R_ICR) & sci::ICR_LA),
            "the chip lost arbitration to its own SEL");
  env.chip_write(sci::R_ICR, 0);
  env.tick(2);

  // Somebody else's does (p. 12).
  env.peer_set(&Env::Peer::sel, true);
  env.tick(2);
  CHECK(env.chip_read(sci::R_ICR) & sci::ICR_LA);

  // Both bits go when ARBITRATE does.
  env.chip_write(sci::R_MR, 0);
  uint8_t icr = env.chip_read(sci::R_ICR);
  CHECK_EQ(uint8_t(icr & (sci::ICR_AIP | sci::ICR_LA)), 0);
}

// ---------------------------------------------------------------------------
// Phase match, and the interrupts
// ---------------------------------------------------------------------------

TEST(bus_phase_match_compares_the_bus_with_target_command) {
  env.power_on_reset();
  Env::Peer p;
  p.bsy = true;
  for (uint8_t ph = 0; ph < 8; ph++) {
    p.phase(ph);
    env.drive_peer(p);
    for (uint8_t tc = 0; tc < 8; tc++) {
      env.chip_write(sci::R_TCR, tc);
      bool match = (env.chip_read(sci::R_BSR) & sci::BSR_PHASE_MATCH) != 0;
      CHECK_MSG(match == (ph == tc),
                std::string("bus in ") + sci::phase_name(ph) +
                    ", Target Command says " + sci::phase_name(tc) +
                    ", PHASE MATCH " + (match ? "set" : "clear"));
    }
  }
}

TEST(bus_selection_interrupt_needs_an_id_match_and_a_settled_bus) {
  env.power_on_reset();

  // Our ID is bit 6.  Only a single bit match is required (p. 19).
  env.chip_write(sci::R_SER, 0x40);

  // Somebody arbitrates and then selects: BSY and SEL together, then BSY
  // drops.  Until it has been down for a bus settle delay there is nothing.
  Env::Peer p;
  p.bsy = true;
  p.sel = true;
  p.with_data(0x41);  // the selector's ID and ours
  env.drive_peer(p);
  env.tick(4);
  CHECK_EQ(uint8_t(env.chip_read(sci::R_BSR) & sci::BSR_IRQ), 0);

  env.peer_set(&Env::Peer::bsy, false);
  uint64_t want = env.ticks_for_ps(sci::T_BUS_SETTLE_PS);
  uint64_t at = 0;
  for (uint64_t i = 1; i <= want + 8; i++) {
    env.tick(1);
    if (env.dut()->dut_irq_o) {
      at = i;
      break;
    }
  }
  CHECK_MSG(at != 0, "no selection interrupt");
  CHECK_MSG(at >= want, "selection interrupt after only " +
                            std::to_string(at) + " clocks");

  CHECK(env.chip_read(sci::R_BSR) & sci::BSR_IRQ);
  // Reading register 7 acknowledges it (p. 19).
  (void)env.chip_read(sci::R_RPI);
  CHECK_EQ(env.dut()->dut_irq_o, 0);
}

TEST(bus_selection_interrupt_is_disabled_by_an_empty_select_enable) {
  env.power_on_reset();
  // "This interrupt may be disabled by writing zeros into all bits of the
  // Select Enable Register" (p. 19), which is exactly what NCR5380_select
  // does before it drops BSY, "otherwise we will trigger an interrupt".
  env.chip_write(sci::R_SER, 0x00);

  Env::Peer p;
  p.sel = true;
  p.with_data(0xff);
  env.drive_peer(p);
  env.tick(int(env.ticks_for_ps(sci::T_BUS_SETTLE_PS) + 8));
  CHECK_EQ(env.dut()->dut_irq_o, 0);
}

TEST(bus_scsi_reset_clears_the_registers_and_interrupts) {
  connected(env);

  env.chip_write(sci::R_MR, uint8_t(sci::MR_MON_BSY | sci::MR_PAR_CHK));
  env.chip_write(sci::R_TCR, 0x0f);
  env.chip_write(sci::R_SER, 0xaa);

  // "If the CPU sets the ASSERT RST bit ... an internal reset is performed.
  // Again, all internal logic and registers are cleared except for the IRQ
  // interrupt latch and the ASSERT RST bit" (p. 23).  The interrupt "cannot
  // be disabled".
  env.chip_write(sci::R_ICR, sci::ICR_RST);
  env.tick(3);

  CHECK(env.bus_csb() & sci::CSB_RST);
  CHECK_MSG(env.dut()->dut_irq_o, "an SCSI reset must interrupt");
  CHECK_EQ(env.chip_read(sci::R_MR), 0);
  CHECK_EQ(env.chip_read(sci::R_TCR), 0);
  CHECK_EQ(env.chip_read(sci::R_ICR), sci::ICR_RST);

  // The interrupt survived the clear, so the driver still learns why.  Then
  // the driver drops RST, which is what do_reset does after its 50 us.
  CHECK(env.chip_read(sci::R_BSR) & sci::BSR_IRQ);
  env.chip_write(sci::R_ICR, 0);
  env.tick(2);
  CHECK_EQ(uint8_t(env.bus_csb() & sci::CSB_RST), 0);
  (void)env.chip_read(sci::R_RPI);
  CHECK_EQ(env.dut()->dut_irq_o, 0);
}

TEST(bus_loss_of_busy_takes_the_bus_away) {
  connected(env);

  env.chip_write(sci::R_ICR, uint8_t(sci::ICR_ATN | sci::ICR_ACK));
  env.chip_write(sci::R_MR, uint8_t(sci::MR_MON_BSY | sci::MR_DMA));
  CHECK_EQ(env.chip_read(sci::R_MR), uint8_t(sci::MR_MON_BSY | sci::MR_DMA));

  env.peer_set(&Env::Peer::bsy, false);
  env.tick(int(env.ticks_for_ps(sci::T_BUS_SETTLE_PS) + 4));

  // "When the interrupt is generated due to loss of BSY, the lower 6 bits of
  // the Initiator Command Register are reset (0) and all signals are removed
  // from the SCSI bus" (p. 13), and "an unexpected loss of BSY will disable
  // any SCSI outputs and will reset the DMA MODE bit" (p. 16).
  CHECK(env.dut()->dut_irq_o);
  CHECK(env.chip_read(sci::R_BSR) & sci::BSR_BUSY_ERR);
  CHECK_EQ(uint8_t(env.chip_read(sci::R_ICR) & sci::ICR_RMASK), 0);
  CHECK_EQ(uint8_t(env.chip_read(sci::R_MR) & sci::MR_DMA), 0);
  CHECK_EQ(env.bus_csb(), 0);

  // BUSY ERROR is a *level*-sensitive latch: "set whenever the MONITOR BUSY
  // bit is true and BSY is false" (p. 16).  Reading register 7 empties it and
  // the condition immediately fills it again, so acknowledging alone achieves
  // nothing.
  (void)env.chip_read(sci::R_RPI);
  CHECK_MSG(env.chip_read(sci::R_BSR) & sci::BSR_BUSY_ERR,
            "the latch cleared while BSY was still false and MONITOR BUSY set");

  // Which is why NCR5380_intr writes the Mode Register before it reads
  // register 7: clearing MONITOR BUSY disarms the condition, and only then
  // does the acknowledge stick.
  env.chip_write(sci::R_MR, 0);
  (void)env.chip_read(sci::R_RPI);
  CHECK_EQ(uint8_t(env.chip_read(sci::R_BSR) & sci::BSR_BUSY_ERR), 0);
  CHECK_EQ(env.dut()->dut_irq_o, 0);
}

TEST(bus_phase_mismatch_interrupts_in_dma_mode) {
  connected(env);

  // Set up for DATA IN and let the target ask for COMMAND instead.
  env.chip_write(sci::R_TCR, sci::PH_DATA_IN);
  env.chip_write(sci::R_MR, sci::MR_DMA);
  env.chip_write(sci::R_SDIR, 0);
  CHECK_EQ(env.dut()->dut_irq_o, 0);

  Env::Peer p = env.peer();
  p.phase(sci::PH_COMMAND);
  env.drive_peer(p);
  env.tick(2);
  // Still nothing: the mismatch only interrupts when REQ goes true (p. 22).
  CHECK_EQ(env.dut()->dut_irq_o, 0);

  env.peer_set(&Env::Peer::req, true);
  env.tick(2);
  CHECK(env.dut()->dut_irq_o);
  CHECK_EQ(uint8_t(env.chip_read(sci::R_BSR) & sci::BSR_PHASE_MATCH), 0);
  // And no handshake happened: a mismatch "prevents the recognition of REQ".
  CHECK_MSG(!env.bus_ack(), "the chip acknowledged a mismatched phase");
}

// ---------------------------------------------------------------------------
// The DMA handshake
// ---------------------------------------------------------------------------

TEST(bus_dma_mode_needs_a_connected_bus) {
  env.power_on_reset();
  // "Note: BSY must be active in order to set the DMA Mode bit" (p. 14).
  env.chip_write(sci::R_MR, sci::MR_DMA);
  CHECK_MSG(!(env.chip_read(sci::R_MR) & sci::MR_DMA),
            "DMA mode was set with no target connected");

  Env::Peer p;
  p.bsy = true;
  env.drive_peer(p);
  env.tick(2);
  env.chip_write(sci::R_MR, sci::MR_DMA);
  CHECK(env.chip_read(sci::R_MR) & sci::MR_DMA);
}

TEST(bus_dma_initiator_receive_handshakes_each_byte) {
  connected(env);

  Env::Peer p = env.peer();
  p.phase(sci::PH_DATA_IN);
  env.drive_peer(p);

  env.chip_write(sci::R_TCR, sci::PH_DATA_IN);
  env.chip_write(sci::R_MR, sci::MR_DMA);
  env.chip_write(sci::R_SDIR, 0);
  env.tick(2);
  CHECK_MSG(!(env.chip_read(sci::R_BSR) & sci::BSR_DRQ),
            "DRQ before the target asked");

  const uint8_t bytes[] = {0x5a, 0xff, 0x00, 0x81};
  for (uint8_t want : bytes) {
    // The target puts the byte up and asserts REQ.
    p = env.peer();
    p.with_data(want);
    p.req = true;
    env.drive_peer(p);
    env.tick(2);

    CHECK_MSG(env.chip_read(sci::R_BSR) & sci::BSR_DRQ,
              "no DRQ after REQ went true");
    CHECK_MSG(env.dut()->dut_drq_o, "the DRQ pin disagrees with the register");
    CHECK_MSG(!env.bus_ack(), "ACK before the byte was taken");

    // The DMA takes it, and the chip acknowledges on the bus.
    CHECK_EQ(env.chip_read_dack(), want);
    env.tick(1);
    CHECK_MSG(env.bus_ack(), "no ACK after the byte was taken");
    CHECK_MSG(!(env.chip_read(sci::R_BSR) & sci::BSR_DRQ), "DRQ stayed up");

    // The target sees ACK and drops REQ; the chip drops ACK.
    env.peer_set(&Env::Peer::req, false);
    env.tick(2);
    CHECK_MSG(!env.bus_ack(), "ACK outlived REQ");
  }

  // The Input Data Register still holds the last byte, and reading it without
  // DACK gives the same answer.
  CHECK_EQ(env.chip_read(sci::R_IDR), 0x81);
}

TEST(bus_dma_initiator_send_handshakes_each_byte) {
  connected(env);
  // DATA OUT is phase zero, which is what the bus reads as with nobody
  // driving the phase lines.
  env.chip_write(sci::R_TCR, sci::PH_DATA_OUT);
  env.chip_write(sci::R_ICR, sci::ICR_DATA);
  env.chip_write(sci::R_MR, sci::MR_DMA);
  env.chip_write(sci::R_SDS, 0);
  env.tick(2);

  // Unlike receive, the chip asks for the first byte straight away: the
  // Output Data Register is its one byte of buffer and it fills it before it
  // has anywhere to send it (p. 20).
  CHECK_MSG(env.chip_read(sci::R_BSR) & sci::BSR_DRQ,
            "no DRQ after Start DMA Send");

  const uint8_t bytes[] = {0xc3, 0x01, 0xfe};
  for (uint8_t want : bytes) {
    env.chip_write_dack(want);
    env.tick(1);
    CHECK_MSG(!(env.chip_read(sci::R_BSR) & sci::BSR_DRQ),
              "DRQ stayed up after the byte was given");
    CHECK_MSG(!env.bus_ack(), "ACK before the target asked");

    // The target asks for it.
    env.peer_set(&Env::Peer::req, true);
    env.tick(2);
    CHECK_EQ(env.bus_data(), want);
    CHECK_MSG(env.bus_ack(), "no ACK after REQ went true");

    // ...takes it and drops REQ.
    env.peer_set(&Env::Peer::req, false);
    env.tick(2);
    CHECK_MSG(!env.bus_ack(), "ACK outlived REQ");
    CHECK_MSG(env.chip_read(sci::R_BSR) & sci::BSR_DRQ,
              "no DRQ for the next byte");
  }
}

TEST(bus_resetting_dma_mode_halts_the_transfer) {
  connected(env);
  Env::Peer p = env.peer();
  p.phase(sci::PH_DATA_IN);
  p.with_data(0x42);
  env.drive_peer(p);

  env.chip_write(sci::R_TCR, sci::PH_DATA_IN);
  env.chip_write(sci::R_MR, sci::MR_DMA);
  env.chip_write(sci::R_SDIR, 0);
  env.peer_set(&Env::Peer::req, true);
  env.tick(2);
  CHECK(env.chip_read(sci::R_BSR) & sci::BSR_DRQ);

  // "A DMA operation may be halted at any time simply by resetting the DMA
  // MODE bit" (p. 25), which is how both drivers end a transfer.
  env.chip_write(sci::R_MR, 0);
  env.tick(2);
  CHECK_EQ(uint8_t(env.chip_read(sci::R_BSR) & sci::BSR_DRQ), 0);
  CHECK_EQ(env.dut()->dut_drq_o, 0);
  CHECK_MSG(!env.bus_ack(), "ACK left asserted after the transfer was halted");
}

TEST(bus_end_of_dma_needs_eop_with_an_acknowledge_and_goes_with_dma_mode) {
  connected(env);
  env.chip_write(sci::R_TCR, sci::PH_DATA_OUT);
  env.chip_write(sci::R_ICR, sci::ICR_DATA);
  env.chip_write(sci::R_MR, uint8_t(sci::MR_DMA | sci::MR_EOP_INTR));
  env.chip_write(sci::R_SDS, 0);
  env.tick(2);

  // EOP on its own does nothing: it has to coincide with an acknowledge
  // (p. 16).
  env.dut()->dut_eop_i = 1;
  env.tick(4);
  CHECK_EQ(uint8_t(env.chip_read(sci::R_BSR) & sci::BSR_END_DMA), 0);

  env.chip_write_dack(0x77);
  env.tick(1);
  CHECK(env.chip_read(sci::R_BSR) & sci::BSR_END_DMA);
  CHECK_MSG(env.dut()->dut_irq_o, "ENABLE EOP INTERRUPT was set");
  env.dut()->dut_eop_i = 0;

  // Reading register 7 acknowledges the interrupt but leaves END OF DMA
  // standing: "This bit is reset when the DMA MODE bit is reset" (p. 16), and
  // a driver that expected the acknowledge to clear it would spin.
  (void)env.chip_read(sci::R_RPI);
  CHECK_EQ(env.dut()->dut_irq_o, 0);
  CHECK_MSG(env.chip_read(sci::R_BSR) & sci::BSR_END_DMA,
            "reading register 7 cleared END OF DMA");

  env.chip_write(sci::R_MR, 0);
  CHECK_EQ(uint8_t(env.chip_read(sci::R_BSR) & sci::BSR_END_DMA), 0);
}

// ---------------------------------------------------------------------------
// Parity
// ---------------------------------------------------------------------------

TEST(bus_parity_is_generated_odd) {
  env.power_on_reset();
  env.chip_write(sci::R_TCR, sci::PH_DATA_OUT);
  env.chip_write(sci::R_ICR, sci::ICR_DATA);

  // Odd parity: the bit is set when the byte has an even number of ones, so
  // that byte and bit together always have an odd number.
  const struct { uint8_t data; bool dbp; } cases[] = {
      {0x00, true}, {0x01, false}, {0x03, true}, {0xff, true}, {0x7f, false},
  };
  for (const auto& c : cases) {
    env.chip_write(sci::R_ODR, c.data);
    uint8_t csb = env.chip_read(sci::R_CSB);
    CHECK_MSG(((csb & sci::CSB_DBP) != 0) == c.dbp,
              "parity for 0x" + std::to_string(int(c.data)) + " is wrong");
  }
}

TEST(bus_parity_errors_are_latched_only_when_checking_is_enabled) {
  connected(env);

  Env::Peer p = env.peer();
  p.phase(sci::PH_DATA_IN);
  p.with_data(0x5a, /*good_parity=*/false);
  env.drive_peer(p);

  // With checking off, "parity will be ignored" (p. 13).
  env.chip_write(sci::R_MR, 0);
  (void)env.chip_read(sci::R_CSD);
  CHECK_EQ(uint8_t(env.chip_read(sci::R_BSR) & sci::BSR_PAR_ERR), 0);

  // With it on, a read of the Current SCSI Data Register is one of the two
  // places parity is checked (p. 21).
  env.chip_write(sci::R_MR, sci::MR_PAR_CHK);
  (void)env.chip_read(sci::R_CSD);
  CHECK(env.chip_read(sci::R_BSR) & sci::BSR_PAR_ERR);
  // ...but no interrupt, because ENABLE PARITY INTERRUPT is clear.  "A parity
  // error can be detected without generating an interrupt by disabling the
  // ENABLE PARITY INTERRUPT bit and checking the PARITY ERROR flag" (p. 21).
  CHECK_EQ(env.dut()->dut_irq_o, 0);

  (void)env.chip_read(sci::R_RPI);
  CHECK_EQ(uint8_t(env.chip_read(sci::R_BSR) & sci::BSR_PAR_ERR), 0);

  // Good parity leaves it alone.
  p.with_data(0x5a, true);
  env.drive_peer(p);
  (void)env.chip_read(sci::R_CSD);
  CHECK_EQ(uint8_t(env.chip_read(sci::R_BSR) & sci::BSR_PAR_ERR), 0);

  // And with both bits set it interrupts.
  p.with_data(0x5a, false);
  env.drive_peer(p);
  env.chip_write(sci::R_MR, uint8_t(sci::MR_PAR_CHK | sci::MR_PAR_INTR));
  (void)env.chip_read(sci::R_CSD);
  env.tick(1);
  CHECK(env.dut()->dut_irq_o);
}

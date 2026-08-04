// SPDX-License-Identifier: MIT
//
// The chip driven by a real DMA controller.
//
// Everything in `test_wb.cpp` reaches DMA mode through the Macintosh's
// pseudo-DMA aperture, which is the CPU pretending to be a DMA engine.  It
// cannot pretend all the way: a CPU moving bytes through a window has no End
// of Process pin to assert, so `EOP`, the END OF DMA status it sets and the
// interrupt that follows are unreachable that way.  Every one of those is on
// the path a Sun 3/60 takes, where an Am9516 UDC drives the handshake and
// asserts EOP on the last word.
//
// The engine here is the testbench's: it watches DRQ, issues one DACK cycle
// per request through the window that does not wait, and raises EOP with the
// last byte.  That is the whole of what a DMA controller looks like to a
// 5380 - the chip has no idea what is on the other side of DACK.

#include "test.h"

using namespace wtb;

namespace {

// A target with data to offer, and the chip set up to receive it by DMA.
void dma_receive(Env& env, uint8_t phase = sci::PH_DATA_IN) {
  env.power_on_reset();
  Env::Peer p;
  p.bsy = true;
  p.phase(phase);
  env.drive_peer(p);
  env.tick(2);

  env.chip_write(sci::R_TCR, phase);
  env.chip_write(sci::R_MR, uint8_t(sci::MR_DMA | sci::MR_EOP_INTR));
  env.chip_write(sci::R_SDIR, 0);
  env.tick(2);
}

// The peer answering a handshake, byte by byte, the way a target does.  The
// engine and the target take turns, so this drives the far side one step at a
// time between DMA accesses.
void offer(Env& env, uint8_t byte) {
  Env::Peer p = env.peer();
  p.with_data(byte);
  p.req = true;
  env.drive_peer(p);
  env.tick(3);
}

void withdraw(Env& env) {
  env.peer_set(&Env::Peer::req, false);
  env.tick(3);
}

}  // namespace

// ---------------------------------------------------------------------------
// End of Process, which pseudo-DMA cannot reach
// ---------------------------------------------------------------------------

TEST(dma_end_of_process_sets_end_of_dma_and_interrupts) {
  dma_receive(env);
  offer(env, 0x5a);

  // No EOP yet, so no END OF DMA and no interrupt however many bytes move.
  CHECK_EQ(uint8_t(env.chip_read(sci::R_BSR) & sci::BSR_END_DMA), 0);
  CHECK_EQ(env.dut()->dut_irq_o, 0);

  Env::Dma d = env.dma_in(1);
  CHECK_EQ(d.moved, size_t(1));
  CHECK_EQ(d.data[0], 0x5a);

  // "END OF DMA is set if EOP, DACK and either IOR or IOW are simultaneously
  // active" (p. 16), and ENABLE EOP INTERRUPT was set, so it interrupts.
  CHECK_MSG(env.chip_read(sci::R_BSR) & sci::BSR_END_DMA,
            "EOP with an acknowledge did not set END OF DMA");
  CHECK_MSG(env.dut()->dut_irq_o, "EOP did not interrupt");
}

TEST(dma_end_of_process_without_the_interrupt_enabled_is_silent) {
  dma_receive(env);
  // The same transfer with ENABLE EOP INTERRUPT clear: the status bit still
  // sets, and nothing interrupts.  A driver that polls END OF DMA rather than
  // taking an interrupt depends on exactly this.
  env.chip_write(sci::R_MR, sci::MR_DMA);
  offer(env, 0x11);
  Env::Dma d = env.dma_in(1);
  CHECK_EQ(d.moved, size_t(1));
  CHECK(env.chip_read(sci::R_BSR) & sci::BSR_END_DMA);
  CHECK_MSG(!env.dut()->dut_irq_o, "EOP interrupted with the enable clear");
}

TEST(dma_end_of_dma_survives_the_acknowledge_and_goes_with_dma_mode) {
  dma_receive(env);
  offer(env, 0x22);
  (void)env.dma_in(1);
  CHECK(env.chip_read(sci::R_BSR) & sci::BSR_END_DMA);

  // Reading register 7 acknowledges the interrupt and leaves END OF DMA
  // standing - it is not in the three bits that read clears (p. 17).  A
  // driver that expected otherwise would spin.
  (void)env.chip_read(sci::R_RPI);
  CHECK_EQ(env.dut()->dut_irq_o, 0);
  CHECK_MSG(env.chip_read(sci::R_BSR) & sci::BSR_END_DMA,
            "the acknowledge cleared END OF DMA");

  // Resetting DMA MODE is what clears it, which is what si_obio_dma_stop
  // does at the end of every transfer.
  env.chip_write(sci::R_MR, 0);
  CHECK_EQ(uint8_t(env.chip_read(sci::R_BSR) & sci::BSR_END_DMA), 0);
}

// ---------------------------------------------------------------------------
// A whole block, both directions
// ---------------------------------------------------------------------------

TEST(dma_receives_a_run_of_bytes_one_dack_per_request) {
  dma_receive(env);

  // The engine and the target take turns: the target offers, the engine
  // takes.  A chip that acknowledged twice, or asked for a byte before the
  // target had one, shows up as a byte out of place rather than as a hang.
  const uint8_t want[] = {0x00, 0xff, 0x5a, 0xa5, 0x01, 0x80, 0x7f, 0x3c};
  Bytes got;
  for (size_t i = 0; i < sizeof(want); i++) {
    offer(env, want[i]);
    Env::Dma d = env.dma_in(1, /*eop_on_last=*/false);
    CHECK_MSG(d.moved == 1, "byte " + std::to_string(i) + " never arrived");
    got.push_back(d.data[0]);
    withdraw(env);
  }
  CHECK_EQ(got, Bytes(want, want + sizeof(want)));
}

TEST(dma_sends_a_run_of_bytes_one_dack_per_request) {
  env.power_on_reset();
  Env::Peer p;
  p.bsy = true;
  env.drive_peer(p);          // DATA OUT is phase zero
  env.tick(2);

  env.chip_write(sci::R_TCR, sci::PH_DATA_OUT);
  env.chip_write(sci::R_ICR, sci::ICR_DATA);
  env.chip_write(sci::R_MR, sci::MR_DMA);
  env.chip_write(sci::R_SDS, 0);
  env.tick(2);

  const uint8_t send[] = {0xc3, 0x01, 0xfe, 0x00, 0xff};
  for (size_t i = 0; i < sizeof(send); i++) {
    Env::Dma d = env.dma_out(Bytes{send[i]}, /*eop_on_last=*/false);
    CHECK_MSG(d.moved == 1, "byte " + std::to_string(i) + " was not taken");

    // The target asks for it and sees it on the bus.
    env.peer_set(&Env::Peer::req, true);
    env.tick(3);
    CHECK_MSG(env.bus_data() == send[i],
              "byte " + std::to_string(i) + " reached the bus as " +
                  std::to_string(int(env.bus_data())));
    CHECK(env.bus_ack());
    env.peer_set(&Env::Peer::req, false);
    env.tick(3);
  }
}

// ---------------------------------------------------------------------------
// What ends a transfer
// ---------------------------------------------------------------------------

TEST(dma_a_phase_change_stops_the_engine_being_asked) {
  dma_receive(env);
  offer(env, 0x42);
  Env::Dma d = env.dma_in(1, false);
  CHECK_EQ(d.moved, size_t(1));
  withdraw(env);

  // The target moves to STATUS.  A phase mismatch "prevents the recognition
  // of REQ" (p. 22), so no further DRQ ever comes and the engine simply stops
  // being asked - which is how a transfer shorter than the count ends.
  Env::Peer p = env.peer();
  p.phase(sci::PH_STATUS);
  p.req = true;
  env.drive_peer(p);
  env.tick(3);

  env.dma_drq_timeout_ps = 100 * US;
  Env::Dma more = env.dma_in(4, false);
  CHECK_MSG(more.moved == 0,
            "the chip acknowledged " + std::to_string(more.moved) +
                " bytes in a phase it was not set up for");
  CHECK(env.dut()->dut_irq_o);   // the phase mismatch interrupt
}

TEST(dma_resetting_dma_mode_stops_the_engine_being_asked) {
  dma_receive(env);
  offer(env, 0x99);
  CHECK(env.chip_read(sci::R_BSR) & sci::BSR_DRQ);

  // "A DMA operation may be halted at any time simply by resetting the DMA
  // MODE bit" (p. 25).  si_obio_dma_stop does this on every transfer, so a
  // chip that kept asking afterwards would have the next command's first
  // byte taken by the last command's engine.
  env.chip_write(sci::R_MR, 0);
  env.tick(2);
  CHECK_EQ(env.dut()->dut_drq_o, 0);

  env.dma_drq_timeout_ps = 100 * US;
  Env::Dma d = env.dma_in(1, false);
  CHECK_MSG(d.moved == 0, "the chip was still asking after DMA mode was reset");
}

// ---------------------------------------------------------------------------
// A whole command with its data phase on the engine
// ---------------------------------------------------------------------------

TEST(dma_a_block_reads_back_through_a_real_engine) {
  env.power_on_reset();
  env.disk().fill_pattern(19, 0xd11a);
  Bytes want = env.disk().read_block(19);

  // Select the target and get as far as DATA IN by hand, then hand the data
  // phase to the engine - which is the shape of si_obio_dma_start: set the
  // chip up, start the UDC, and let the two of them move the block.
  CHECK_DRV(env.drv().select(env.cfg().target_id));

  uint8_t identify = 0x80;
  CHECK_EQ(env.drv().pio(sci::PH_MSG_OUT, &identify, nullptr, 1), size_t(1));
  Bytes cdb{0x08, 0, 0, 19, 1, 0};
  CHECK_EQ(env.drv().pio(sci::PH_COMMAND, cdb.data(), nullptr, cdb.size()),
           cdb.size());

  // Wait for the target to reach DATA IN, then set the chip up for DMA.
  CHECK(env.sim().run_until(
      [&]() {
        return (env.chip_read(sci::R_CSB) & sci::CSB_REQ) &&
               sci::csb_to_phase(env.chip_read(sci::R_CSB)) == sci::PH_DATA_IN;
      },
      5 * MS));

  env.chip_write(sci::R_TCR, sci::PH_DATA_IN);
  env.chip_write(sci::R_MR, uint8_t(sci::MR_DMA | sci::MR_EOP_INTR));
  env.chip_write(sci::R_ICR, 0);
  env.chip_write(sci::R_SDIR, 0);

  Env::Dma d = env.dma_in(512);
  CHECK_EQ(d.moved, size_t(512));
  CHECK_EQ(d.data, want);
  CHECK_MSG(env.chip_read(sci::R_BSR) & sci::BSR_END_DMA,
            "the engine's EOP did not reach the chip");

  // End it the way the driver does, and the command still finishes.
  env.chip_write(sci::R_MR, 0);
  env.chip_write(sci::R_ICR, 0);
  uint8_t st = 0xff, msg = 0xff;
  CHECK_EQ(env.drv().pio(sci::PH_STATUS, nullptr, &st, 1), size_t(1));
  CHECK_EQ(env.drv().pio(sci::PH_MSG_IN, nullptr, &msg, 1), size_t(1));
  CHECK_EQ(st, 0x00);
  CHECK_EQ(msg, 0x00);
}

TEST(dma_a_target_that_answers_short_ends_the_transfer_and_interrupts) {
  // SunOS asks a disk for 56 bytes of INQUIRY; a SCSI-1 disk answers with the
  // standard 36 and goes to STATUS.  The transfer therefore ends on a phase
  // mismatch with 20 bytes still outstanding, and not at terminal count -
  // which is the case NetBSD never produces, because it always asks for
  // exactly what it is going to get.  The chip has to interrupt here, or a
  // driver waits for something that never comes: SunOS's si0 gave up and
  // reset the bus.
  env.power_on_reset();

  CHECK_DRV(env.drv().select(env.cfg().target_id));
  uint8_t identify = 0x80;
  CHECK_EQ(env.drv().pio(sci::PH_MSG_OUT, &identify, nullptr, 1), size_t(1));
  Bytes cdb{0x12, 0, 0, 0, 56, 0};                 // INQUIRY, 56 bytes wanted
  CHECK_EQ(env.drv().pio(sci::PH_COMMAND, cdb.data(), nullptr, cdb.size()),
           cdb.size());

  CHECK(env.sim().run_until(
      [&]() {
        return (env.chip_read(sci::R_CSB) & sci::CSB_REQ) &&
               sci::csb_to_phase(env.chip_read(sci::R_CSB)) == sci::PH_DATA_IN;
      },
      5 * MS));

  env.chip_write(sci::R_TCR, sci::PH_DATA_IN);
  env.chip_write(sci::R_MR, sci::MR_DMA);
  env.chip_write(sci::R_ICR, 0);
  env.chip_write(sci::R_SDIR, 0);

  Env::Dma d = env.dma_in(56);
  CHECK_MSG(d.moved == 36,
            "the target gave " + std::to_string(d.moved) +
                " bytes of INQUIRY where 36 was expected");

  // The bus is in STATUS with REQ up and the engine has nothing left to do.
  CHECK_EQ(sci::csb_to_phase(env.chip_read(sci::R_CSB)), sci::PH_STATUS);
  uint8_t bsr = env.chip_read(sci::R_BSR);
  CHECK_MSG((bsr & sci::BSR_PHASE_MATCH) == 0,
            "the chip still thinks the phase matches");
  CHECK_MSG(env.dut()->dut_irq_o != 0,
            "the transfer ended on a phase mismatch and the chip did not "
            "interrupt: bsr 0x" + std::to_string(int(bsr)));
}

TEST(dma_a_block_writes_through_a_real_engine) {
  env.power_on_reset();
  Bytes payload = random_block(512, 0xd11b);

  CHECK_DRV(env.drv().select(env.cfg().target_id));
  uint8_t identify = 0x80;
  CHECK_EQ(env.drv().pio(sci::PH_MSG_OUT, &identify, nullptr, 1), size_t(1));
  Bytes cdb{0x0a, 0, 0, 23, 1, 0};
  CHECK_EQ(env.drv().pio(sci::PH_COMMAND, cdb.data(), nullptr, cdb.size()),
           cdb.size());

  CHECK(env.sim().run_until(
      [&]() {
        return (env.chip_read(sci::R_CSB) & sci::CSB_REQ) &&
               sci::csb_to_phase(env.chip_read(sci::R_CSB)) == sci::PH_DATA_OUT;
      },
      5 * MS));

  env.chip_write(sci::R_TCR, sci::PH_DATA_OUT);
  env.chip_write(sci::R_ICR, sci::ICR_DATA);
  env.chip_write(sci::R_MR, uint8_t(sci::MR_DMA | sci::MR_EOP_INTR));
  env.chip_write(sci::R_SDS, 0);

  Env::Dma d = env.dma_out(payload);
  CHECK_EQ(d.moved, size_t(512));

  env.chip_write(sci::R_MR, 0);
  env.chip_write(sci::R_ICR, 0);
  uint8_t st = 0xff, msg = 0xff;
  CHECK_EQ(env.drv().pio(sci::PH_STATUS, nullptr, &st, 1), size_t(1));
  CHECK_EQ(env.drv().pio(sci::PH_MSG_IN, nullptr, &msg, 1), size_t(1));
  CHECK_EQ(st, 0x00);
  CHECK_MSG(env.disk().read_block(23) == payload,
            "the block the engine sent is not what reached the media");
}

TEST(dma_and_programmed_io_agree) {
  env.power_on_reset();
  env.disk().fill_pattern(51, 0xd11c);

  SciDriver::Result slow = env.drv().execute(
      env.cfg().target_id, Bytes{0x08, 0, 0, 51, 1, 0}, Bytes(), 512);
  CHECK_DRV(slow.ok);

  // The same block again, by hand, over the engine.
  CHECK_DRV(env.drv().select(env.cfg().target_id));
  uint8_t identify = 0x80;
  (void)env.drv().pio(sci::PH_MSG_OUT, &identify, nullptr, 1);
  Bytes cdb{0x08, 0, 0, 51, 1, 0};
  (void)env.drv().pio(sci::PH_COMMAND, cdb.data(), nullptr, cdb.size());
  CHECK(env.sim().run_until(
      [&]() {
        return (env.chip_read(sci::R_CSB) & sci::CSB_REQ) &&
               sci::csb_to_phase(env.chip_read(sci::R_CSB)) == sci::PH_DATA_IN;
      },
      5 * MS));
  env.chip_write(sci::R_TCR, sci::PH_DATA_IN);
  env.chip_write(sci::R_MR, sci::MR_DMA);
  env.chip_write(sci::R_ICR, 0);
  env.chip_write(sci::R_SDIR, 0);
  Env::Dma fast = env.dma_in(512);

  // Three paths through the chip now - polled, pseudo-DMA and a real engine -
  // and they must not disagree about a single byte.
  CHECK_EQ(fast.data, slow.data);
  CHECK_EQ(fast.data, env.disk().read_block(51));
}

// ---------------------------------------------------------------------------
// The end of a transfer, without the End of Process interrupt
// ---------------------------------------------------------------------------
//
// A driver that does not enable ENABLE EOP INTERRUPT has only one thing left
// to tell it a DMA transfer is over: the interrupt the chip raises when the
// target changes phase.  The Sun 3/60 PROM is such a driver - it writes the
// Mode Register with DMA MODE alone and then waits for the board's SBC_IP
// bit, which is this chip's interrupt output - so if the phase mismatch does
// not interrupt, a real machine waits for ever.
//
// This is the same transfer as `dma_a_block_reads_back_through_a_real_engine`
// with that one bit taken out, which is the only difference that matters.

TEST(dma_a_transfer_ends_by_interrupting_without_the_eop_enable) {
  env.power_on_reset();
  env.disk().fill_pattern(23, 0xd11e);

  CHECK_DRV(env.drv().select(env.cfg().target_id));
  uint8_t identify = 0x80;
  CHECK_EQ(env.drv().pio(sci::PH_MSG_OUT, &identify, nullptr, 1), size_t(1));
  Bytes cdb{0x08, 0, 0, 23, 1, 0};
  CHECK_EQ(env.drv().pio(sci::PH_COMMAND, cdb.data(), nullptr, cdb.size()),
           cdb.size());
  CHECK(env.sim().run_until(
      [&]() {
        return (env.chip_read(sci::R_CSB) & sci::CSB_REQ) &&
               sci::csb_to_phase(env.chip_read(sci::R_CSB)) == sci::PH_DATA_IN;
      },
      5 * MS));

  env.chip_write(sci::R_TCR, sci::PH_DATA_IN);
  env.chip_write(sci::R_MR, sci::MR_DMA);      // and *not* MR_EOP_INTR
  env.chip_write(sci::R_ICR, 0);
  env.chip_write(sci::R_SDIR, 0);

  Env::Dma d = env.dma_in(512);
  CHECK_EQ(d.moved, size_t(512));

  // The target now leaves DATA IN for STATUS.  Nothing else has to be done to
  // the chip: the phase mismatch is what raises the interrupt (p. 22), and it
  // has no enable bit.
  CHECK_MSG(env.sim().run_until([&]() { return env.dut()->dut_irq_o != 0; },
                                20 * MS),
            "the transfer ended and the chip never interrupted");
}

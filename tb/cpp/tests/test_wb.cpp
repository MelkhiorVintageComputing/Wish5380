// SPDX-License-Identifier: MIT
//
// The machine glue: the three windows a Macintosh presents in front of the
// chip, and what each of them does with an access.
//
// Everything else in the suite reaches the chip through the register window
// already, so these are about the glue itself: where the registers sit, what
// a pseudo-DMA access moves, and the bus error the handshaking window raises
// when a byte does not arrive.

#include "test.h"

using namespace wtb;

namespace {

// The chip in DMA initiator receive, with the peer standing in for a target
// that has data to offer.  Everything about the pseudo-DMA windows needs this
// set up first.
void dma_receive(Env& env) {
  env.power_on_reset();
  Env::Peer p;
  p.bsy = true;
  p.phase(sci::PH_DATA_IN);
  env.drive_peer(p);
  env.tick(2);

  env.chip_write(sci::R_TCR, sci::PH_DATA_IN);
  env.chip_write(sci::R_MR, sci::MR_DMA);
  env.chip_write(sci::R_SDIR, 0);
  env.tick(2);
}

// One byte offered by the peer: data up, REQ up, and the chip latches it.
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
// The register window
// ---------------------------------------------------------------------------

TEST(wb_registers_are_sixteen_bytes_apart) {
  env.power_on_reset();
  // `#define NCR5380_read(reg) in_8(hostdata->io + ((reg) << 4))`, mac_scsi.c
  // line 38.  The Mode Register is register 2, so byte offset 0x20.
  // The pattern is the four Mode bits that only enable things.  DMA MODE is
  // refused with no target connected, MONITOR BUSY would interrupt on an
  // empty bus and ARBITRATE would start arbitrating; this test is about
  // where the register lives, not about any of that.
  env.host().write32(env.cfg().reg_base + 0x20, 0x000000b8, 0x1);
  CHECK_EQ(env.chip_read(sci::R_MR), 0xb8);

  // ...and the fifteen bytes after it are not the Mode Register.  A stride
  // that had collapsed to one would put the Target Command Register here.
  CHECK_MSG(env.wb_err_on_write(env.cfg().reg_base + 0x21, 0x2, 0x00003000),
            "a byte inside a register's stride was decoded as a register");
  CHECK_EQ(env.chip_read(sci::R_MR), 0xb8);
}

TEST(wb_each_of_the_eight_registers_has_its_own_address) {
  env.power_on_reset();
  // Written through the window at its own offset, read back through the
  // accessor: if two registers shared an address the second write would
  // disturb the first.
  //
  // The Mode Register is not a scratchpad - ARBITRATE starts arbitrating,
  // MONITOR BUSY interrupts on a bus with nothing on it, and DMA MODE is
  // refused outright - so the value here is the four bits that only enable
  // things: BLOCK MODE, parity checking, and the two interrupt enables.
  env.host().write32(env.cfg().reg_base + 2 * 16, 0x000000b8, 0x1);  // Mode
  env.host().write32(env.cfg().reg_base + 3 * 16, 0x0000000f, 0x1);  // Target
  env.host().write32(env.cfg().reg_base + 4 * 16, 0x00000055, 0x1);  // Sel Enb
  env.host().write32(env.cfg().reg_base + 1 * 16, 0x00000012, 0x1);  // Init Cmd
  CHECK_EQ(env.chip_read(sci::R_MR), 0xb8);
  CHECK_EQ(env.chip_read(sci::R_TCR), 0x0f);
  CHECK_EQ(env.chip_read(sci::R_ICR), 0x12);
}

TEST(wb_a_wide_access_to_the_register_window_is_a_bus_error) {
  env.power_on_reset();
  // A register is one byte and every driver reaches it with one.  What real
  // hardware would do with a `movew` there is a property of the board's
  // decoder rather than of the chip, so there is nothing to be faithful to,
  // and a fault says so where quietly serving one lane would not.
  CHECK(env.wb_err_on_read(env.cfg().reg_base, 0x3));
  CHECK(env.wb_err_on_read(env.cfg().reg_base, 0xf));
  CHECK(env.wb_err_on_write(env.cfg().reg_base + 0x20, 0x3, 0));

  // The chip was not touched by any of them.
  CHECK_EQ(env.chip_read(sci::R_MR), 0);
}

TEST(wb_an_address_in_no_window_is_a_bus_error) {
  env.power_on_reset();
  // Between the register window and the first pseudo-DMA one.
  CHECK(env.wb_err_on_read(0x80, 0x1));
  // Past the last one.
  CHECK(env.wb_err_on_read(0x400, 0x1));
  // Outside the slave altogether: the machine's own decode should never have
  // routed this here, so it is a fault rather than something to alias back
  // into the register window.
  CHECK(env.wb_err_on_read(0x10000, 0x1));
}

// ---------------------------------------------------------------------------
// The pseudo-DMA windows
// ---------------------------------------------------------------------------

TEST(wb_pseudo_dma_moves_one_scsi_byte_per_byte_lane) {
  dma_receive(env);
  offer(env, 0x5a);

  Env::Pdma p = env.pdma_read(1);
  CHECK(!p.error);
  CHECK_EQ(p.data.size(), size_t(1));
  CHECK_EQ(p.data[0], 0x5a);

  // Exactly one byte moved: the chip acknowledged once and is waiting for the
  // target to let go, not asking for another.
  CHECK(env.bus_ack());
  withdraw(env);
  CHECK(!env.bus_ack());
  CHECK_EQ(uint8_t(env.chip_read(sci::R_BSR) & sci::BSR_DRQ), 0);
}

TEST(wb_a_word_access_moves_two_scsi_bytes) {
  env.power_on_reset();
  // The Mac's fast path is `movew` on a fixed aperture address, so one bus
  // cycle is two REQ/ACK handshakes on the SCSI bus.  `MOVE_16_WORDS` looks
  // like a burst and is not - it is sixteen of these unrolled.
  //
  // A static peer cannot handshake twice inside one cycle, so this runs a
  // real READ against the target instead, which is what the Mac does anyway.
  env.disk().fill_pattern(9, 0x51ded);
  Bytes want = env.disk().read_block(9);

  env.drv().use_pdma = true;
  env.drv().pdma_width = 2;
  SciDriver::Result r = env.drv().execute(
      env.cfg().target_id, Bytes{0x08, 0, 0, 9, 1, 0}, Bytes(), 512);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, 0x00);
  CHECK_EQ(r.data.size(), size_t(512));
  CHECK_EQ(r.data, want);
}

TEST(wb_the_window_without_a_handshake_does_not_wait) {
  dma_receive(env);
  // No REQ, so no DRQ and no byte.  The window that does not handshake
  // acknowledges anyway - the driver using it has already satisfied itself
  // that a byte is ready, and on the machines it exists for the hardware
  // handshake is broken or absent.
  CHECK_EQ(uint8_t(env.chip_read(sci::R_BSR) & sci::BSR_DRQ), 0);
  Env::Pdma p = env.pdma_read(1, /*handshake=*/false);
  CHECK_MSG(!p.error, "the non-handshaking window waited for DRQ");
}

TEST(wb_the_handshaking_window_bus_errors_when_the_byte_never_comes) {
  dma_receive(env);
  // The target has gone quiet - a phase change, or a drive that stopped
  // answering.  "Mac OS requires disk drivers to specify the number of bytes
  // between the delays expected from a SCSI target.  This allows the
  // operating system to prevent bus errors when a target fails to deliver the
  // next byte within the processor bus error timeout period."  Linux cannot
  // know those timings, so it takes the bus errors and catches them with an
  // exception fixup table.
  u64 t0 = env.sim().time_ps();
  Env::Pdma p = env.pdma_read(1, /*handshake=*/true);
  u64 dt = env.sim().time_ps() - t0;

  CHECK_MSG(p.error, "the handshaking window answered without a byte");
  CHECK_MSG(dt >= 10 * US, "it gave up after only " +
                               std::to_string(dt / 1000) + " ns");

  // And the chip is undisturbed: the next byte still arrives normally.
  offer(env, 0xc3);
  Env::Pdma q = env.pdma_read(1);
  CHECK(!q.error);
  CHECK_EQ(q.data[0], 0xc3);
}

TEST(wb_pseudo_dma_send_gives_the_chip_a_byte) {
  env.power_on_reset();
  Env::Peer p;
  p.bsy = true;
  env.drive_peer(p);            // DATA OUT is phase zero: nothing to drive
  env.tick(2);

  env.chip_write(sci::R_TCR, sci::PH_DATA_OUT);
  env.chip_write(sci::R_ICR, sci::ICR_DATA);
  env.chip_write(sci::R_MR, sci::MR_DMA);
  env.chip_write(sci::R_SDS, 0);
  env.tick(2);
  CHECK(env.chip_read(sci::R_BSR) & sci::BSR_DRQ);

  Env::Pdma w = env.pdma_write(Bytes{0x77});
  CHECK(!w.error);

  // The byte is in the Output Data Register and the chip is waiting to be
  // asked for it.
  CHECK_EQ(uint8_t(env.chip_read(sci::R_BSR) & sci::BSR_DRQ), 0);
  CHECK(!env.bus_ack());
  env.peer_set(&Env::Peer::req, true);
  env.tick(3);
  CHECK_EQ(env.bus_data(), 0x77);
  CHECK(env.bus_ack());
}

// ---------------------------------------------------------------------------
// An access is an event
// ---------------------------------------------------------------------------

TEST(wb_one_bus_cycle_is_one_access_to_the_chip) {
  dma_receive(env);
  // Writing register 5, 6 or 7 starts a DMA transfer whatever is on the data
  // bus, and reading register 7 clears three status bits, so a bus cycle that
  // strobed the chip twice would do real damage.  A one-lane pseudo-DMA read
  // consuming exactly one byte is the sharpest way to see it: two strobes
  // would swallow the byte the next access is meant to get.
  offer(env, 0x11);
  Env::Pdma a = env.pdma_read(1);
  CHECK_EQ(a.data[0], 0x11);
  withdraw(env);

  offer(env, 0x22);
  Env::Pdma b = env.pdma_read(1);
  CHECK_MSG(b.data[0] == 0x22,
            "the second byte read back as " + std::to_string(int(b.data[0])));
  withdraw(env);

  offer(env, 0x33);
  Env::Pdma c = env.pdma_read(1);
  CHECK_EQ(c.data[0], 0x33);
}

TEST(wb_reading_the_interrupt_register_is_not_a_peek) {
  env.power_on_reset();
  // Reading register 7 is a strobe: it clears the interrupt latch.  A front
  // end that speculated a read - to have the data ready, say - would
  // acknowledge interrupts nobody asked about.
  env.chip_write(sci::R_SER, 0x40);
  Env::Peer p;
  p.sel = true;
  p.with_data(0x41);
  env.drive_peer(p);
  env.tick(int(env.ticks_for_ps(sci::T_BUS_SETTLE_PS) + 8));
  CHECK(env.dut()->dut_irq_o);

  // Reading anything else leaves it alone, however many times.
  for (int i = 0; i < 4; i++) {
    (void)env.chip_read(sci::R_CSB);
    (void)env.chip_read(sci::R_BSR);
    (void)env.chip_read(sci::R_ICR);
  }
  CHECK_MSG(env.dut()->dut_irq_o, "something acknowledged the interrupt");

  (void)env.chip_read(sci::R_RPI);
  CHECK_EQ(env.dut()->dut_irq_o, 0);
}

// ---------------------------------------------------------------------------
// The whole thing over pseudo-DMA
// ---------------------------------------------------------------------------

TEST(wb_a_block_reads_back_over_pseudo_dma) {
  env.power_on_reset();
  env.disk().fill_pattern(64, 0xfeed);
  Bytes want = env.disk().read_block(64);

  env.drv().use_pdma = true;
  SciDriver::Result r = env.drv().execute(
      env.cfg().target_id, Bytes{0x08, 0, 0, 64, 1, 0}, Bytes(), 512);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, 0x00);
  CHECK_EQ(r.data, want);
}

TEST(wb_a_block_writes_over_pseudo_dma) {
  env.power_on_reset();
  Bytes payload = random_block(512, 0xd15c);

  env.drv().use_pdma = true;
  SciDriver::Result r =
      env.drv().execute(env.cfg().target_id, Bytes{0x0a, 0, 0, 33, 1, 0}, payload);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, 0x00);
  CHECK_EQ(env.disk().read_block(33), payload);
}

TEST(wb_pseudo_dma_and_programmed_io_agree) {
  env.power_on_reset();
  env.disk().fill_pattern(128, 0x1010);

  env.drv().use_pdma = false;
  SciDriver::Result slow = env.drv().execute(
      env.cfg().target_id, Bytes{0x08, 0, 0, 128, 1, 0}, Bytes(), 512);
  CHECK_DRV(slow.ok);

  env.drv().use_pdma = true;
  SciDriver::Result fast = env.drv().execute(
      env.cfg().target_id, Bytes{0x08, 0, 0, 128, 1, 0}, Bytes(), 512);
  CHECK_DRV(fast.ok);

  // The two paths through the chip are different - one polls REQ and drives
  // ACK by hand, the other lets the chip do it - and they must not disagree
  // about a single byte.
  CHECK_EQ(fast.data, slow.data);
  CHECK_EQ(fast.data, env.disk().read_block(128));
}

TEST(wb_several_blocks_over_pseudo_dma) {
  env.power_on_reset();
  Bytes want;
  for (uint32_t i = 0; i < 4; i++) {
    env.disk().fill_pattern(300 + i, 0x2000 + i);
    Bytes b = env.disk().read_block(300 + i);
    want.insert(want.end(), b.begin(), b.end());
  }

  env.drv().use_pdma = true;
  env.disk().clear_counts();
  SciDriver::Result r = env.drv().execute(
      env.cfg().target_id, Bytes{0x08, 0x00, 0x01, 0x2c, 4, 0}, Bytes(), 4 * 512);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, 0x00);
  CHECK_EQ(r.data.size(), size_t(4 * 512));
  CHECK_EQ(r.data, want);
  CHECK_EQ(env.disk().reads(), size_t(4));
}

// SPDX-License-Identifier: MIT
//
// The card behind the block interface, and the whole stack on top of it.
//
// These drive the second instance in `tb_top`, which is the same design with a
// real SD card in its slot instead of a software disk.  They are the slowest
// tests in the suite by a wide margin - a card has to be initialised at 400
// kHz before it will say anything - which is exactly why the block interface
// exists and why nothing else pays for this.

#include "test.h"

using namespace wtb;

namespace {

// The suite's CHECK_DRV reports the fast path's driver, and everything here
// runs on the other one.
#define CHECK_SD(EXPR) CHECK_MSG(EXPR, env.sd_drv().last_error())

constexpr uint8_t ST_GOOD = 0x00;
constexpr uint8_t ST_CHECK = 0x02;

Bytes read6(uint32_t lba, uint8_t n) {
  return Bytes{0x08, uint8_t((lba >> 16) & 0x1f), uint8_t(lba >> 8),
               uint8_t(lba), n, 0};
}
Bytes write6(uint32_t lba, uint8_t n) {
  return Bytes{0x0a, uint8_t((lba >> 16) & 0x1f), uint8_t(lba >> 8),
               uint8_t(lba), n, 0};
}
Bytes request_sense() { return Bytes{0x03, 0, 0, 0, 18, 0}; }

}  // namespace

TEST(sd_the_card_comes_up_and_the_disk_reports_its_size) {
  env.power_on_reset();
  CHECK_MSG(env.wait_card_ready(), "the card never finished initialising");

  SciDriver::Result r = env.sd_drv().execute(
      env.cfg().target_id, Bytes{0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}, Bytes(), 8);
  CHECK_SD(r.ok);
  CHECK_EQ(r.status, ST_GOOD);

  uint32_t last = (uint32_t(r.data[0]) << 24) | (uint32_t(r.data[1]) << 16) |
                  (uint32_t(r.data[2]) << 8) | r.data[3];
  uint32_t bsize = (uint32_t(r.data[4]) << 24) | (uint32_t(r.data[5]) << 16) |
                   (uint32_t(r.data[6]) << 8) | r.data[7];
  // The size travelled from the card's CSD, through the capacity arithmetic,
  // into a SCSI READ CAPACITY answer.  Getting the CSD layout wrong gives a
  // disk of the right shape and the wrong size, which is the kind of fault
  // that only shows up when a filesystem runs off the end.
  CHECK_EQ(last, env.cfg().sd_blocks - 1);
  CHECK_EQ(bsize, uint32_t(512));
}

TEST(sd_initialisation_follows_the_specified_order) {
  env.power_on_reset();
  CHECK(env.wait_card_ready());

  // CMD0 to enter SPI mode, CMD8 to ask about the 2.00 spec, CMD55+ACMD41
  // until it stops reporting idle, CMD58 for the capacity class, and CMD9 for
  // the CSD.  A card that is sent these out of order does not come up.
  const std::vector<uint8_t>& log = env.card().command_log();
  CHECK_MSG(log.size() >= 6, "only " + std::to_string(log.size()) +
                                 " commands reached the card");
  CHECK_EQ(int(log[0]), 0);    // CMD0
  CHECK_EQ(int(log[1]), 8);    // CMD8

  bool saw_acmd41 = false, saw_cmd58 = false, saw_cmd9 = false;
  for (uint8_t c : log) {
    if (c == (41 | 0x80)) saw_acmd41 = true;   // an ACMD, not CMD41
    if (c == 58) saw_cmd58 = true;
    if (c == 9) saw_cmd9 = true;
  }
  CHECK_MSG(saw_acmd41, "ACMD41 was never sent, so the card never came up");
  CHECK_MSG(saw_cmd58, "CMD58 was never sent, so the addressing mode is a guess");
  CHECK_MSG(saw_cmd9, "CMD9 was never sent, so the capacity is a guess");
  CHECK(env.card().initialised());
}

TEST(sd_a_block_reads_back_through_the_whole_stack) {
  env.power_on_reset();
  env.card().fill_pattern(21, 0x5d10);
  Bytes want = env.card().read_block(21);
  CHECK(env.wait_card_ready());

  env.card().clear_counts();
  SciDriver::Result r =
      env.sd_drv().execute(env.cfg().target_id, read6(21, 1), Bytes(), 512);
  CHECK_SD(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  CHECK_EQ(r.data.size(), size_t(512));
  // Wishbone to the chip, the chip to the SCSI bus, the target to the block
  // interface, the block interface to CMD17, and every byte the same.
  CHECK_EQ(r.data, want);
  CHECK_EQ(env.card().reads(), size_t(1));
}

TEST(sd_a_block_writes_through_the_whole_stack) {
  env.power_on_reset();
  CHECK(env.wait_card_ready());
  Bytes payload = random_block(512, 0x5d11);

  env.card().clear_counts();
  SciDriver::Result r =
      env.sd_drv().execute(env.cfg().target_id, write6(77, 1), payload);
  CHECK_SD(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  CHECK_EQ(env.card().writes(), size_t(1));
  CHECK_EQ(env.card().read_block(77), payload);

  // ...and it reads back the same way.
  r = env.sd_drv().execute(env.cfg().target_id, read6(77, 1), Bytes(), 512);
  CHECK_SD(r.ok);
  CHECK_EQ(r.data, payload);
}

TEST(sd_several_blocks_in_one_command) {
  env.power_on_reset();
  Bytes want;
  for (uint32_t i = 0; i < 3; i++) {
    env.card().fill_pattern(400 + i, 0x3000 + i);
    Bytes b = env.card().read_block(400 + i);
    want.insert(want.end(), b.begin(), b.end());
  }
  CHECK(env.wait_card_ready());

  env.card().clear_counts();
  SciDriver::Result r =
      env.sd_drv().execute(env.cfg().target_id, read6(400, 3), Bytes(), 3 * 512);
  CHECK_SD(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  CHECK_EQ(r.data, want);
  // One CMD17 per block: the target asks for them one at a time and the
  // controller must not lose its place between them.
  CHECK_EQ(env.card().reads(), size_t(3));
}

TEST(sd_the_card_is_clocked_slowly_until_it_is_up) {
  env.power_on_reset();

  // A card must see no more than 400 kHz until initialisation is over.  The
  // clock is watched from the first command until the card is ready, and the
  // shortest half period seen has to be the slow one.
  uint64_t want = env.ticks_for_ps(1250 * 1000);  // 1.25 us: half of 400 kHz
  uint64_t last_edge = 0, shortest = ~uint64_t(0), n = 0, i = 0;
  bool prev = false;

  // Bounded by simulated time rather than by a clock count, because how many
  // clocks initialisation takes depends entirely on how fast the clock is.
  u64 t0 = env.sim().time_ps();
  while (env.sim().time_ps() - t0 < 20 * MS && !env.card().initialised()) {
    env.tick(1);
    i++;
    bool now = env.dut()->sd_clk_o != 0;
    if (now != prev) {
      if (last_edge != 0 && (i - last_edge) < shortest) shortest = i - last_edge;
      last_edge = i;
      n++;
    }
    prev = now;
  }

  CHECK_MSG(env.card().initialised(), "the card never came up");
  CHECK_MSG(n > 100, "only " + std::to_string(n) + " clock edges were seen");
  CHECK_MSG(shortest >= want,
            "the card was clocked with a half period of " +
                std::to_string(shortest) + " system clocks during "
                "initialisation, and needs at least " + std::to_string(want));
}

TEST(sd_the_card_is_clocked_fast_once_it_is_up) {
  env.power_on_reset();
  CHECK(env.wait_card_ready());
  env.card().fill_pattern(5, 0x1234);

  // At the initialisation clock a 512-byte block would take more than ten
  // milliseconds to move.  Anything close to that means the controller never
  // changed gear, which would make a real disk unusably slow without ever
  // failing a test that only checked the data.
  u64 t0 = env.sim().time_ps();
  SciDriver::Result r =
      env.sd_drv().execute(env.cfg().target_id, read6(5, 1), Bytes(), 512);
  u64 dt = env.sim().time_ps() - t0;
  CHECK_SD(r.ok);
  CHECK_EQ(r.data, env.card().read_block(5));
  CHECK_MSG(dt < 4 * MS, "a single block took " + std::to_string(dt / 1000000) +
                             " us, which is initialisation speed");
}

TEST(sd_a_version_1_card_is_addressed_in_bytes) {
  // A card below four gibibytes may be either kind, and real ones of both
  // exist.  A version 1 card has never heard of CMD8 and is addressed by byte
  // offset rather than by block number, so a controller that assumed the
  // modern case would read the wrong sector - or, on a small card, sector
  // zero for everything.
  env.card().set_present(true);
  env.card().set_high_capacity(false);
  env.power_on_reset();
  CHECK_MSG(env.wait_card_ready(), "a version 1 card never came up");

  env.card().fill_pattern(9, 0xa1a1);
  SciDriver::Result r =
      env.sd_drv().execute(env.cfg().target_id, read6(9, 1), Bytes(), 512);
  CHECK_SD(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  CHECK_EQ(r.data, env.card().read_block(9));

  // ...and its capacity comes out of the other CSD layout entirely.
  r = env.sd_drv().execute(env.cfg().target_id,
                           Bytes{0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}, Bytes(), 8);
  CHECK_SD(r.ok);
  uint32_t last = (uint32_t(r.data[0]) << 24) | (uint32_t(r.data[1]) << 16) |
                  (uint32_t(r.data[2]) << 8) | r.data[3];
  CHECK_EQ(last, env.cfg().sd_blocks - 1);
}

TEST(sd_no_card_means_the_disk_is_not_ready) {
  env.card().set_present(false);
  env.power_on_reset();
  // Nothing answers, so initialisation gives up rather than hanging, and the
  // target reports what a drive with no medium reports.
  CHECK_MSG(!env.wait_card_ready(1 * MS), "a disk appeared with no card in it");

  SciDriver::Result r =
      env.sd_drv().execute(env.cfg().target_id, Bytes{0x00, 0, 0, 0, 0, 0});
  CHECK_SD(r.ok);
  CHECK_EQ(r.status, ST_CHECK);

  r = env.sd_drv().execute(env.cfg().target_id, request_sense(), Bytes(), 18);
  CHECK_SD(r.ok);
  CHECK_EQ(uint8_t(r.data[2] & 0x0f), 0x02);  // NOT READY
  CHECK_EQ(r.data[12], 0x3a);                 // MEDIUM NOT PRESENT
}

TEST(sd_a_read_the_card_refuses_becomes_a_medium_error) {
  env.power_on_reset();
  CHECK(env.wait_card_ready());
  env.card().fail_read_on(33);

  SciDriver::Result r =
      env.sd_drv().execute(env.cfg().target_id, read6(33, 1), Bytes(), 512);
  CHECK_SD(r.ok);
  CHECK_EQ(r.status, ST_CHECK);
  CHECK_EQ(r.data.size(), size_t(0));

  r = env.sd_drv().execute(env.cfg().target_id, request_sense(), Bytes(), 18);
  CHECK_SD(r.ok);
  CHECK_EQ(uint8_t(r.data[2] & 0x0f), 0x03);  // MEDIUM ERROR
  CHECK_EQ(r.data[12], 0x11);                 // UNRECOVERED READ ERROR

  // ...and the next block is fine, so nothing was left wedged.
  env.card().fail_read_on(-1);
  r = env.sd_drv().execute(env.cfg().target_id, read6(34, 1), Bytes(), 512);
  CHECK_SD(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
}

TEST(sd_a_corrupt_block_is_caught_by_its_crc) {
  env.power_on_reset();
  CHECK(env.wait_card_ready());
  env.card().fill_pattern(44, 0xbad);
  env.card().corrupt_read_on(44);

  // The card sends good-looking data with a wrong CRC16.  In SPI mode nothing
  // obliges a host to check it, which is exactly why this one does: it is the
  // only thing standing between a marginal card and a silently corrupt
  // sector.
  SciDriver::Result r =
      env.sd_drv().execute(env.cfg().target_id, read6(44, 1), Bytes(), 512);
  CHECK_SD(r.ok);
  CHECK_MSG(r.status == ST_CHECK,
            "a block with a wrong CRC16 was passed on as good data");

  env.card().corrupt_read_on(-1);
  r = env.sd_drv().execute(env.cfg().target_id, read6(44, 1), Bytes(), 512);
  CHECK_SD(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  CHECK_EQ(r.data, env.card().read_block(44));
}

TEST(sd_a_write_the_card_refuses_is_reported) {
  env.power_on_reset();
  CHECK(env.wait_card_ready());
  env.card().fail_write_on(55);

  SciDriver::Result r = env.sd_drv().execute(env.cfg().target_id, write6(55, 1),
                                             random_block(512, 0x99));
  CHECK_SD(r.ok);
  CHECK_EQ(r.status, ST_CHECK);
  CHECK_EQ(env.card().writes(), size_t(0));

  env.card().fail_write_on(-1);
  Bytes payload = random_block(512, 0x9a);
  r = env.sd_drv().execute(env.cfg().target_id, write6(55, 1), payload);
  CHECK_SD(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  CHECK_EQ(env.card().read_block(55), payload);
}

TEST(sd_commands_can_follow_one_another) {
  env.power_on_reset();
  CHECK(env.wait_card_ready());

  // Nothing is left behind between operations: the card goes back to idle,
  // the controller goes back to ready, and a hundred sectors in a row work
  // the same as the first.
  for (uint32_t i = 0; i < 6; i++) {
    Bytes payload = random_block(512, 0x700 + i);
    SciDriver::Result w =
        env.sd_drv().execute(env.cfg().target_id, write6(100 + i, 1), payload);
    CHECK_MSG(w.ok && w.status == ST_GOOD,
              "write " + std::to_string(i) + ": " + env.sd_drv().last_error());
    SciDriver::Result r = env.sd_drv().execute(env.cfg().target_id,
                                               read6(100 + i, 1), Bytes(), 512);
    CHECK_MSG(r.ok && r.status == ST_GOOD,
              "read " + std::to_string(i) + ": " + env.sd_drv().last_error());
    CHECK_MSG(r.data == payload, "block " + std::to_string(i) + " came back wrong");
  }
}

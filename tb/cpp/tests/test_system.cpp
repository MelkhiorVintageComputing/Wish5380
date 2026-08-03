// SPDX-License-Identifier: MIT
//
// The whole thing: a driver arbitrates, selects, sends a command descriptor
// block and gets an answer, through the chip's registers and over the fabric,
// exactly as a vintage machine would.
//
// The driver model follows `doc/drivers/Linux/NCR5380.c` step for step, so a
// failure here is a failure a real driver would have hit.  Nothing in these
// tests touches a Verilator signal: if a test needs to reach inside, the
// models are missing something.

#include "test.h"

using namespace wtb;

namespace {

// The commands the tests issue, built the way a driver builds them.
Bytes cdb6(uint8_t op, uint32_t lba, uint8_t len, uint8_t ctl = 0) {
  return Bytes{op, uint8_t((lba >> 16) & 0x1f), uint8_t(lba >> 8),
               uint8_t(lba), len, ctl};
}

Bytes cdb10(uint8_t op, uint32_t lba, uint16_t len) {
  return Bytes{op,
               0,
               uint8_t(lba >> 24),
               uint8_t(lba >> 16),
               uint8_t(lba >> 8),
               uint8_t(lba),
               0,
               uint8_t(len >> 8),
               uint8_t(len),
               0};
}

Bytes inquiry(uint8_t alloc) { return Bytes{0x12, 0, 0, 0, alloc, 0}; }
Bytes test_unit_ready() { return Bytes{0x00, 0, 0, 0, 0, 0}; }
Bytes request_sense(uint8_t alloc) { return Bytes{0x03, 0, 0, 0, alloc, 0}; }
Bytes read_capacity() { return Bytes{0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}; }

constexpr uint8_t ST_GOOD = 0x00;
constexpr uint8_t ST_CHECK = 0x02;

std::string ascii(const Bytes& b, size_t off, size_t n) {
  std::string s;
  for (size_t i = 0; i < n && off + i < b.size(); i++) s += char(b[off + i]);
  return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Getting to the target at all
// ---------------------------------------------------------------------------

TEST(sys_arbitrate_and_select_the_target) {
  env.power_on_reset();
  CHECK_DRV(env.drv().select(env.cfg().target_id));
  // The target answered by asserting BSY, and the driver is still holding ATN
  // so the first phase will be MESSAGE OUT.
  CHECK(env.chip_read(sci::R_CSB) & sci::CSB_BSY);
}

TEST(sys_selecting_an_empty_id_times_out) {
  env.power_on_reset();
  // Nothing answers at ID 3, and the driver gives up rather than hanging.
  // This is the path `NCR5380_select` reports as DID_BAD_TARGET.
  env.drv().t_select = 100 * US;
  CHECK(!env.drv().select(3));
  CHECK_MSG(env.drv().last_error() == "selection timeout",
            "gave up for the wrong reason: " + env.drv().last_error());
  // ...and the bus is left free for the next attempt.
  env.tick(4);
  CHECK_EQ(uint8_t(env.chip_read(sci::R_CSB) & sci::CSB_BSY), 0);
}

TEST(sys_test_unit_ready) {
  env.power_on_reset();
  SciDriver::Result r = env.drv().execute(env.cfg().target_id, test_unit_ready());
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  CHECK_EQ(r.message, 0x00);   // COMMAND COMPLETE
}

TEST(sys_test_unit_ready_reports_no_medium) {
  env.power_on_reset();
  env.disk().set_ready(false);
  SciDriver::Result r = env.drv().execute(env.cfg().target_id, test_unit_ready());
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_CHECK);

  // ...and the sense the driver then asks for says why.
  r = env.drv().execute(env.cfg().target_id, request_sense(18), Bytes(), 18);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  CHECK_EQ(r.data.size(), size_t(18));
  CHECK_EQ(r.data[0], 0x70);            // current error
  CHECK_EQ(uint8_t(r.data[2] & 0x0f), 0x02);  // NOT READY
  CHECK_EQ(r.data[7], 10);              // additional sense length
  CHECK_EQ(r.data[12], 0x3a);           // MEDIUM NOT PRESENT
}

// ---------------------------------------------------------------------------
// What a driver reads to decide what it has found
// ---------------------------------------------------------------------------

TEST(sys_inquiry_describes_a_direct_access_device) {
  env.power_on_reset();
  SciDriver::Result r = env.drv().execute(env.cfg().target_id, inquiry(36),
                                          Bytes(), 36);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  CHECK_EQ(r.data.size(), size_t(36));

  CHECK_EQ(r.data[0], 0x00);   // direct access device, qualifier 0
  CHECK_EQ(uint8_t(r.data[1] & 0x80), 0);  // not removable
  CHECK_EQ(r.data[4], 31);     // additional length: 36 in all
  CHECK_EQ(ascii(r.data, 8, 8), std::string("DOLBEAU "));
  CHECK_EQ(ascii(r.data, 16, 16), std::string("WISH5380 SD CARD"));
  CHECK_EQ(ascii(r.data, 32, 4), std::string("0001"));
}

TEST(sys_inquiry_is_cut_short_by_the_allocation_length) {
  env.power_on_reset();
  // A driver that only wants the first few bytes gets the first few bytes.
  // Every probe does this before deciding whether to ask for the rest.
  SciDriver::Result r = env.drv().execute(env.cfg().target_id, inquiry(5),
                                          Bytes(), 36);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.data.size(), size_t(5));
  CHECK_EQ(r.data[0], 0x00);
  CHECK_EQ(r.data[4], 31);

  // An allocation length of zero means zero, and is not 256 - the one place
  // that rule differs from a six-byte READ's transfer length.
  r = env.drv().execute(env.cfg().target_id, inquiry(0), Bytes(), 36);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.data.size(), size_t(0));
  CHECK_EQ(r.status, ST_GOOD);
}

TEST(sys_inquiry_to_an_absent_logical_unit_says_nothing_here) {
  env.power_on_reset();
  // Probing logical units is the first thing a driver does, and answering
  // CHECK CONDITION makes some of them give up on the whole target.  The
  // standard's answer is peripheral qualifier 3 with device type 0x1f.
  SciDriver::Result r =
      env.drv().execute(env.cfg().target_id, inquiry(36), Bytes(), 36, 3);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  CHECK_EQ(r.data[0], 0x7f);

  // Anything else aimed at that unit is refused, with sense saying which.
  r = env.drv().execute(env.cfg().target_id, test_unit_ready(), Bytes(), 0, 3);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_CHECK);
}

TEST(sys_read_capacity_reports_the_last_block_not_the_count) {
  env.power_on_reset();
  SciDriver::Result r = env.drv().execute(env.cfg().target_id, read_capacity(),
                                          Bytes(), 8);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  CHECK_EQ(r.data.size(), size_t(8));

  uint32_t last = (uint32_t(r.data[0]) << 24) | (uint32_t(r.data[1]) << 16) |
                  (uint32_t(r.data[2]) << 8) | r.data[3];
  uint32_t bsize = (uint32_t(r.data[4]) << 24) | (uint32_t(r.data[5]) << 16) |
                   (uint32_t(r.data[6]) << 8) | r.data[7];
  CHECK_EQ(last, env.cfg().disk_blocks - 1);
  CHECK_EQ(bsize, uint32_t(512));
}

TEST(sys_an_unknown_command_is_refused_with_sense) {
  env.power_on_reset();
  // 0xff is not a command any drive of the period had.
  SciDriver::Result r =
      env.drv().execute(env.cfg().target_id, Bytes{0xff, 0, 0, 0, 0, 0});
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_CHECK);

  r = env.drv().execute(env.cfg().target_id, request_sense(18), Bytes(), 18);
  CHECK_DRV(r.ok);
  CHECK_EQ(uint8_t(r.data[2] & 0x0f), 0x05);  // ILLEGAL REQUEST
  CHECK_EQ(r.data[12], 0x20);                 // INVALID COMMAND OPERATION CODE

  // Reading the sense clears it: the next command starts clean.
  r = env.drv().execute(env.cfg().target_id, request_sense(18), Bytes(), 18);
  CHECK_DRV(r.ok);
  CHECK_EQ(uint8_t(r.data[2] & 0x0f), 0x00);  // NO SENSE
}

// ---------------------------------------------------------------------------
// Moving blocks
// ---------------------------------------------------------------------------

TEST(sys_read_one_block) {
  env.power_on_reset();
  env.disk().fill_pattern(17, 0xc0ffee);
  Bytes want = env.disk().read_block(17);

  SciDriver::Result r =
      env.drv().execute(env.cfg().target_id, cdb6(0x08, 17, 1), Bytes(), 512);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  CHECK_EQ(r.data.size(), size_t(512));
  CHECK_EQ(r.data, want);
  CHECK_EQ(env.disk().last_lba(), uint32_t(17));
}

TEST(sys_write_then_read_back_one_block) {
  env.power_on_reset();
  Bytes payload = random_block(512, 0xbeef);

  SciDriver::Result r =
      env.drv().execute(env.cfg().target_id, cdb6(0x0a, 5, 1), payload);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  // The block reached the media, not just the target's buffer.
  CHECK_EQ(env.disk().read_block(5), payload);

  r = env.drv().execute(env.cfg().target_id, cdb6(0x08, 5, 1), Bytes(), 512);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.data, payload);
}

TEST(sys_read_several_blocks_in_one_command) {
  env.power_on_reset();
  Bytes want;
  for (uint32_t i = 0; i < 3; i++) {
    env.disk().fill_pattern(100 + i, 0x1000 + i);
    Bytes b = env.disk().read_block(100 + i);
    want.insert(want.end(), b.begin(), b.end());
  }

  env.disk().clear_counts();
  SciDriver::Result r =
      env.drv().execute(env.cfg().target_id, cdb6(0x08, 100, 3), Bytes(), 3 * 512);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  CHECK_EQ(r.data.size(), size_t(3 * 512));
  CHECK_EQ(r.data, want);
  // One block fetched per block asked for, and no more: a target that
  // re-read a block would still pass the comparison above.
  CHECK_EQ(env.disk().reads(), size_t(3));
}

TEST(sys_write_several_blocks_in_one_command) {
  env.power_on_reset();
  Bytes payload = random_block(2 * 512, 0x5eed);

  env.disk().clear_counts();
  SciDriver::Result r =
      env.drv().execute(env.cfg().target_id, cdb6(0x0a, 200, 2), payload);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  CHECK_EQ(env.disk().writes(), size_t(2));
  CHECK_EQ(env.disk().read_block(200), Bytes(payload.begin(), payload.begin() + 512));
  CHECK_EQ(env.disk().read_block(201), Bytes(payload.begin() + 512, payload.end()));
}

TEST(sys_ten_byte_read_addresses_the_same_blocks) {
  env.power_on_reset();
  env.disk().fill_pattern(1000, 0xabcd);
  Bytes want = env.disk().read_block(1000);

  // The ten-byte form puts the address and the length in different places;
  // getting either offset wrong reads the wrong block, which is exactly what
  // this compares against the six-byte form's answer.
  SciDriver::Result r =
      env.drv().execute(env.cfg().target_id, cdb10(0x28, 1000, 1), Bytes(), 512);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  CHECK_EQ(r.data, want);
  CHECK_EQ(env.disk().last_lba(), uint32_t(1000));
}

TEST(sys_ten_byte_write_addresses_the_same_blocks) {
  env.power_on_reset();
  Bytes payload = random_block(512, 0x1234);
  SciDriver::Result r =
      env.drv().execute(env.cfg().target_id, cdb10(0x2a, 700, 1), payload);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  CHECK_EQ(env.disk().read_block(700), payload);
}

TEST(sys_a_six_byte_transfer_length_of_zero_means_256_blocks) {
  env.power_on_reset();
  // The one place SCSI-1 counts that way, and the reason a six-byte command
  // cannot share its length decode with an allocation length.
  env.disk().clear_counts();
  SciDriver::Result r = env.drv().execute(env.cfg().target_id,
                                          cdb6(0x08, 0, 0), Bytes(), 256 * 512);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  CHECK_EQ(r.data.size(), size_t(256 * 512));
  CHECK_EQ(env.disk().reads(), size_t(256));
}

TEST(sys_reading_past_the_end_is_refused) {
  env.power_on_reset();
  uint32_t last = env.cfg().disk_blocks - 1;

  // The last block is fine.
  SciDriver::Result r =
      env.drv().execute(env.cfg().target_id, cdb10(0x28, last, 1), Bytes(), 512);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_GOOD);

  // One past it is not, and nothing is fetched.
  env.disk().clear_counts();
  r = env.drv().execute(env.cfg().target_id, cdb10(0x28, last + 1, 1), Bytes(), 512);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_CHECK);
  CHECK_EQ(env.disk().reads(), size_t(0));

  r = env.drv().execute(env.cfg().target_id, request_sense(18), Bytes(), 18);
  CHECK_DRV(r.ok);
  CHECK_EQ(uint8_t(r.data[2] & 0x0f), 0x05);  // ILLEGAL REQUEST
  CHECK_EQ(r.data[12], 0x21);                 // LOGICAL BLOCK ADDRESS OUT OF RANGE

  // A transfer that starts inside and runs off the end is refused as a whole.
  r = env.drv().execute(env.cfg().target_id, cdb10(0x28, last, 2), Bytes(), 1024);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_CHECK);
}

TEST(sys_a_media_error_becomes_check_condition_and_sense) {
  env.power_on_reset();
  env.disk().fail_on(42);

  SciDriver::Result r =
      env.drv().execute(env.cfg().target_id, cdb6(0x08, 42, 1), Bytes(), 512);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_CHECK);
  // The target must not have offered any data for a block it never got.
  CHECK_EQ(r.data.size(), size_t(0));

  r = env.drv().execute(env.cfg().target_id, request_sense(18), Bytes(), 18);
  CHECK_DRV(r.ok);
  CHECK_EQ(uint8_t(r.data[2] & 0x0f), 0x03);  // MEDIUM ERROR
  CHECK_EQ(r.data[12], 0x11);                 // UNRECOVERED READ ERROR
  // The sense carries the block it happened on.
  uint32_t at = (uint32_t(r.data[3]) << 24) | (uint32_t(r.data[4]) << 16) |
                (uint32_t(r.data[5]) << 8) | r.data[6];
  CHECK_EQ(at, uint32_t(42));

  // ...and the next block is fine, so nothing was left wedged.
  env.disk().fail_on(-1);
  r = env.drv().execute(env.cfg().target_id, cdb6(0x08, 43, 1), Bytes(), 512);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
}

// ---------------------------------------------------------------------------
// The rest of what a driver does
// ---------------------------------------------------------------------------

TEST(sys_the_quiet_commands_all_succeed) {
  env.power_on_reset();
  // None of these has anything to do here, and a driver decides the disk is
  // broken if any of them fails.  REZERO, SEEK, START STOP UNIT,
  // PREVENT/ALLOW, SEND DIAGNOSTIC, VERIFY.
  const uint8_t ops[] = {0x01, 0x0b, 0x1b, 0x1e, 0x1d};
  for (uint8_t op : ops) {
    SciDriver::Result r =
        env.drv().execute(env.cfg().target_id, Bytes{op, 0, 0, 0, 0, 0});
    CHECK_MSG(r.ok, "command 0x" + std::to_string(int(op)) + ": " +
                        env.drv().last_error());
    CHECK_MSG(r.status == ST_GOOD,
              "command 0x" + std::to_string(int(op)) + " returned status " +
                  std::to_string(int(r.status)));
  }
}

TEST(sys_mode_sense_describes_the_geometry) {
  env.power_on_reset();
  SciDriver::Result r = env.drv().execute(env.cfg().target_id,
                                          Bytes{0x1a, 0, 0x3f, 0, 12, 0},
                                          Bytes(), 12);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  CHECK_EQ(r.data.size(), size_t(12));
  CHECK_EQ(r.data[0], 11);   // the length of what follows
  CHECK_EQ(r.data[3], 8);    // one block descriptor

  uint32_t blocks = (uint32_t(r.data[5]) << 16) | (uint32_t(r.data[6]) << 8) |
                    r.data[7];
  // The block descriptor starts at byte 4: a density code, three bytes of
  // block count, a reserved byte, then three bytes of block length.  So the
  // length is bytes 9 to 11, not 8 to 10.
  uint32_t bsize = (uint32_t(r.data[9]) << 16) | (uint32_t(r.data[10]) << 8) |
                   r.data[11];
  CHECK_EQ(blocks, env.cfg().disk_blocks);
  CHECK_EQ(bsize, uint32_t(512));
}

TEST(sys_mode_select_is_accepted_and_ignored) {
  env.power_on_reset();
  // A driver sends this during initialisation and stops if it fails.  There
  // is no geometry here to change, so it is taken and thrown away.
  Bytes params{0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x08, 0x00,
               0x00, 0x00, 0x02, 0x00};
  SciDriver::Result r = env.drv().execute(
      env.cfg().target_id, Bytes{0x15, 0x10, 0, 0, uint8_t(params.size()), 0},
      params);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
}

TEST(sys_a_bus_reset_frees_the_target_mid_command) {
  env.power_on_reset();
  CHECK_DRV(env.drv().select(env.cfg().target_id));
  CHECK(env.chip_read(sci::R_CSB) & sci::CSB_BSY);

  // Pull the bus out from under a connected target.  Both the chip and the
  // target must let go, and the next command must work as if nothing had
  // happened - which is the whole point of the driver's reset path.
  env.drv().reset_bus();
  CHECK_EQ(uint8_t(env.chip_read(sci::R_CSB) & sci::CSB_BSY), 0);

  SciDriver::Result r = env.drv().execute(env.cfg().target_id, inquiry(36),
                                          Bytes(), 36);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_GOOD);
  CHECK_EQ(r.data[0], 0x00);
}

TEST(sys_commands_can_follow_one_another) {
  env.power_on_reset();
  // Nothing is left behind between commands: the bus goes free, the target
  // goes back to watching for a selection, and the chip's registers are where
  // the driver left them.  A probe issues dozens of these in a row.
  for (int i = 0; i < 8; i++) {
    SciDriver::Result r =
        env.drv().execute(env.cfg().target_id, test_unit_ready());
    CHECK_MSG(r.ok, "command " + std::to_string(i) + ": " +
                        env.drv().last_error());
    CHECK_EQ(r.status, ST_GOOD);
  }

  SciDriver::Result r = env.drv().execute(env.cfg().target_id, inquiry(36),
                                          Bytes(), 36);
  CHECK_DRV(r.ok);
  CHECK_EQ(ascii(r.data, 8, 8), std::string("DOLBEAU "));
}

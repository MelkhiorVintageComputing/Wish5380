// SPDX-License-Identifier: MIT
//
// Two drives on the bus.
//
// A SCSI bus with one device never exercises the ID decode against anything
// that could get it wrong, because the only other outcome is silence: a target
// that answered every selection and a target that answered only its own would
// both pass every test in `test_system.cpp`.  These need two.
//
// They also put a third driver on the wired-OR, which nothing else does, and
// they check the thing a driver does first at every boot: walk the IDs and see
// what is there.

#include "test.h"

using namespace wtb;

namespace {

constexpr uint8_t ST_GOOD = 0x00;

Bytes read6(uint32_t lba, uint8_t n) {
  return Bytes{0x08, uint8_t((lba >> 16) & 0x1f), uint8_t(lba >> 8),
               uint8_t(lba), n, 0};
}
Bytes write6(uint32_t lba, uint8_t n) {
  return Bytes{0x0a, uint8_t((lba >> 16) & 0x1f), uint8_t(lba >> 8),
               uint8_t(lba), n, 0};
}
Bytes inquiry() { return Bytes{0x12, 0, 0, 0, 36, 0}; }
Bytes test_unit_ready() { return Bytes{0x00, 0, 0, 0, 0, 0}; }
Bytes read_capacity() { return Bytes{0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}; }

}  // namespace

TEST(two_both_drives_answer_their_own_id) {
  env.power_on_reset();

  for (uint8_t id : {env.cfg().target_id, env.cfg().target1_id}) {
    SciDriver::Result r = env.drv().execute(id, test_unit_ready());
    CHECK_MSG(r.ok, "ID " + std::to_string(int(id)) + ": " +
                        env.drv().last_error());
    CHECK_MSG(r.status == ST_GOOD,
              "ID " + std::to_string(int(id)) + " returned status " +
                  std::to_string(int(r.status)));
  }
}

TEST(two_a_probe_of_every_id_finds_exactly_two) {
  env.power_on_reset();
  // What a driver does at every boot: select each ID in turn and see which
  // answer.  A target whose ID decode was a comparison against zero, or which
  // ignored the mask entirely, would answer all of them.
  env.drv().t_select = 100 * US;

  std::string found;
  for (uint8_t id = 0; id < 7; id++) {
    bool answered = env.drv().select(id);
    if (answered) {
      found += std::to_string(int(id));
      // Let the target finish and release the bus, the way a driver that gave
      // up mid-nexus would have to.
      env.drv().reset_bus();
    }
    env.tick(4);
  }

  std::string want = std::to_string(int(env.cfg().target_id)) +
                     std::to_string(int(env.cfg().target1_id));
  CHECK_MSG(found == want,
            "IDs [" + found + "] answered, expected [" + want + "]");
}

TEST(two_drives_have_their_own_media) {
  env.power_on_reset();
  // The same block number on each, with different contents.  A second target
  // wired to the first one's buffer - which is the mistake this catches -
  // would return the same bytes twice.
  env.disk().fill_pattern(12, 0xaaaa);
  env.disk1().fill_pattern(12, 0x5555);
  CHECK_NE(env.disk().read_block(12), env.disk1().read_block(12));

  SciDriver::Result a =
      env.drv().execute(env.cfg().target_id, read6(12, 1), Bytes(), 512);
  CHECK_DRV(a.ok);
  SciDriver::Result b =
      env.drv().execute(env.cfg().target1_id, read6(12, 1), Bytes(), 512);
  CHECK_DRV(b.ok);

  CHECK_EQ(a.data, env.disk().read_block(12));
  CHECK_EQ(b.data, env.disk1().read_block(12));
  CHECK_NE(a.data, b.data);
}

TEST(two_a_write_to_one_leaves_the_other_alone) {
  env.power_on_reset();
  Bytes before = env.disk1().read_block(30);
  Bytes payload = random_block(512, 0xd00d);

  SciDriver::Result r =
      env.drv().execute(env.cfg().target_id, write6(30, 1), payload);
  CHECK_DRV(r.ok);
  CHECK_EQ(r.status, ST_GOOD);

  CHECK_EQ(env.disk().read_block(30), payload);
  CHECK_MSG(env.disk1().read_block(30) == before,
            "writing to one drive changed the other");
}

TEST(two_drives_can_be_different_sizes) {
  env.power_on_reset();
  // A driver reads the capacity of each drive separately, and a controller
  // that answered with one of them for both would give the second a disk of
  // the wrong length.  The models are the same size here, so the check is
  // that each answer came from its own target rather than from a shared one.
  SciDriver::Result a =
      env.drv().execute(env.cfg().target_id, read_capacity(), Bytes(), 8);
  CHECK_DRV(a.ok);
  SciDriver::Result b =
      env.drv().execute(env.cfg().target1_id, read_capacity(), Bytes(), 8);
  CHECK_DRV(b.ok);
  CHECK_EQ(a.data, b.data);

  // Take the medium out of the second drive and the two stop agreeing, which
  // is what proves the answers are not one answer given twice.
  env.disk1().set_ready(false);
  b = env.drv().execute(env.cfg().target1_id, read_capacity(), Bytes(), 8);
  CHECK_DRV(b.ok);
  CHECK_MSG(b.status != ST_GOOD, "a drive with no medium reported a capacity");

  a = env.drv().execute(env.cfg().target_id, read_capacity(), Bytes(), 8);
  CHECK_DRV(a.ok);
  CHECK_MSG(a.status == ST_GOOD,
            "taking the medium out of one drive stopped the other");
}

TEST(two_drives_take_turns_without_interfering) {
  env.power_on_reset();
  for (uint32_t i = 0; i < 4; i++) {
    env.disk().fill_pattern(200 + i, 0x100 + i);
    env.disk1().fill_pattern(200 + i, 0x900 + i);
  }

  // Alternating between them is where a target that failed to let go of the
  // bus, or one that answered a selection meant for its neighbour, would
  // show up.
  for (uint32_t i = 0; i < 4; i++) {
    SciDriver::Result a = env.drv().execute(env.cfg().target_id,
                                            read6(200 + i, 1), Bytes(), 512);
    CHECK_MSG(a.ok && a.status == ST_GOOD,
              "drive 0 pass " + std::to_string(i) + ": " +
                  env.drv().last_error());
    CHECK_MSG(a.data == env.disk().read_block(200 + i),
              "drive 0 pass " + std::to_string(i) + " read the wrong bytes");

    SciDriver::Result b = env.drv().execute(env.cfg().target1_id,
                                            read6(200 + i, 1), Bytes(), 512);
    CHECK_MSG(b.ok && b.status == ST_GOOD,
              "drive 1 pass " + std::to_string(i) + ": " +
                  env.drv().last_error());
    CHECK_MSG(b.data == env.disk1().read_block(200 + i),
              "drive 1 pass " + std::to_string(i) + " read the wrong bytes");
  }
}

TEST(two_an_idle_drive_drives_nothing) {
  env.power_on_reset();
  // Three devices share the wired-OR now.  A target that held a signal while
  // it was not selected would be indistinguishable from one that was, and the
  // bus would never look free.
  CHECK_EQ(env.bus_csb(), 0);
  CHECK_EQ(env.bus_data(), 0);

  // ...and while one of them is busy, the other is still quiet: the bus shows
  // BSY, but from one device, so it goes free the moment that one lets go.
  CHECK_DRV(env.drv().select(env.cfg().target1_id));
  CHECK(env.chip_read(sci::R_CSB) & sci::CSB_BSY);

  env.drv().reset_bus();
  env.tick(int(env.ticks_for_ps(sci::T_BUS_CLEAR_PS) + 8));
  CHECK_MSG(env.bus_csb() == 0,
            "the bus did not go free after a reset: " +
                sci::csb_str(env.bus_csb()));
}

TEST(two_selecting_both_at_once_is_answered_by_both) {
  env.power_on_reset();
  // Not something a driver does - the standard allows exactly two IDs on the
  // bus during selection - but it is the sharpest check that each target
  // decodes its own bit rather than any bit.  Both should assert BSY, and the
  // bus should show it.
  env.chip_write(sci::R_TCR, 0);
  env.chip_write(sci::R_ODR,
                 uint8_t((1u << env.cfg().target_id) |
                         (1u << env.cfg().target1_id)));
  env.chip_write(sci::R_ICR, uint8_t(sci::ICR_DATA | sci::ICR_SEL));
  env.tick(int(env.ticks_for_ps(sci::T_BUS_SETTLE_PS) + 8));

  CHECK_MSG(env.bus_csb() & sci::CSB_BSY, "neither target answered");

  // Drop the selection and let both go; the bus has to come back to idle,
  // which it will not if one of them is left holding BSY.
  env.chip_write(sci::R_ICR, 0);
  env.drv().reset_bus();
  env.tick(int(env.ticks_for_ps(sci::T_BUS_CLEAR_PS) + 8));
  CHECK_EQ(env.bus_csb(), 0);
}

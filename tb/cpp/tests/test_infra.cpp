// SPDX-License-Identifier: MIT
//
// The testbench checking itself.
//
// These come first in the trust order: if one of them fails, no result from
// any other prefix means anything, because the clock, the reset or the
// register accessors underneath them are not doing what the rest of the suite
// assumes.

#include "test.h"

using namespace wtb;

TEST(infra_clock_runs_at_the_configured_period) {
  u64 t0 = env.sim().time_ps();
  env.tick(10);
  u64 dt = env.sim().time_ps() - t0;
  CHECK_EQ(dt, 10 * env.cfg().sys_period_ps);
}

TEST(infra_run_until_stops_on_the_condition) {
  int n = 0;
  env.sim().on_posedge(env.sysclk(), [&n]() { n++; });
  bool got = env.sim().run_until([&n]() { return n >= 5; }, 1 * US);
  CHECK(got);
  CHECK(n >= 5);

  // ...and gives up rather than hanging when it never holds.
  bool never = env.sim().run_until([]() { return false; }, 200 * NS);
  CHECK(!never);
}

TEST(infra_reset_is_synchronous_and_releases) {
  env.dut()->rst_i = 1;
  env.tick(4);
  // Something was written while reset was held; it must not have stuck.
  env.reg_write(sci::R_MR, 0xff);
  CHECK_EQ(env.dut()->rg_mr_o, 0);

  env.power_on_reset();
  env.reg_write(sci::R_MR, 0xff);
  CHECK_EQ(env.dut()->rg_mr_o, 0xff);
}

TEST(infra_register_accessor_is_one_cycle_wide) {
  env.power_on_reset();
  u64 t0 = env.sim().time_ps();
  env.reg_write(sci::R_MR, 0x12);
  CHECK_EQ(env.sim().time_ps() - t0, env.cfg().sys_period_ps);
  // The strobe must be down again afterwards, or the next access would be
  // seen as a second one at the old address.
  CHECK_EQ(env.dut()->rg_stb_i, 0);
}

TEST(infra_hex_dump_and_random_block_are_deterministic) {
  (void)env;
  Bytes a = random_block(16, 1);
  Bytes b = random_block(16, 1);
  Bytes c = random_block(16, 2);
  CHECK_EQ(a, b);
  CHECK_NE(a, c);
  CHECK_EQ(a.size(), size_t(16));
  // The dump is what a failure message shows, so it has to survive an empty
  // buffer and say how much it left out of a long one.
  CHECK_EQ(hex_dump(Bytes()), std::string(""));
  CHECK(hex_dump(random_block(100, 3), 8).find("...") != std::string::npos);
}

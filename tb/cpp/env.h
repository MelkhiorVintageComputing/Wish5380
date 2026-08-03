// SPDX-License-Identifier: MIT
//
// Per-test environment: a fresh DUT, its clock, and the models around it.
//
// Each test gets its own Env, so nothing leaks from one test to the next and a
// failing test can be rerun on its own with a waveform.

#pragma once

#include <memory>
#include <string>

#include "ncr5380.h"
#include "sim.h"
#include "util.h"
#include "wb.h"

class VerilatedContext;
class VerilatedVcdC;
class Vtb_top;

namespace wtb {

// The system clock period in picoseconds, set by the Makefile.  The 5380 is a
// clockless part whose delays come out of gate propagation, so everything the
// RTL counts out - the 400 ns bus free filter, the 800 ns bus clear delay - is
// derived from this number rather than written down.  Build for a slower
// machine with `make test SYS_PERIOD_PS=50000` and both the RTL and the
// testbench follow.
#ifndef SYS_PERIOD_PS
#define SYS_PERIOD_PS 20000
#endif

struct EnvConfig {
  u64 sys_period_ps = u64(SYS_PERIOD_PS);
};

class Env {
 public:
  Env(const std::string& test_name, bool trace, EnvConfig cfg = EnvConfig());
  ~Env();

  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;

  // Pulses the global (power on) reset and lets things settle.
  void power_on_reset(int cycles = 8);
  void tick(int cycles = 1);

  Vtb_top* dut() { return dut_.get(); }
  Sim& sim() { return *sim_; }
  Sim::Clock* sysclk() { return sysclk_; }
  const EnvConfig& cfg() const { return cfg_; }
  const std::string& name() const { return name_; }

  // ---- the register file, poked on its own -------------------------------
  //
  // These drive the leaf module wired into tb_top beside the chip, so a test
  // can check the register map without a bus or an engine in the way.  They
  // are blocking: each runs the clock, in the same way a driver's register
  // access blocks until the chip answers.
  void reg_write(uint8_t adr, uint8_t data);
  uint8_t reg_read(uint8_t adr);
  // The same two with DACK asserted, which reaches a data register whatever
  // the address lines say (p. 6).
  void reg_write_dack(uint8_t data);
  uint8_t reg_read_dack();

  // Which strobes the register file raised during the last access.  They are
  // one cycle wide and combinational on the access itself, so there is no way
  // for a test to catch them after the fact; the accessors above sample them
  // while the strobe is still up.
  enum Strobe : uint8_t { S_RPI = 1, S_SDS = 2, S_SDTR = 4, S_SDIR = 8 };
  uint8_t last_strobes() const { return strobes_; }

 private:
  void bind_models();

  std::string name_;
  EnvConfig cfg_;
  std::unique_ptr<VerilatedContext> ctx_;
  std::unique_ptr<Vtb_top> dut_;
  std::unique_ptr<VerilatedVcdC> tfp_;
  std::unique_ptr<Sim> sim_;
  Sim::Clock* sysclk_ = nullptr;
  void sample_strobes();
  uint8_t strobes_ = 0;
};

}  // namespace wtb

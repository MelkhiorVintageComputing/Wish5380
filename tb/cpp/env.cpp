// SPDX-License-Identifier: MIT

#include "env.h"

#include <verilated.h>
#include <verilated_vcd_c.h>

#include <sys/stat.h>
#include <sys/types.h>

#include "Vtb_top.h"

namespace wtb {

Env::Env(const std::string& test_name, bool trace, EnvConfig cfg)
    : name_(test_name), cfg_(cfg) {
  ctx_.reset(new VerilatedContext());
  ctx_->timeunit(-12);  // the kernel counts in picoseconds
  ctx_->timeprecision(-12);
  if (trace) ctx_->traceEverOn(true);

  dut_.reset(new Vtb_top(ctx_.get(), (name_ + "_dut").c_str()));

  sim_.reset(new Sim([this]() { dut_->eval(); }));

  if (trace) {
    tfp_.reset(new VerilatedVcdC());
    dut_->trace(tfp_.get(), 99);
    ::mkdir("build", 0755);
    ::mkdir("build/waves", 0755);
    tfp_->open(("build/waves/" + name_ + ".vcd").c_str());
    sim_->set_trace(tfp_.get());
  }

  sysclk_ = sim_->add_clock(&dut_->clk_i, cfg_.sys_period_ps, "clk");
  bind_models();
}

void Env::bind_models() {
  Vtb_top* d = dut_.get();

  d->rst_i = 1;
  d->dut_stb_i = 0;
  d->dut_we_i = 0;
  d->dut_dack_i = 0;
  d->dut_adr_i = 0;
  d->dut_dat_i = 0;
  d->dut_eop_i = 0;
  drive_peer(Peer());
  // The leaf register file is given a connected bus by default: most of the
  // reg_ tests are not about the one rule that depends on it, and
  // reg_dma_mode_needs_busy is.
  d->rg_bsy_i = 1;
  d->rg_sclr_i = 0;
  d->rg_icr_clr_lo_i = 0;
  d->rg_mr_dma_clr_i = 0;
  d->rg_stb_i = 0;
  d->rg_we_i = 0;
  d->rg_dack_i = 0;
  d->rg_adr_i = 0;
  d->rg_dat_i = 0;
  d->rg_aip_i = 0;
  d->rg_la_i = 0;
  d->rg_csd_i = 0;
  d->rg_csb_i = 0;
  d->rg_idr_i = 0;
  d->rg_end_dma_i = 0;
  d->rg_drq_i = 0;
  d->rg_par_err_i = 0;
  d->rg_irq_i = 0;
  d->rg_phase_match_i = 0;
  d->rg_busy_err_i = 0;
  d->rg_atn_i = 0;
  d->rg_ack_i = 0;
  sim_->eval();
}

Env::~Env() {
  if (tfp_) {
    tfp_->flush();
    tfp_->close();
  }
  if (dut_) dut_->final();
}

void Env::power_on_reset(int cycles) {
  dut_->rst_i = 1;
  sim_->run_posedges(sysclk_, uint64_t(cycles));
  dut_->rst_i = 0;
  sim_->run_posedges(sysclk_, 2);
}

void Env::tick(int cycles) { sim_->run_posedges(sysclk_, uint64_t(cycles)); }

// ---------------------------------------------------------------------------
// The register file on its own.
//
// stb_i is one cycle wide, so each of these raises it, steps over exactly one
// rising edge and lowers it again.  Read data is combinational, so it is
// sampled while the strobe is still up.
// ---------------------------------------------------------------------------

namespace {

// Setting up just after a rising edge leaves the values stable across the next
// one, which is where the register file samples them - the same discipline the
// bus models follow.
void settle(Sim& sim, Sim::Clock* clk) { sim.run_posedges(clk, 1); }

}  // namespace

void Env::sample_strobes() {
  strobes_ = uint8_t((dut_->rg_rpi_o ? S_RPI : 0) |
                     (dut_->rg_sds_o ? S_SDS : 0) |
                     (dut_->rg_sdtr_o ? S_SDTR : 0) |
                     (dut_->rg_sdir_o ? S_SDIR : 0));
}

void Env::reg_write(uint8_t adr, uint8_t data) {
  dut_->rg_stb_i = 1;
  dut_->rg_we_i = 1;
  dut_->rg_dack_i = 0;
  dut_->rg_adr_i = adr & 7;
  dut_->rg_dat_i = data;
  sim_->eval();
  sample_strobes();
  settle(*sim_, sysclk_);
  dut_->rg_stb_i = 0;
  dut_->rg_we_i = 0;
  sim_->eval();
}

uint8_t Env::reg_read(uint8_t adr) {
  dut_->rg_stb_i = 1;
  dut_->rg_we_i = 0;
  dut_->rg_dack_i = 0;
  dut_->rg_adr_i = adr & 7;
  sim_->eval();
  sample_strobes();
  uint8_t v = dut_->rg_dat_o;
  settle(*sim_, sysclk_);
  dut_->rg_stb_i = 0;
  sim_->eval();
  return v;
}

void Env::reg_write_dack(uint8_t data) {
  dut_->rg_stb_i = 1;
  dut_->rg_we_i = 1;
  dut_->rg_dack_i = 1;
  dut_->rg_dat_i = data;
  sim_->eval();
  sample_strobes();
  settle(*sim_, sysclk_);
  dut_->rg_stb_i = 0;
  dut_->rg_we_i = 0;
  dut_->rg_dack_i = 0;
  sim_->eval();
}

// ---------------------------------------------------------------------------
// The whole chip.
// ---------------------------------------------------------------------------

void Env::chip_write(uint8_t adr, uint8_t data) {
  dut_->dut_stb_i = 1;
  dut_->dut_we_i = 1;
  dut_->dut_dack_i = 0;
  dut_->dut_adr_i = adr & 7;
  dut_->dut_dat_i = data;
  settle(*sim_, sysclk_);
  dut_->dut_stb_i = 0;
  dut_->dut_we_i = 0;
  sim_->eval();
}

uint8_t Env::chip_read(uint8_t adr) {
  dut_->dut_stb_i = 1;
  dut_->dut_we_i = 0;
  dut_->dut_dack_i = 0;
  dut_->dut_adr_i = adr & 7;
  sim_->eval();
  uint8_t v = dut_->dut_dat_o;
  settle(*sim_, sysclk_);
  dut_->dut_stb_i = 0;
  sim_->eval();
  return v;
}

void Env::chip_write_dack(uint8_t data) {
  dut_->dut_stb_i = 1;
  dut_->dut_we_i = 1;
  dut_->dut_dack_i = 1;
  dut_->dut_dat_i = data;
  settle(*sim_, sysclk_);
  dut_->dut_stb_i = 0;
  dut_->dut_we_i = 0;
  dut_->dut_dack_i = 0;
  sim_->eval();
}

uint8_t Env::chip_read_dack() {
  dut_->dut_stb_i = 1;
  dut_->dut_we_i = 0;
  dut_->dut_dack_i = 1;
  sim_->eval();
  uint8_t v = dut_->dut_dat_o;
  settle(*sim_, sysclk_);
  dut_->dut_stb_i = 0;
  dut_->dut_dack_i = 0;
  sim_->eval();
  return v;
}

// ---------------------------------------------------------------------------
// The peer device.
// ---------------------------------------------------------------------------

Env::Peer& Env::Peer::phase(uint8_t ph) {
  msg = (ph & sci::TCR_MSG) != 0;
  cd = (ph & sci::TCR_CD) != 0;
  io = (ph & sci::TCR_IO) != 0;
  return *this;
}

Env::Peer& Env::Peer::with_data(uint8_t d, bool good_parity) {
  data = d;
  // Odd parity: the bit is set when the byte has an even number of ones.
  bool odd = false;
  for (int i = 0; i < 8; i++) odd ^= ((d >> i) & 1) != 0;
  dbp = good_parity ? !odd : odd;
  return *this;
}

void Env::drive_peer(const Peer& p) {
  peer_ = p;
  dut_->pr_rst_i = p.rst;
  dut_->pr_bsy_i = p.bsy;
  dut_->pr_sel_i = p.sel;
  dut_->pr_req_i = p.req;
  dut_->pr_ack_i = p.ack;
  dut_->pr_atn_i = p.atn;
  dut_->pr_msg_i = p.msg;
  dut_->pr_cd_i = p.cd;
  dut_->pr_io_i = p.io;
  dut_->pr_data_i = p.data;
  dut_->pr_dbp_i = p.dbp;
  sim_->eval();
}

void Env::peer_set(bool Peer::*field, bool v) {
  Peer p = peer_;
  p.*field = v;
  drive_peer(p);
}

uint8_t Env::bus_csb() {
  sim_->eval();
  return uint8_t((dut_->bus_rst_o ? sci::CSB_RST : 0) |
                 (dut_->bus_bsy_o ? sci::CSB_BSY : 0) |
                 (dut_->bus_req_o ? sci::CSB_REQ : 0) |
                 (dut_->bus_msg_o ? sci::CSB_MSG : 0) |
                 (dut_->bus_cd_o ? sci::CSB_CD : 0) |
                 (dut_->bus_io_o ? sci::CSB_IO : 0) |
                 (dut_->bus_sel_o ? sci::CSB_SEL : 0) |
                 (dut_->bus_dbp_o ? sci::CSB_DBP : 0));
}

uint8_t Env::bus_data() {
  sim_->eval();
  return dut_->bus_data_o;
}

bool Env::bus_ack() {
  sim_->eval();
  return dut_->bus_ack_o != 0;
}

bool Env::bus_req() {
  sim_->eval();
  return dut_->bus_req_o != 0;
}

uint8_t Env::reg_read_dack() {
  dut_->rg_stb_i = 1;
  dut_->rg_we_i = 0;
  dut_->rg_dack_i = 1;
  sim_->eval();
  sample_strobes();
  uint8_t v = dut_->rg_dat_o;
  settle(*sim_, sysclk_);
  dut_->rg_stb_i = 0;
  dut_->rg_dack_i = 0;
  sim_->eval();
  return v;
}

}  // namespace wtb

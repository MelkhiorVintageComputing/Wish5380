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

  // The host CPU, poking the slave.
  WbMasterPorts mp;
  mp.cyc = &d->wbs_cyc_i;
  mp.stb = &d->wbs_stb_i;
  mp.we = &d->wbs_we_i;
  mp.sel = &d->wbs_sel_i;
  mp.adr = &d->wbs_adr_i;
  mp.dat_w = &d->wbs_dat_i;
  mp.dat_r = &d->wbs_dat_o;
  mp.ack = &d->wbs_ack_o;
  mp.err = &d->wbs_err_o;
  host_.reset(new WbHost(*sim_, sysclk_, mp));
  // The host model's timeout is a bus watchdog and not a driver's wait, so it
  // has to sit above the slave's own worst case: the handshaking window will
  // hold a cycle for its whole DRQ timeout before answering with ERR.
  host_->timeout_ps = 200 * US;

  // The card behind the target.
  DiskPorts dp;
  dp.start = &d->tg_blk_start_o;
  dp.we = &d->tg_blk_we_o;
  dp.lba = &d->tg_blk_lba_o;
  dp.done = &d->tg_blk_done_i;
  dp.err = &d->tg_blk_err_i;
  dp.ready = &d->tg_blk_ready_i;
  dp.count = &d->tg_blk_count_i;
  dp.buf_we = &d->tg_bbuf_we_i;
  dp.buf_addr = &d->tg_bbuf_addr_i;
  dp.buf_wdata = &d->tg_bbuf_wdata_i;
  dp.buf_rdata = &d->tg_bbuf_rdata_o;
  disk_.reset(new Disk(*sim_, sysclk_, dp, cfg_.disk_blocks));

  // The driver, reaching the chip the way a real one does.
  RegPort rp;
  rp.write = [this](uint8_t a, uint8_t v) { chip_write(a, v); };
  rp.read = [this](uint8_t a) { return chip_read(a); };
  rp.pdma_read = [this](uint8_t* buf, size_t n) {
    Pdma p = pdma_read(n, /*handshake=*/true);
    if (p.error) return false;
    for (size_t i = 0; i < n && i < p.data.size(); i++) buf[i] = p.data[i];
    return true;
  };
  rp.pdma_write = [this](const uint8_t* buf, size_t n) {
    return !pdma_write(Bytes(buf, buf + n), /*handshake=*/true).error;
  };
  drv_.reset(new SciDriver(*sim_, rp, cfg_.host_id));

  // The second instance, and the card in its slot.
  WbMasterPorts sp;
  sp.cyc = &d->sdb_cyc_i;
  sp.stb = &d->sdb_stb_i;
  sp.we = &d->sdb_we_i;
  sp.sel = &d->sdb_sel_i;
  sp.adr = &d->sdb_adr_i;
  sp.dat_w = &d->sdb_dat_i;
  sp.dat_r = &d->sdb_dat_o;
  sp.ack = &d->sdb_ack_o;
  sp.err = &d->sdb_err_o;
  sd_host_.reset(new WbHost(*sim_, sysclk_, sp));
  sd_host_->timeout_ps = 200 * US;

  SdPorts cp;
  cp.sclk = &d->sd_clk_o;
  cp.cs_n = &d->sd_cs_n_o;
  cp.mosi = &d->sd_mosi_o;
  cp.miso = &d->sd_miso_i;
  card_.reset(new SdCard(*sim_, sysclk_, cp, cfg_.sd_blocks));

  RegPort sp2;
  sp2.write = [this](uint8_t a, uint8_t v) { sd_chip_write(a, v); };
  sp2.read = [this](uint8_t a) { return sd_chip_read(a); };
  sp2.pdma_read = [](uint8_t*, size_t) { return false; };
  sp2.pdma_write = [](const uint8_t*, size_t) { return false; };
  sd_drv_.reset(new SciDriver(*sim_, sp2, cfg_.host_id));

  sim_->eval();
}

void Env::sd_chip_write(uint8_t adr, uint8_t data) {
  uint32_t ba = cfg_.reg_base + uint32_t(adr & 7) * cfg_.reg_stride;
  uint8_t lane = ba & 3;
  sd_host_->write32(ba, uint32_t(data) << (8 * lane), uint8_t(1u << lane));
}

uint8_t Env::sd_chip_read(uint8_t adr) {
  uint32_t ba = cfg_.reg_base + uint32_t(adr & 7) * cfg_.reg_stride;
  uint8_t lane = ba & 3;
  uint32_t v = sd_host_->read32(ba, uint8_t(1u << lane));
  return uint8_t((v >> (8 * lane)) & 0xff);
}

bool Env::wait_card_ready(u64 timeout_ps) {
  u64 t0 = sim_->time_ps();
  while (sim_->time_ps() - t0 < timeout_ps) {
    SciDriver::Result r =
        sd_drv_->execute(cfg_.target_id, Bytes{0x00, 0, 0, 0, 0, 0});
    if (r.ok && r.status == 0x00) return true;
    // Twenty microseconds a poll: a whole SCSI command costs about twelve, so
    // polling any harder spends more time asking than waiting.
    tick(1000);
  }
  return false;
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

// Byte address of register `adr`, and which lane of its word it lands in.
// The lane is the low two bits of the byte address, which is the Wishbone
// little-endian convention `wb_5380` decodes against.
void Env::chip_write(uint8_t adr, uint8_t data) {
  uint32_t ba = cfg_.reg_base + uint32_t(adr & 7) * cfg_.reg_stride;
  uint8_t lane = ba & 3;
  host_->write32(ba, uint32_t(data) << (8 * lane), uint8_t(1u << lane));
}

uint8_t Env::chip_read(uint8_t adr) {
  uint32_t ba = cfg_.reg_base + uint32_t(adr & 7) * cfg_.reg_stride;
  uint8_t lane = ba & 3;
  uint32_t v = host_->read32(ba, uint8_t(1u << lane));
  return uint8_t((v >> (8 * lane)) & 0xff);
}

void Env::chip_write_dack(uint8_t data) {
  Pdma p = pdma_write(Bytes{data}, /*handshake=*/false);
  (void)p;
}

uint8_t Env::chip_read_dack() {
  Pdma p = pdma_read(1, /*handshake=*/false);
  return p.data.empty() ? 0 : p.data[0];
}

Env::Pdma Env::pdma_read(size_t n, bool handshake) {
  Pdma out;
  uint32_t ba = handshake ? cfg_.hsk_base : cfg_.dma_base;
  uint8_t sel = uint8_t((1u << n) - 1);
  uint32_t v = host_->read32(ba, sel);
  out.error = host_->last_error() || host_->last_timeout();
  if (!out.error) {
    for (size_t i = 0; i < n; i++) out.data.push_back(uint8_t(v >> (8 * i)));
  }
  return out;
}

Env::Pdma Env::pdma_write(const Bytes& b, bool handshake) {
  Pdma out;
  uint32_t ba = handshake ? cfg_.hsk_base : cfg_.dma_base;
  uint8_t sel = uint8_t((1u << b.size()) - 1);
  uint32_t v = 0;
  for (size_t i = 0; i < b.size(); i++) v |= uint32_t(b[i]) << (8 * i);
  host_->write32(ba, v, sel);
  out.error = host_->last_error() || host_->last_timeout();
  return out;
}

bool Env::wb_err_on_read(uint32_t byte_adr, uint8_t sel) {
  (void)host_->read32(byte_adr, sel);
  return host_->last_error() || host_->last_timeout();
}

bool Env::wb_err_on_write(uint32_t byte_adr, uint8_t sel, uint32_t data) {
  host_->write32(byte_adr, data, sel);
  return host_->last_error() || host_->last_timeout();
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

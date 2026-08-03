// SPDX-License-Identifier: MIT
//
// The Verilated wish5380 behind a C interface an emulated card can dlopen.
//
// Everything inside is reused from `tb/cpp` - the same kernel, the same
// Wishbone host model, the same SD card.  Nothing test-specific comes with
// them, which is the point: a co-simulation with its own bus model would drift
// from the regression and stop saying anything about it.

#include "wish_rtl.h"

#include <verilated.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "Vrtl_top.h"
#include "sd_card.h"
#include "sim.h"
#include "util.h"
#include "wb.h"

using namespace wtb;

namespace {

// The system clock the core is built for, matching rtl_top.sv's parameter.
constexpr u64 PERIOD_PS = 20000;

// How long to give the card before deciding there is not one.  A real card
// may take most of a second; this is the co-simulation's patience, and a card
// model that took longer would be modelling a card nobody would ship.
constexpr u64 CARD_TIMEOUT_PS = 60 * MS;

bool trace_on() {
  const char* e = getenv("WISH_RTL_TRACE");
  return e && *e && *e != '0';
}

}  // namespace

struct WishRtl {
  std::unique_ptr<VerilatedContext> ctx;
  std::unique_ptr<Vrtl_top> dut;
  std::unique_ptr<Sim> sim;
  Sim::Clock* clk = nullptr;
  std::unique_ptr<WbHost> host;
  std::unique_ptr<SdCard> card;
  std::string image;
  uint32_t blocks = 0;
  bool ready = false;
  bool trace = false;
};

extern "C" uint32_t wish_rtl_abi(void) { return WISH_RTL_ABI; }

extern "C" WishRtl* wish_rtl_new(const char* image, uint32_t blocks) {
  WishRtl* r = new WishRtl();
  r->trace = trace_on();
  r->blocks = blocks ? blocks : 4096;
  if (image) r->image = image;

  r->ctx.reset(new VerilatedContext());
  r->ctx->timeunit(-12);
  r->ctx->timeprecision(-12);
  r->dut.reset(new Vrtl_top(r->ctx.get(), "wish5380"));
  r->sim.reset(new Sim([r]() { r->dut->eval(); }));
  r->clk = r->sim->add_clock(&r->dut->clk, PERIOD_PS, "clk");

  WbMasterPorts mp;
  mp.cyc = &r->dut->wb_cyc_i;
  mp.stb = &r->dut->wb_stb_i;
  mp.we = &r->dut->wb_we_i;
  mp.sel = &r->dut->wb_sel_i;
  mp.adr = &r->dut->wb_adr_i;
  mp.dat_w = &r->dut->wb_dat_i;
  mp.dat_r = &r->dut->wb_dat_o;
  mp.ack = &r->dut->wb_ack_o;
  mp.err = &r->dut->wb_err_o;
  r->host.reset(new WbHost(*r->sim, r->clk, mp));
  r->host->timeout_ps = 1 * MS;

  SdPorts cp;
  cp.sclk = &r->dut->sd_clk_o;
  cp.cs_n = &r->dut->sd_cs_n_o;
  cp.mosi = &r->dut->sd_mosi_o;
  cp.miso = &r->dut->sd_miso_i;
  r->card.reset(new SdCard(*r->sim, r->clk, cp, r->blocks));

  // Load the image, one block at a time, so a short file simply leaves the
  // rest of the card blank.
  if (!r->image.empty()) {
    FILE* f = fopen(r->image.c_str(), "rb");
    if (f) {
      Bytes buf(SdCard::BLOCK);
      for (uint32_t b = 0; b < r->blocks; b++) {
        size_t n = fread(buf.data(), 1, SdCard::BLOCK, f);
        if (n == 0) break;
        if (n < SdCard::BLOCK) memset(buf.data() + n, 0, SdCard::BLOCK - n);
        r->card->write_block(b, buf);
      }
      fclose(f);
    }
  }

  r->dut->rst = 1;
  r->sim->eval();
  return r;
}

extern "C" void wish_rtl_flush(WishRtl* r) {
  if (!r || r->image.empty()) return;
  FILE* f = fopen(r->image.c_str(), "r+b");
  if (!f) f = fopen(r->image.c_str(), "wb");
  if (!f) return;
  for (uint32_t b = 0; b < r->blocks; b++) {
    Bytes blk = r->card->read_block(b);
    fwrite(blk.data(), 1, blk.size(), f);
  }
  fclose(f);
}

extern "C" void wish_rtl_free(WishRtl* r) {
  if (!r) return;
  wish_rtl_flush(r);
  if (r->dut) r->dut->final();
  delete r;
}

// Byte address of a register.  rtl_top builds the card with a stride of one,
// which is the generic ISA layout the guest's driver expects.
static uint32_t reg_addr(int reg) { return uint32_t(reg & 7); }

extern "C" void wish_rtl_write(WishRtl* r, int reg, uint8_t val) {
  uint32_t ba = reg_addr(reg);
  uint8_t lane = ba & 3;
  r->host->write32(ba, uint32_t(val) << (8 * lane), uint8_t(1u << lane));
  if (r->trace) {
    fprintf(stderr, "[wish5380 %8llu ns] w reg%d <- %02x%s\n",
            (unsigned long long)(r->sim->time_ps() / 1000), reg & 7, val,
            r->host->last_error() ? "  ERR" : "");
  }
}

extern "C" uint8_t wish_rtl_read(WishRtl* r, int reg) {
  uint32_t ba = reg_addr(reg);
  uint8_t lane = ba & 3;
  uint32_t v = r->host->read32(ba, uint8_t(1u << lane));
  uint8_t b = uint8_t((v >> (8 * lane)) & 0xff);
  if (r->trace) {
    fprintf(stderr, "[wish5380 %8llu ns] r reg%d -> %02x%s\n",
            (unsigned long long)(r->sim->time_ps() / 1000), reg & 7, b,
            r->host->last_error() ? "  ERR" : "");
  }
  return b;
}

extern "C" int wish_rtl_irq(WishRtl* r) {
  r->sim->eval();
  return r->dut->irq_o ? 1 : 0;
}

extern "C" void wish_rtl_run_ns(WishRtl* r, uint64_t ns) {
  r->sim->run_ps(ns * 1000);
}

extern "C" uint64_t wish_rtl_time_ns(WishRtl* r) {
  return r->sim->time_ps() / 1000;
}

extern "C" uint32_t wish_rtl_blocks(WishRtl* r) { return r->blocks; }

extern "C" int wish_rtl_ready(WishRtl* r) { return r->ready ? 1 : 0; }

extern "C" int wish_rtl_reset(WishRtl* r) {
  r->dut->rst = 1;
  r->sim->run_posedges(r->clk, 8);
  r->dut->rst = 0;
  r->sim->run_posedges(r->clk, 4);
  r->ready = false;

  // Run until the card is up.  This is time the guest never sees: on a real
  // machine the card finishes initialising while the firmware is still
  // counting memory, and a driver that probed before then would find a drive
  // reporting NOT READY - which is a legitimate answer, and one the guest's
  // own retry logic would eventually get past, but only after making this
  // co-simulation look broken for reasons that have nothing to do with SCSI.
  u64 t0 = r->sim->time_ps();
  while (r->sim->time_ps() - t0 < CARD_TIMEOUT_PS) {
    r->sim->run_ps(200 * US);
    if (r->card->initialised()) {
      // The card is up; give the controller time to read the CSD and settle.
      r->sim->run_ps(2 * MS);
      r->ready = true;
      break;
    }
  }

  if (r->trace) {
    fprintf(stderr, "[wish5380] reset: card %s after %llu us\n",
            r->ready ? "up" : "DID NOT COME UP",
            (unsigned long long)((r->sim->time_ps() - t0) / 1000000));
  }
  return r->ready ? 1 : 0;
}

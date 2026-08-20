// SPDX-License-Identifier: MIT
//
// The SD back end against a card model that is not ours.
//
// `tb/cpp/sd_card.cpp` was written alongside `src/blk_sd.sv`, from one reading
// of the specification, and answers exactly the commands `blk_sd` sends - its
// own header says so.  That is a good model and a poor witness: a misreading
// that landed in both would cancel out, and no test in the regression could
// see it.  The SCSI side of this design has four drivers, a second
// implementation and two differential harnesses to stop the same thing
// happening there; this is the SD side's first.
//
// The other card is `SDSPISIM`, from ZipCPU's sdspi.  It was written against a
// different controller, it knows commands we never send, and its author's
// controller runs on real hardware.  It is GPLv3, which is why it is cloned
// into `work/` by `cosim/scripts/build-sdspi.sh` and never lives here; nothing
// in `src/` or `tb/` may depend on any of this.
//
// Two things run here.  `--mode swap` drives the whole back end against
// whichever card is asked for, and says whether it comes up, reads and writes.
// `--mode diff` runs both cards from the same wire at once, ours driving MISO
// and the other one shadowing it, and compares what each would have said.
//
// The comparison is of the *decoded* byte stream with runs of 0xFF collapsed,
// not of MISO bit by bit.  N(CR), the idle gap before a response, is a window
// and not a number - Linux calls it "1..8 bytes of all-ones"
// (doc/drivers/SD/Linux/mmc_spi.c:428) - so two conformant cards differ there
// without either being wrong, and a harness that compared it would report
// noise.  Everything that carries information must match exactly.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "Vsdcheck_top.h"
#include "verilated.h"

#include "sim.h"
#include "util.h"
#include "sd_card.h"

#include "sdspisim.h"

using namespace wtb;

namespace {

constexpr u64 PERIOD_PS = 20000;      // 50 MHz, as the regression's default
constexpr size_t BLOCK = 512;

int g_failed = 0;
int g_passed = 0;

void ok(const std::string& what) {
  printf("  ok    %s\n", what.c_str());
  g_passed++;
}
void bad(const std::string& what, const std::string& why) {
  printf("  FAIL  %s\n        %s\n", what.c_str(), why.c_str());
  g_failed++;
}
#define CHECK_OK(cond, what, why) \
  do { if (cond) ok(what); else bad(what, why); } while (0)

// ---------------------------------------------------------------------------
// ZipCPU's card, as a model of this testbench's shape.
//
// It attaches on the negative edge like everything else here: at a negedge the
// outputs the RTL produced at the preceding posedge are stable, and what this
// drives takes effect at the next one.  SDSPISIM finds the card clock's edges
// itself from the level it is handed, which is the same bargain `wtb::SdCard`
// makes, so it wants calling once per system clock and no faster.
// ---------------------------------------------------------------------------

class ZipCard {
 public:
  ZipCard(Sim& sim, Sim::Clock* clk, SdPorts p, const char* image,
          bool drive_miso)
      : ports_(p), drive_miso_(drive_miso) {
    card_.load(image);
    sim.on_negedge(clk, [this]() { tick(); });
  }

  // What it would have driven, whether or not anyone is listening.
  uint8_t miso() const { return last_miso_; }

 private:
  void tick() {
    int csn = *ports_.cs_n ? 1 : 0;
    int sck = *ports_.sclk ? 1 : 0;
    int mosi = *ports_.mosi ? 1 : 0;
    last_miso_ = uint8_t(card_(csn, sck, mosi) ? 1 : 0);
    if (drive_miso_) *ports_.miso = last_miso_;
  }

  SDSPISIM card_;
  SdPorts ports_;
  bool drive_miso_;
  uint8_t last_miso_ = 1;
};

// ---------------------------------------------------------------------------
// What both cards can be asked, which is a file on disk and a block interface.
// ---------------------------------------------------------------------------

class Harness {
 public:
  explicit Harness(bool trace = false) {
    dut_.reset(new Vsdcheck_top);
    sim_.reset(new Sim([this]() { dut_->eval(); }));
    clk_ = sim_->add_clock(&dut_->clk, PERIOD_PS, "clk");
    (void)trace;

    ports_.sclk = &dut_->sd_clk_o;
    ports_.cs_n = &dut_->sd_cs_n_o;
    ports_.mosi = &dut_->sd_mosi_o;
    ports_.miso = &dut_->sd_miso_i;

    // The sector buffer lives in the target in the real design, so it lives
    // here.  buf_rdata answers buf_addr one cycle late, which is the contract
    // doc/block.md states and the thing a back end is entitled to assume.
    sim_->on_negedge(clk_, [this]() {
      if (dut_->blk_buf_we_o) buf_[dut_->blk_buf_addr_o] = dut_->blk_buf_wdata_o;
      dut_->blk_buf_rdata_i = buf_[dut_->blk_buf_addr_o];
    });

    // The power-up clocks, counted rather than assumed.  A card needs at
    // least 74 of them with the card deselected and the line high before it
    // will look at a command; FatFs sends eighty and calls them "dummy
    // clocks" (doc/drivers/SD/FatFs/sdmm.c:406).  Nothing else here counts
    // them, and a controller that sent too few would come up against a
    // forgiving model and fail against a card.
    sim_->on_negedge(clk_, [this]() {
      uint8_t sclk = dut_->sd_clk_o;
      bool rising = sclk && !last_sclk_;
      last_sclk_ = sclk;
      if (!first_select_seen_) {
        if (!dut_->sd_cs_n_o) first_select_seen_ = true;
        else if (rising) powerup_clocks_++;
      }
    });
  }

  unsigned powerup_clocks() const { return powerup_clocks_; }

  Sim& sim() { return *sim_; }
  Sim::Clock* clk() { return clk_; }
  SdPorts ports() const { return ports_; }
  Vsdcheck_top& dut() { return *dut_; }
  uint8_t* buf() { return buf_; }

  void power_on_reset() {
    dut_->rst = 1;
    dut_->blk_start_i = 0;
    dut_->blk_we_i = 0;
    dut_->blk_lba_i = 0;
    dut_->blk_buf_rdata_i = 0;
    sim_->run_cycles(clk_, 10);
    dut_->rst = 0;
    // Count from here: before this the model has not been evaluated and its
    // outputs are whatever Verilator zeroed them to, which is not the card
    // being selected.
    powerup_clocks_ = 0;
    first_select_seen_ = false;
    last_sclk_ = dut_->sd_clk_o;
    sim_->run_cycles(clk_, 2);
  }

  bool wait_ready(u64 timeout_ps = 3000 * MS) {
    return sim_->run_until([this]() { return dut_->blk_ready_o != 0; },
                           timeout_ps);
  }

  // One transfer, the way `scsi_targ` starts one.
  bool transfer(bool we, uint32_t lba, u64 timeout_ps = 1000 * MS) {
    dut_->blk_lba_i = lba;
    dut_->blk_we_i = we ? 1 : 0;
    dut_->blk_start_i = 1;
    sim_->run_cycles(clk_, 1);
    dut_->blk_start_i = 0;
    bool done = sim_->run_until([this]() { return dut_->blk_done_o != 0; },
                                timeout_ps);
    err_ = dut_->blk_err_o != 0;
    return done;
  }

  bool err() const { return err_; }

 private:
  std::unique_ptr<Vsdcheck_top> dut_;
  std::unique_ptr<Sim> sim_;
  Sim::Clock* clk_ = nullptr;
  SdPorts ports_;
  uint8_t buf_[BLOCK] = {0};
  bool err_ = false;
  uint8_t last_sclk_ = 0;
  unsigned powerup_clocks_ = 0;
  bool first_select_seen_ = false;
};

// A card image with a recognisable pattern in every block.
void make_image(const std::string& path, uint32_t blocks) {
  std::vector<uint8_t> img(size_t(blocks) * BLOCK);
  for (uint32_t b = 0; b < blocks; b++) {
    for (size_t i = 0; i < BLOCK; i++) {
      img[size_t(b) * BLOCK + i] = uint8_t((b * 7 + i * 3 + (i >> 5)) & 0xff);
    }
  }
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) { perror(path.c_str()); exit(2); }
  fwrite(img.data(), 1, img.size(), f);
  fclose(f);
}

std::vector<uint8_t> read_image_block(const std::string& path, uint32_t lba) {
  std::vector<uint8_t> out(BLOCK, 0);
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return out;
  fseek(f, long(size_t(lba) * BLOCK), SEEK_SET);
  if (fread(out.data(), 1, BLOCK, f) != BLOCK) out.assign(BLOCK, 0);
  fclose(f);
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Mode 1: substitution.  Whichever card is in the slot, does the back end
// come up, does it agree about the capacity, and do blocks survive the round
// trip?
// ---------------------------------------------------------------------------

namespace {

int run_swap(const std::string& which, const std::string& image,
             uint32_t blocks) {
  printf("\n%s: blk_sd against %s\n\n", "swap",
         which == "zip" ? "ZipCPU's SDSPISIM" : "our own wtb::SdCard");

  make_image(image, blocks);
  Harness h;

  std::unique_ptr<ZipCard> zip;
  std::unique_ptr<SdCard> ours;
  if (which == "zip") {
    zip.reset(new ZipCard(h.sim(), h.clk(), h.ports(), image.c_str(), true));
  } else {
    ours.reset(new SdCard(h.sim(), h.clk(), h.ports(), blocks));
    // Give it the same contents the file has, so both modes check the same
    // thing rather than each checking its own idea of a disk.
    for (uint32_t b = 0; b < blocks; b++) {
      std::vector<uint8_t> v = read_image_block(image, b);
      ours->write_block(b, Bytes(v.begin(), v.end()));
    }
  }

  h.power_on_reset();

  CHECK_OK(h.wait_ready(), "the card comes up",
           "blk_ready_o never rose: the initialisation sequence did not "
           "complete against this card");
  if (g_failed) return 1;

  // The power-up clocks, now that they have all gone out.
  CHECK_OK(h.powerup_clocks() >= 74,
           "at least 74 clocks before the card is selected",
           "only " + std::to_string(h.powerup_clocks()) +
               " clocks went out with the card deselected, where a card is "
               "entitled to ignore everything until it has seen 74");

  // The capacity travelled from the card's own CSD through blk_sd's capacity
  // arithmetic.  Against our model that checks the arithmetic; against
  // ZipCPU's it checks the arithmetic *and* our reading of the CSD layout,
  // because the CSD was built by someone else.
  uint32_t count = h.dut().blk_count_o;
  CHECK_OK(count == blocks, "the capacity is right",
           "blk_count_o is " + std::to_string(count) + ", the image is " +
               std::to_string(blocks) + " blocks");

  // A read, compared against the file rather than against the model.
  const uint32_t lba = 17;
  std::vector<uint8_t> want = read_image_block(image, lba);
  memset(h.buf(), 0, BLOCK);
  bool done = h.transfer(false, lba);
  bool match = done && !h.err() && memcmp(h.buf(), want.data(), BLOCK) == 0;
  CHECK_OK(match, "a known block reads back",
           done ? (h.err() ? "the transfer reported an error"
                           : "the bytes differ from the image file")
                : "the transfer never finished");

  // A write, checked in the file afterwards.  Both cards are file- or
  // memory-backed, so the check is the same shape either way.
  const uint32_t wlba = 42;
  std::vector<uint8_t> payload(BLOCK);
  for (size_t i = 0; i < BLOCK; i++) payload[i] = uint8_t(0xA0 ^ (i * 5));
  memcpy(h.buf(), payload.data(), BLOCK);
  done = h.transfer(true, wlba);
  bool wrote = done && !h.err();
  if (wrote) {
    if (which == "zip") {
      // SDSPISIM writes through its FILE*, so it has to be flushed before the
      // host can look.  Reading it back through blk_sd is the stronger check
      // in any case, and is what the next one does.
      memset(h.buf(), 0, BLOCK);
      done = h.transfer(false, wlba);
      wrote = done && !h.err() && memcmp(h.buf(), payload.data(), BLOCK) == 0;
    } else {
      Bytes got = ours->read_block(wlba);
      wrote = got.size() == BLOCK &&
              memcmp(got.data(), payload.data(), BLOCK) == 0;
    }
  }
  CHECK_OK(wrote, "a written block comes back",
           "what was written did not read back the same");

  // Several in a row: the ready-state loop, not just the first transfer.
  bool all = true;
  for (uint32_t i = 0; i < 4 && all; i++) {
    std::vector<uint8_t> w = read_image_block(image, 100 + i);
    memset(h.buf(), 0, BLOCK);
    all = h.transfer(false, 100 + i) && !h.err() &&
          memcmp(h.buf(), w.data(), BLOCK) == 0;
  }
  CHECK_OK(all, "several blocks in a row", "one of four transfers went wrong");

  return g_failed ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Mode 2: the differential.
//
// The obvious arrangement - both cards on one wire, ours driving MISO and the
// other shadowing - does not work, and the reason is worth recording because
// it looks like it should.  Only one card can drive MISO, so only one of them
// is ever *answered*; the other watches commands go past and never learns that
// its own ACMD41 was accepted.  It falls behind, and the first command that
// depends on the card being out of idle - CMD9 - arrives while the shadow
// still thinks it is initialising.  ZipCPU's model says so with an assert.
//
// So the host is run twice, once against each card, and what is compared is
// the transcript: for every command, the index and argument that went out and
// the answer that came back.  That makes the stimulus identical at the level
// that matters - the driver is the same state machine both times - and it
// removes the timing noise for free, because N(CR) is the gap *between* the
// things recorded rather than one of them.
//
// What is recorded per command:
//   * the six command bytes, from MOSI;
//   * R1, the first answer byte with bit 7 clear;
//   * the four bytes after it for CMD8 and CMD58, which are defined to follow
//     R1 immediately and carry the version echo and the OCR;
//   * for a command that returns data, the start token and a CRC over the
//     block, so a difference in the CSD or in a sector shows up as one line
//     rather than five hundred.
// ---------------------------------------------------------------------------


struct Entry {
  uint8_t cmd = 0;
  uint32_t arg = 0;
  uint8_t r1 = 0xff;
  bool have_tail = false;
  uint8_t tail[4] = {0, 0, 0, 0};
  bool have_data = false;
  uint8_t token = 0;
  uint16_t data_crc = 0;

  std::string str() const {
    char b[192];
    int n = snprintf(b, sizeof b, "CMD%-2u arg=0x%08x -> R1=0x%02x", cmd, arg,
                     r1);
    if (have_tail)
      n += snprintf(b + n, sizeof b - size_t(n), " tail=%02x%02x%02x%02x",
                    tail[0], tail[1], tail[2], tail[3]);
    if (have_data)
      snprintf(b + n, sizeof b - size_t(n), " token=0x%02x data_crc=0x%04x",
               token, data_crc);
    return b;
  }
  bool operator==(const Entry& o) const {
    if (cmd != o.cmd || arg != o.arg || r1 != o.r1) return false;
    if (have_tail != o.have_tail) return false;
    if (have_tail && memcmp(tail, o.tail, 4) != 0) return false;
    if (have_data != o.have_data) return false;
    if (have_data && (token != o.token || data_crc != o.data_crc)) return false;
    return true;
  }
};

// CRC16-CCITT, the same one the card computes over a data block.  Used here
// only to say "these five hundred bytes differ" in one line.
uint16_t crc16(const uint8_t* p, size_t n) {
  uint16_t c = 0;
  for (size_t i = 0; i < n; i++) {
    c = uint16_t(c ^ (uint16_t(p[i]) << 8));
    for (int b = 0; b < 8; b++)
      c = uint16_t((c & 0x8000) ? ((c << 1) ^ 0x1021) : (c << 1));
  }
  return c;
}

class Transcript {
 public:
  Transcript(Sim& sim, Sim::Clock* clk, SdPorts p) : ports_(p) {
    sim.on_negedge(clk, [this]() { sample(); });
  }
  const std::vector<Entry>& entries() const { return e_; }

 private:
  enum { IDLE, CMD, WAIT_R1, TAIL, WAIT_TOKEN, DATA };

  void sample() {
    uint8_t sclk = *ports_.sclk;
    bool rising = sclk && !last_sclk_;
    last_sclk_ = sclk;
    if (*ports_.cs_n) { bits_ = 0; return; }
    if (!rising) return;

    mo_ = uint8_t((mo_ << 1) | (*ports_.mosi ? 1 : 0));
    mi_ = uint8_t((mi_ << 1) | (*ports_.miso ? 1 : 0));
    if (++bits_ < 8) return;
    bits_ = 0;
    byte(mo_, mi_);
    mo_ = mi_ = 0;
  }

  void byte(uint8_t out, uint8_t in) {
    switch (st_) {
      case IDLE:
        if ((out & 0xc0) == 0x40) { buf_[0] = out; n_ = 1; st_ = CMD; }
        return;
      case CMD:
        buf_[n_++] = out;
        if (n_ == 6) {
          cur_ = Entry();
          cur_.cmd = uint8_t(buf_[0] & 0x3f);
          cur_.arg = (uint32_t(buf_[1]) << 24) | (uint32_t(buf_[2]) << 16) |
                     (uint32_t(buf_[3]) << 8) | buf_[4];
          st_ = WAIT_R1;
        }
        return;
      case WAIT_R1:
        if (in & 0x80) return;              // still N(CR): not recorded
        cur_.r1 = in;
        n_ = 0;
        if (cur_.cmd == 8 || cur_.cmd == 58) { cur_.have_tail = true; st_ = TAIL; }
        else if (cur_.cmd == 9 || cur_.cmd == 17) st_ = WAIT_TOKEN;
        else finish();
        return;
      case TAIL:
        cur_.tail[n_++] = in;
        if (n_ == 4) {
          if (cur_.cmd == 9 || cur_.cmd == 17) { n_ = 0; st_ = WAIT_TOKEN; }
          else finish();
        }
        return;
      case WAIT_TOKEN:
        if (in == 0xff) return;             // the card has not started yet
        cur_.have_data = true;
        cur_.token = in;
        n_ = 0;
        st_ = (in == 0xfe) ? DATA : IDLE;
        if (st_ == IDLE) finish();
        return;
      case DATA:
        data_[n_++] = in;
        // The CSD is 16 bytes, a sector is 512; both are followed by two CRC
        // bytes, which are not included because they are a function of what
        // is already compared.
        if (n_ == (cur_.cmd == 9 ? 16u : 512u)) {
          cur_.data_crc = crc16(data_, n_);
          finish();
        }
        return;
    }
  }

  void finish() {
    e_.push_back(cur_);
    st_ = IDLE;
    n_ = 0;
  }

  SdPorts ports_;
  std::vector<Entry> e_;
  Entry cur_;
  uint8_t last_sclk_ = 0, mo_ = 0, mi_ = 0;
  unsigned bits_ = 0, n_ = 0, st_ = IDLE;
  uint8_t buf_[8] = {0};
  uint8_t data_[512] = {0};
};

// One run of the same driver against one card, returning what was said.
std::vector<Entry> run_one(const std::string& which, const std::string& image,
                           uint32_t blocks) {
  Harness h;
  std::unique_ptr<ZipCard> zip;
  std::unique_ptr<SdCard> ours;
  if (which == "zip") {
    zip.reset(new ZipCard(h.sim(), h.clk(), h.ports(), image.c_str(), true));
  } else {
    ours.reset(new SdCard(h.sim(), h.clk(), h.ports(), blocks));
    for (uint32_t b = 0; b < blocks; b++) {
      std::vector<uint8_t> v = read_image_block(image, b);
      ours->write_block(b, Bytes(v.begin(), v.end()));
    }
  }
  Transcript t(h.sim(), h.clk(), h.ports());

  h.power_on_reset();
  if (!h.wait_ready()) {
    printf("  (%s never came up)\n", which.c_str());
    return t.entries();
  }
  for (uint32_t i = 0; i < 3; i++) {
    memset(h.buf(), 0, BLOCK);
    h.transfer(false, i);
  }
  return t.entries();
}

// How many times a card says "still initialising" is the card's own affair.
// ACMD41 is polled until it stops reporting idle and the specification allows
// a card most of a second to get there, so the *number* of CMD55/ACMD41 pairs
// is not a thing two conformant cards have to agree about - our model answers
// idle three times and ZipCPU's has its own counter.  The polling iterations
// are dropped and counted; the pair that ends the loop is kept, because
// whether the card ever leaves idle is very much a thing to agree about.
//
// This is the same kind of exclusion as N(CR), and the same rule applies: the
// gap is not compared, what happens either side of it is.
std::vector<Entry> without_polling(const std::vector<Entry>& in, size_t* polls) {
  std::vector<Entry> out;
  *polls = 0;
  for (size_t i = 0; i < in.size(); i++) {
    // A polling iteration is the pair, not either half: the CMD55 that
    // precedes an ACMD41 which answers "still idle".  Dropping the halves
    // separately on their own R1 would keep every CMD55 from a card that
    // answers it differently, and turn one disagreement into fourteen.
    if (in[i].cmd == 55 && i + 1 < in.size() && in[i + 1].cmd == 41 &&
        in[i + 1].r1 == 0x01) {
      *polls += 2;
      i++;
      continue;
    }
    out.push_back(in[i]);
  }
  return out;
}

int run_diff(const std::string& image, uint32_t blocks, bool verbose) {
  printf("\ndiff: the same driver against both cards, every command and "
         "answer compared\n\n");

  make_image(image, blocks);
  std::vector<Entry> raw_a = run_one("ours", image, blocks);
  make_image(image, blocks);          // the write above must not carry over
  std::vector<Entry> raw_b = run_one("zip", image, blocks);

  size_t polls_a = 0, polls_b = 0;
  std::vector<Entry> a = without_polling(raw_a, &polls_a);
  std::vector<Entry> b = without_polling(raw_b, &polls_b);

  // Two models agreeing about nothing is worth nothing.  Both of the 5380
  // harnesses guard against this and the first version of one of them passed
  // vacuously, so the guard comes before the comparison.
  const size_t MIN = 8;
  CHECK_OK(a.size() >= MIN, "the comparison is not vacuous",
           "only " + std::to_string(a.size()) +
               " commands were seen against our own card, expected at least " +
               std::to_string(MIN));

  size_t n = a.size() < b.size() ? a.size() : b.size();

  // The same driver ran both times, so it must have asked the same questions
  // in the same order.  A difference here is a difference in what a card made
  // the host do, which is the strongest kind there is.
  bool shape = (a.size() == b.size());
  for (size_t i = 0; i < n && shape; i++)
    shape = (a[i].cmd == b[i].cmd) && (a[i].arg == b[i].arg);
  CHECK_OK(shape, "the same commands in the same order",
           "the driver took a different path against the two cards");

  // The data path is the claim this harness exists to make: a sector read
  // through blk_sd must be the same sector whichever card answered.  Nothing
  // about a card's identity licenses a difference here.
  bool data = true;
  std::string data_why;
  for (size_t i = 0; i < n && data; i++) {
    if (a[i].cmd != 17) continue;
    if (a[i].have_data != b[i].have_data || a[i].token != b[i].token ||
        a[i].data_crc != b[i].data_crc) {
      data = false;
      data_why = "CMD17 arg=" + std::to_string(a[i].arg) + ": ours token 0x" +
                 std::to_string(a[i].token) + " crc 0x" +
                 std::to_string(a[i].data_crc) + ", theirs token 0x" +
                 std::to_string(b[i].token) + " crc 0x" +
                 std::to_string(b[i].data_crc);
    }
  }
  CHECK_OK(data, "every sector reads back the same from either card",
           data_why);

  // What is left is card identity - the CSD's manufacturer and timing fields,
  // the OCR's voltage window - and the places where the two models read the
  // specification differently.  Neither is a reason to fail; both are a reason
  // to look, so they are listed and cosim/README.md records the verdict on
  // each.
  size_t first = n;
  for (size_t i = 0; i < n; i++) if (!(a[i] == b[i])) { first = i; break; }
  bool same = shape && (first == n);
  std::string why;
  if (first != n) why = std::to_string(n - 0) + " commands, first difference at " +
                        std::to_string(first);
  if (!same)
    printf("  note  the two cards differ in what they say about themselves; "
           "see below\n");

  if (verbose || !same) {
    size_t m = a.size() > b.size() ? a.size() : b.size();
    printf("\n  %-58s  %s\n", "ours", "theirs");
    for (size_t i = 0; i < m; i++) {
      std::string l = i < a.size() ? a[i].str() : std::string("-");
      std::string r = i < b.size() ? b[i].str() : std::string("-");
      bool d = !(i < a.size() && i < b.size() && a[i] == b[i]);
      printf("  %-58s  %-58s%s\n", l.c_str(), r.c_str(), d ? "  <--" : "");
    }
  }

  printf("\n%zu commands compared, answer for answer; %zu and %zu ACMD41 "
         "polls dropped\n", a.size(), polls_a, polls_b);
  return g_failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);

  std::string mode = "swap", card = "zip";
  std::string image = "work/images/sdcheck.img";
  uint32_t blocks = 2048;
  bool verbose = false;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--mode" && i + 1 < argc) mode = argv[++i];
    else if (a == "--card" && i + 1 < argc) card = argv[++i];
    else if (a == "--image" && i + 1 < argc) image = argv[++i];
    else if (a == "--blocks" && i + 1 < argc) blocks = uint32_t(atoi(argv[++i]));
    else if (a == "-v") verbose = true;
    else if (a == "-h" || a == "--help") {
      printf("usage: sdcheck [--mode swap|diff] [--card zip|ours] "
             "[--image F] [--blocks N] [-v]\n");
      return 0;
    }
  }

  int rc = (mode == "diff") ? run_diff(image, blocks, verbose)
                            : run_swap(card, image, blocks);
  printf("\n%d passed, %d failed\n", g_passed, g_failed);
  return rc;
}

// SPDX-License-Identifier: MIT
//
// Drive the shared library the way a driver does, with no emulator in the
// loop.
//
// This is the thing to run first when the guest misbehaves.  Between the
// driver, the emulated card, this library and the RTL there are four places a
// fault can be; here there are two, and the sequences that would otherwise be
// buried inside a kernel are written out in the open.
//
// The sequences are `NCR5380_select` and `NCR5380_transfer_pio` from
// `doc/drivers/Linux/NCR5380.c`, transcribed by hand.  They are deliberately
// not shared with `tb/cpp/sci_driver.h`: this side of the library is meant to
// look like the guest, and a self test that reused the testbench's driver
// model would be checking the model against itself.

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "wish_rtl.h"

namespace {

// The registers, from doc/NCR5380_design_manual_Mar86.pdf p. 10.
enum {
  R_CSD = 0, R_ODR = 0, R_ICR = 1, R_MR = 2, R_TCR = 3,
  R_CSB = 4, R_SER = 4, R_BSR = 5, R_SDS = 5,
  R_IDR = 6, R_RPI = 7,
};

constexpr uint8_t ICR_RST = 0x80, ICR_AIP = 0x40, ICR_LA = 0x20;
constexpr uint8_t ICR_ACK = 0x10, ICR_BSY = 0x08, ICR_SEL = 0x04;
constexpr uint8_t ICR_ATN = 0x02, ICR_DATA = 0x01;
constexpr uint8_t MR_ARB = 0x01;
constexpr uint8_t CSB_BSY = 0x40, CSB_REQ = 0x20;
constexpr uint8_t TCR_IO = 0x01;

constexpr uint8_t PH_DATA_OUT = 0, PH_DATA_IN = 1, PH_COMMAND = 2;
constexpr uint8_t PH_STATUS = 3, PH_MSG_OUT = 6, PH_MSG_IN = 7;

const char* phase_name(uint8_t p) {
  switch (p & 7) {
    case 0: return "DATA OUT";
    case 1: return "DATA IN";
    case 2: return "COMMAND";
    case 3: return "STATUS";
    case 6: return "MESSAGE OUT";
    case 7: return "MESSAGE IN";
    default: return "unspecified";
  }
}

struct Lib {
  void* h = nullptr;
  uint32_t (*abi)(void);
  WishRtl* (*create)(const char*, uint32_t);
  void (*destroy)(WishRtl*);
  int (*reset)(WishRtl*);
  void (*write)(WishRtl*, int, uint8_t);
  uint8_t (*read)(WishRtl*, int);
  int (*irq)(WishRtl*);
  void (*run_ns)(WishRtl*, uint64_t);
  uint64_t (*time_ns)(WishRtl*);
  uint32_t (*blocks)(WishRtl*);
};

Lib lib;
WishRtl* dev = nullptr;
int failures = 0;

void fail(const std::string& what) {
  fprintf(stderr, "  FAIL  %s\n", what.c_str());
  failures++;
}

void w(int reg, uint8_t v) { lib.write(dev, reg, v); }
uint8_t r(int reg) { return lib.read(dev, reg); }
void delay_ns(uint64_t ns) { lib.run_ns(dev, ns); }

uint8_t phase_of(uint8_t csb) { return uint8_t((csb >> 2) & 7); }

// Poll a register until it matches, giving up after `us` microseconds of the
// core's own time.  A driver's polling loop advances the machine's clock; here
// it advances the core's, which is the same bargain.
bool poll(int reg, uint8_t mask, uint8_t want, uint64_t us, const char* what) {
  uint64_t t0 = lib.time_ns(dev);
  do {
    if ((r(reg) & mask) == want) return true;
    delay_ns(1000);
  } while (lib.time_ns(dev) - t0 < us * 1000);
  fail(std::string("timed out waiting for ") + what);
  return false;
}

// NCR5380_select, transcribed.
bool select_target(int id) {
  w(R_TCR, 0);
  w(R_ODR, 0x80);            // the host's ID: seven
  w(R_MR, MR_ARB);
  if (!poll(R_ICR, ICR_AIP, ICR_AIP, 1000, "arbitration to start")) return false;

  delay_ns(2400);            // the SCSI-2 arbitration delay, which is ours
  if (r(R_ICR) & ICR_LA) {
    fail("lost arbitration with nothing else on the bus");
    return false;
  }

  w(R_ICR, uint8_t(ICR_SEL | ICR_BSY));
  delay_ns(1200);
  w(R_ODR, uint8_t(0x80 | (1u << id)));
  w(R_ICR, uint8_t(ICR_BSY | ICR_DATA | ICR_ATN | ICR_SEL));
  w(R_MR, 0);
  w(R_SER, 0);
  delay_ns(200);
  w(R_ICR, uint8_t(ICR_DATA | ICR_ATN | ICR_SEL));   // drop BSY
  delay_ns(200);

  if (!poll(R_CSB, CSB_BSY, CSB_BSY, 5000, "the target to answer")) return false;
  delay_ns(200);
  w(R_ICR, ICR_ATN);
  return true;
}

// NCR5380_transfer_pio, transcribed.  Returns bytes moved; stops on a phase
// change, which is how a target says a phase is over.
size_t pio(uint8_t phase, const uint8_t* out, uint8_t* in, size_t n) {
  w(R_TCR, phase);
  bool is_in = (phase & TCR_IO) != 0;
  size_t i = 0;
  for (; i < n; i++) {
    if (!poll(R_CSB, uint8_t(CSB_REQ | CSB_BSY), uint8_t(CSB_REQ | CSB_BSY),
              5000, "REQ")) {
      break;
    }
    if (phase_of(r(R_CSB)) != phase) break;

    uint8_t keep = (phase == PH_MSG_OUT && (i + 1) < n) ? ICR_ATN : 0;
    if (is_in) {
      in[i] = r(R_CSD);
      w(R_ICR, uint8_t(keep | ICR_ACK));
    } else {
      w(R_ODR, out[i]);
      w(R_ICR, uint8_t(keep | ICR_DATA));
      w(R_ICR, uint8_t(keep | ICR_DATA | ICR_ACK));
    }
    if (!poll(R_CSB, CSB_REQ, 0, 5000, "REQ to be released")) { i++; break; }
    w(R_ICR, keep);
  }
  return i;
}

struct Result {
  bool ok = false;
  uint8_t status = 0xff;
  std::vector<uint8_t> data;
};

Result command(int id, const std::vector<uint8_t>& cdb,
               const std::vector<uint8_t>& out, size_t max_in) {
  Result res;
  if (!select_target(id)) return res;

  size_t out_pos = 0;
  for (int guard = 0; guard < 4096; guard++) {
    if (!poll(R_CSB, CSB_REQ, CSB_REQ, 5000, "a phase")) return res;
    uint8_t csb = r(R_CSB);
    if (!(csb & CSB_BSY)) break;

    switch (phase_of(csb)) {
      case PH_MSG_OUT: {
        uint8_t identify = 0x80;
        if (pio(PH_MSG_OUT, &identify, nullptr, 1) != 1) {
          fail("IDENTIFY was not accepted");
          return res;
        }
        break;
      }
      case PH_COMMAND:
        if (pio(PH_COMMAND, cdb.data(), nullptr, cdb.size()) != cdb.size()) {
          fail("the target took only part of the command");
          return res;
        }
        break;
      case PH_DATA_OUT: {
        size_t left = out.size() - out_pos;
        if (left == 0) { fail("the target asked for data we had none of"); return res; }
        out_pos += pio(PH_DATA_OUT, out.data() + out_pos, nullptr, left);
        break;
      }
      case PH_DATA_IN: {
        size_t left = max_in - res.data.size();
        if (left == 0) { fail("the target offered more data than expected"); return res; }
        std::vector<uint8_t> chunk(left);
        size_t got = pio(PH_DATA_IN, nullptr, chunk.data(), left);
        res.data.insert(res.data.end(), chunk.begin(), chunk.begin() + got);
        break;
      }
      case PH_STATUS:
        if (pio(PH_STATUS, nullptr, &res.status, 1) != 1) {
          fail("no status byte");
          return res;
        }
        break;
      case PH_MSG_IN: {
        uint8_t msg = 0;
        if (pio(PH_MSG_IN, nullptr, &msg, 1) != 1) { fail("no message in"); return res; }
        if (msg == 0x00) {
          poll(R_CSB, CSB_BSY, 0, 5000, "the bus to go free");
          res.ok = true;
          return res;
        }
        break;
      }
      default:
        fail(std::string("the target asked for ") + phase_name(phase_of(csb)));
        return res;
    }
  }
  fail("the command never finished");
  return res;
}

std::string ascii(const std::vector<uint8_t>& b, size_t off, size_t n) {
  std::string s;
  for (size_t i = 0; i < n && off + i < b.size(); i++) s += char(b[off + i]);
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = (argc > 1) ? argv[1] : "work/lib/libwish5380rtl.so";
  lib.h = dlopen(path, RTLD_NOW);
  if (!lib.h) {
    fprintf(stderr, "cannot load %s: %s\n", path, dlerror());
    return 2;
  }

#define SYM(name, member)                                        \
  do {                                                           \
    *(void**)(&lib.member) = dlsym(lib.h, name);                 \
    if (!lib.member) {                                           \
      fprintf(stderr, "missing symbol %s\n", name);              \
      return 2;                                                  \
    }                                                            \
  } while (0)
  SYM("wish_rtl_abi", abi);
  SYM("wish_rtl_new", create);
  SYM("wish_rtl_free", destroy);
  SYM("wish_rtl_reset", reset);
  SYM("wish_rtl_write", write);
  SYM("wish_rtl_read", read);
  SYM("wish_rtl_irq", irq);
  SYM("wish_rtl_run_ns", run_ns);
  SYM("wish_rtl_time_ns", time_ns);
  SYM("wish_rtl_blocks", blocks);
#undef SYM

  if (lib.abi() != WISH_RTL_ABI) {
    fprintf(stderr, "ABI %u, expected %u\n", lib.abi(), WISH_RTL_ABI);
    return 2;
  }

  printf("wish5380 self test, ABI %u\n", lib.abi());
  dev = lib.create(nullptr, 4096);
  if (!dev) { fprintf(stderr, "could not create the device\n"); return 2; }

  printf("resetting, and waiting for the card...\n");
  if (!lib.reset(dev)) {
    fprintf(stderr, "  FAIL  the card never came up\n");
    lib.destroy(dev);
    return 1;
  }
  printf("  card up after %llu us of simulated time\n",
         (unsigned long long)(lib.time_ns(dev) / 1000));

  // The registers, before anything is on the bus.
  if (r(R_CSB) != 0) fail("the bus is not idle after reset");
  if (lib.irq(dev)) fail("the interrupt is asserted after reset");

  printf("TEST UNIT READY...\n");
  Result t = command(0, {0x00, 0, 0, 0, 0, 0}, {}, 0);
  if (!t.ok) fail("TEST UNIT READY did not complete");
  else if (t.status != 0) fail("TEST UNIT READY returned CHECK CONDITION");
  else printf("  GOOD\n");

  printf("INQUIRY...\n");
  Result q = command(0, {0x12, 0, 0, 0, 36, 0}, {}, 36);
  if (!q.ok || q.data.size() != 36) {
    fail("INQUIRY did not return thirty-six bytes");
  } else {
    printf("  type %02x  '%s'  '%s'  '%s'\n", q.data[0],
           ascii(q.data, 8, 8).c_str(), ascii(q.data, 16, 16).c_str(),
           ascii(q.data, 32, 4).c_str());
    if (q.data[0] != 0x00) fail("not a direct access device");
  }

  printf("READ CAPACITY...\n");
  Result c = command(0, {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {}, 8);
  if (!c.ok || c.data.size() != 8) {
    fail("READ CAPACITY did not return eight bytes");
  } else {
    uint32_t last = (uint32_t(c.data[0]) << 24) | (uint32_t(c.data[1]) << 16) |
                    (uint32_t(c.data[2]) << 8) | c.data[3];
    uint32_t bs = (uint32_t(c.data[4]) << 24) | (uint32_t(c.data[5]) << 16) |
                  (uint32_t(c.data[6]) << 8) | c.data[7];
    printf("  last block %u, %u bytes each\n", last, bs);
    if (bs != 512) fail("the block size is not 512");
    if (last != lib.blocks(dev) - 1) fail("the capacity is not the card's");
  }

  printf("WRITE(6) then READ(6) of block 3...\n");
  std::vector<uint8_t> payload(512);
  for (size_t i = 0; i < payload.size(); i++) payload[i] = uint8_t(i * 7 + 1);
  Result wq = command(0, {0x0a, 0, 0, 3, 1, 0}, payload, 0);
  if (!wq.ok || wq.status != 0) fail("WRITE(6) failed");

  Result rq = command(0, {0x08, 0, 0, 3, 1, 0}, {}, 512);
  if (!rq.ok || rq.status != 0) {
    fail("READ(6) failed");
  } else if (rq.data != payload) {
    fail("the block did not read back as it was written");
  } else {
    printf("  512 bytes, unchanged\n");
  }

  printf("simulated time: %llu us\n",
         (unsigned long long)(lib.time_ns(dev) / 1000));
  lib.destroy(dev);

  if (failures) {
    printf("\n%d failure%s\n", failures, failures == 1 ? "" : "s");
    return 1;
  }
  printf("\nall good\n");
  return 0;
}

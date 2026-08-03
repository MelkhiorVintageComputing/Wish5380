/* SPDX-License-Identifier: MIT
 *
 * The C interface between an emulated ISA SCSI card and the Verilated
 * wish5380.
 *
 * The card knows nothing about Verilator: it loads this library at run time
 * and calls it for the handful of things its host interface can do - reset the
 * chip, read or write one of the eight registers, and ask whether the
 * interrupt is asserted.  Those are the same things a card asks any 5380
 * model for, which is what makes the two interchangeable.
 *
 * There are no guest-memory callbacks, and their absence is the point.  The
 * sibling project needed them because a LANCE masters the bus and fetches its
 * own descriptors; the NCR 5380 has no address counter and no byte counter and
 * never masters anything, so every byte crosses the register port under the
 * driver's own control.  A whole class of co-simulation difficulty - where the
 * guest's RAM is, whether it is contiguous, what an IOMMU does to it - simply
 * does not arise.
 *
 * What does arise instead is time.  A LANCE is ready the moment it is reset;
 * an SD card takes milliseconds to come up, and until it has, the disk behind
 * this chip answers NOT READY.  So `wish_rtl_reset` does not return until the
 * card is up or has failed to be, which is what a machine that finishes its
 * power-on self test before probing SCSI would see anyway.
 *
 * Nothing here is re-entrant and nothing runs in the background.  A call that
 * gives the core work to do runs the simulation until it has finished.
 */

#ifndef WISH_RTL_H
#define WISH_RTL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WISH_RTL_ABI 2

typedef struct WishRtl WishRtl;

/* Bumped whenever anything below changes shape.  The caller checks it. */
uint32_t wish_rtl_abi(void);

/* `image` is the file behind the SD card slot, or NULL for a blank card.  It
 * is read on open and written back by wish_rtl_flush and wish_rtl_free, so a
 * guest that writes a sector finds it there afterwards. */
WishRtl *wish_rtl_new(const char *image, uint32_t blocks);
void wish_rtl_free(WishRtl *r);
void wish_rtl_flush(WishRtl *r);

/* Pulse the part's RESET pin, then run until the card behind it has finished
 * initialising.  Returns non-zero if it did. */
int wish_rtl_reset(WishRtl *r);

/* The eight registers, as A2..A0 select them.  These are the chip's own
 * numbers, not any board's offsets: where a board puts them is the board's
 * business and it has already undone that by the time it calls here. */
void wish_rtl_write(WishRtl *r, int reg, uint8_t val);
uint8_t wish_rtl_read(WishRtl *r, int reg);

/* The core's interrupt output, as it stands now. */
int wish_rtl_irq(WishRtl *r);

/* The DMA handshake.  A board with a real DMA controller in front of the chip
 * watches DRQ and answers it with an acknowledge cycle, which reaches a data
 * register without the address being decoded at all (p. 6) - so there is no
 * register number to pass.
 *
 * A card that moves bytes with the CPU never needs any of this: it polls the
 * DRQ bit in the Bus and Status Register instead, and has no End of Process
 * pin to assert.  That is the whole difference between the Macintosh's
 * pseudo-DMA and the Sun-3's Am9516. */
int wish_rtl_drq(WishRtl *r);
uint8_t wish_rtl_dack_read(WishRtl *r);
void wish_rtl_dack_write(WishRtl *r, uint8_t val);

/* Hold End of Process asserted across the next acknowledge cycle, which is
 * what sets END OF DMA and, if it is enabled, interrupts. */
void wish_rtl_set_eop(WishRtl *r, int asserted);

/* Let the core run with nothing else happening.  A card calls this from a
 * timer so the target and the SD card keep making progress while the guest is
 * between accesses. */
void wish_rtl_run_ns(WishRtl *r, uint64_t ns);

/* Simulated time the core has run for, in nanoseconds.  For diagnostics. */
uint64_t wish_rtl_time_ns(WishRtl *r);

/* How many 512-byte blocks the card holds, and whether it came up. */
uint32_t wish_rtl_blocks(WishRtl *r);
int wish_rtl_ready(WishRtl *r);

#ifdef __cplusplus
}
#endif

#endif /* WISH_RTL_H */

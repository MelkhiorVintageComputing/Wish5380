// SPDX-License-Identifier: MIT
//
// The machine glue: a Wishbone B4 slave in front of the NCR 5380's register
// port.
//
// Everything board-specific lives here and nothing else does.  The chip
// decodes /CS with A0..A2 on the die, so every board that used one presented
// the same eight registers; what boards differ in is how far apart those
// registers sit, and whether there is a pseudo-DMA aperture in front of them.
//
// The Macintosh is the model, and it has three windows.  NetBSD's
// `sbc_obio.c` names them (lines 60-74):
//
//   SBC_REG_OFS   the registers, sixteen bytes apart - `(reg) << 4` in
//                 Linux's `mac_scsi.c`
//   SBC_HSK_OFS   pseudo-DMA with a hardware handshake, which waits for DRQ
//                 and raises a bus error if it waits too long
//   SBC_DMA_OFS   pseudo-DMA without one, for the machines whose handshake is
//                 broken or absent
//
// The handshake window is where the Mac's character comes from.  Linux reads
// and writes it with `moveb` and `movew` and wraps them in an exception fixup
// table, because the hardware raises a bus error when the chip does not
// produce a byte within the processor's bus error timeout.  The driver counts
// on that as a normal outcome rather than a fault: a faulting `moveb` on
// receive is retried, and a faulting `movew` aborts the transfer because the
// residual byte count is then uncertain.  `ERR_O` is that bus error.
//
// A `movew` is the widest access the aperture has to serve.  `MOVE_16_WORDS`
// looks like a burst and is not - it is sixteen `movew` instructions unrolled
// for speed, each one still two bytes.  So an access here moves one SCSI byte
// per asserted byte lane, in ascending byte address order.

module wb_5380 #(
  parameter int CLK_PERIOD_PS = 20000,

  // Bytes between one register and the next.  Sixteen is the Mac's
  // `(reg) << 4`; a generic ISA card sets it to one.  Must be a power of two.
  parameter int REG_STRIDE = 16,

  // Byte offsets of the three windows within the slave, and how big the two
  // pseudo-DMA ones are.  Each window must be at least four bytes and
  // aligned, so that every lane of one access falls in the same window.
  parameter int REG_BASE = 'h000,
  parameter int HSK_BASE = 'h100,
  parameter int DMA_BASE = 'h200,
  parameter int PDMA_SIZE = 'h100,

  // How long the handshaking window waits for DRQ before it gives up and
  // raises a bus error.  A 68000 board's bus error timeout is of this order;
  // what matters is that it exists, because a target that changes phase will
  // never produce the byte and the driver is waiting for the fault.
  parameter int DRQ_TIMEOUT_NS = 16000
) (
  input  logic clk_i,
  input  logic rst_i,

  // ---- Wishbone B4 classic slave -----------------------------------------
  //
  // Word addressed: ADR carries the index of a 32-bit word and SEL alone says
  // which bytes are meant.  Byte address is ADR * 4 + lane, which is the
  // little-endian lane convention; a big-endian host crosses the lanes in its
  // bridge, the same way a Sun crossed them in front of a LANCE.
  input  wb_req_t wb_i,
  output wb_rsp_t wb_o,

  // ---- the part's microprocessor port ------------------------------------
  output logic       stb_o,
  output logic       we_o,
  output logic       dack_o,
  output logic [2:0] adr_o,
  output logic [7:0] dat_o,
  input  logic [7:0] dat_i,
  input  logic       drq_i
);

  localparam int REG_SHIFT = $clog2(REG_STRIDE);
  localparam int REG_SPAN  = 8 * REG_STRIDE;

  localparam int T_DRQ = (DRQ_TIMEOUT_NS * 1000 + CLK_PERIOD_PS - 1) /
                         CLK_PERIOD_PS;
  localparam int TCNT_W = $clog2(T_DRQ + 1);
  localparam logic [TCNT_W-1:0] N_DRQ = T_DRQ[TCNT_W-1:0];

  // The slave occupies four kibibytes, which is room for the three windows
  // with the Mac's spacing.  Anything above that is the machine's own decode
  // and never reaches here.
  localparam int DEC_W = 12;

  // ---------------------------------------------------------------------------
  // Decode
  // ---------------------------------------------------------------------------

  // A part select only applies to a name, not to an expression, so the sums
  // get their own integers first.
  // Each window is decoded by masking rather than by a pair of comparisons.
  // That needs every window to be a power of two in size and aligned to it,
  // which is the same requirement the header above states anyway, and it
  // avoids a range check that is constant whenever a base is zero.
  localparam int I_REG_MASK = REG_SPAN - 1;
  localparam int I_PDMA_MASK = PDMA_SIZE - 1;
  localparam int I_STRIDE_MASK = REG_STRIDE - 1;

  localparam logic [DEC_W-1:0] A_REG = REG_BASE[DEC_W-1:0];
  localparam logic [DEC_W-1:0] A_HSK = HSK_BASE[DEC_W-1:0];
  localparam logic [DEC_W-1:0] A_DMA = DMA_BASE[DEC_W-1:0];
  localparam logic [DEC_W-1:0] M_REG = I_REG_MASK[DEC_W-1:0];
  localparam logic [DEC_W-1:0] M_PDMA = I_PDMA_MASK[DEC_W-1:0];

  // The reply, assembled at the port.  `wb_dat` is combinational and the two
  // handshake bits are registered, and a struct carrying both kinds of driver
  // is multiply driven as far as a linter is concerned even though no bit is.
  logic [31:0] wb_dat;
  logic        wb_ack, wb_err;

  // Icarus 11 rejects a variable part select of a *struct member* - `A
  // reference to a wire or reg is not allowed in a constant expression` - and
  // accepts the identical select of a plain vector.  The lane multiplexer
  // below indexes by `lane`, so the write data gets a name of its own first.
  logic [31:0] wb_wdata;
  assign wb_wdata = wb_i.dat;

  always_comb begin
    wb_o.dat = wb_dat;
    wb_o.ack = wb_ack;
    wb_o.err = wb_err;
  end

  logic [DEC_W-1:0] badr;                   // byte address of lane zero
  assign badr = {wb_i.adr[DEC_W-3:0], 2'b00};

  // The slave is four kibibytes.  Anything above that is the machine's own
  // decode and should never have been routed here, so it is a fault rather
  // than something to alias back into the register window.
  logic in_range;
  assign in_range = (wb_i.adr[29:DEC_W-2] == '0);

  logic in_reg, in_hsk, in_dma;
  assign in_reg = in_range && ((badr & ~M_REG)  == A_REG);
  assign in_hsk = in_range && ((badr & ~M_PDMA) == A_HSK);
  assign in_dma = in_range && ((badr & ~M_PDMA) == A_DMA);

  // The lanes of one access, lowest byte address first.
  logic [1:0] first_lane;
  logic       any_lane;
  always_comb begin
    any_lane   = (wb_i.sel != 4'b0000);
    first_lane = 2'd0;
    if      (wb_i.sel[0]) first_lane = 2'd0;
    else if (wb_i.sel[1]) first_lane = 2'd1;
    else if (wb_i.sel[2]) first_lane = 2'd2;
    else                  first_lane = 2'd3;
  end

  logic one_lane;
  assign one_lane = (wb_i.sel == 4'b0001) || (wb_i.sel == 4'b0010) ||
                    (wb_i.sel == 4'b0100) || (wb_i.sel == 4'b1000);

  // A register is one byte, and the eight of them are REG_STRIDE apart, so a
  // register access is a byte access whose offset divides exactly.
  //
  // The alignment test is a mask rather than a part select, because with a
  // stride of one the part select would be `[-1:0]` and no tool accepts that
  // however unreachable the branch is.
  localparam logic [DEC_W-1:0] STRIDE_MASK = I_STRIDE_MASK[DEC_W-1:0];

  logic [DEC_W-1:0] reg_off;
  logic             reg_aligned;
  logic [2:0]       reg_idx;
  assign reg_off = (badr + {{(DEC_W-2){1'b0}}, first_lane}) - A_REG;
  assign reg_aligned = ((reg_off & STRIDE_MASK) == '0);
  assign reg_idx = reg_off[REG_SHIFT+2:REG_SHIFT];

  // A wider access to the register window is answered with a bus error.  No
  // driver makes one - every one of them reaches the registers with a byte
  // access - and what real hardware would do with a `movew` there is a
  // property of the board's decoder rather than of the chip, so there is
  // nothing to be faithful to.  A fault says so; quietly serving one lane and
  // dropping the rest would not.
  logic reg_ok;
  assign reg_ok = one_lane && reg_aligned;

  // ---------------------------------------------------------------------------
  // The cycle
  // ---------------------------------------------------------------------------

  localparam logic [2:0] S_IDLE = 3'd0;
  localparam logic [2:0] S_REG  = 3'd1;
  localparam logic [2:0] S_WAIT = 3'd2;   // pseudo-DMA, waiting for DRQ
  localparam logic [2:0] S_XFER = 3'd3;   // pseudo-DMA, one byte
  localparam logic [2:0] S_ACK  = 3'd4;
  localparam logic [2:0] S_ERR  = 3'd5;
  localparam logic [2:0] S_END  = 3'd6;   // hold until the master lets go

  logic [2:0]  st;
  logic [1:0]  lane;
  logic        hsk;         // this access waits for DRQ
  logic [31:0] rdata;
  logic [TCNT_W-1:0] tcnt;

  // The lanes still to serve after this one.
  logic [3:0] left;
  logic [1:0] next_lane;
  logic       more;
  always_comb begin
    left = wb_i.sel & ~((4'b1 << lane) | ((4'b1 << lane) - 4'b1));
    more = (left != 4'b0000);
    next_lane = 2'd0;
    if      (left[0]) next_lane = 2'd0;
    else if (left[1]) next_lane = 2'd1;
    else if (left[2]) next_lane = 2'd2;
    else              next_lane = 2'd3;
  end

  assign wb_dat = rdata;

  always_comb begin
    // An access is an event, not a level: the chip's registers 5, 6 and 7
    // start a DMA transfer on the act of being written and register 7 clears
    // three status bits on the act of being read.  So the strobe is asserted
    // in exactly one state, for exactly one clock, and never speculatively.
    stb_o  = (st == S_REG) || (st == S_XFER);
    dack_o = (st == S_XFER);
    we_o   = wb_i.we;
    adr_o  = (st == S_XFER) ? 3'd0 : reg_idx;
    dat_o  = wb_wdata[8*lane +: 8];
  end

  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      st       <= S_IDLE;
      lane     <= 2'd0;
      hsk      <= 1'b0;
      rdata    <= '0;
      tcnt     <= '0;
      wb_ack <= 1'b0;
      wb_err <= 1'b0;
    end else begin
      wb_ack <= 1'b0;
      wb_err <= 1'b0;

      unique case (st)
        S_IDLE: begin
          if (wb_i.cyc && wb_i.stb) begin
            rdata <= '0;
            lane  <= first_lane;
            tcnt  <= '0;
            if (!any_lane) begin
              // A cycle that selects no bytes has nothing to do, and must not
              // strobe the chip.
              st <= S_ACK;
            end else if (in_reg) begin
              st <= reg_ok ? S_REG : S_ERR;
            end else if (in_hsk || in_dma) begin
              hsk <= in_hsk;
              st  <= S_WAIT;
            end else begin
              st <= S_ERR;
            end
          end
        end

        // A register access.  Read data is combinational on the address, so
        // it is captured in the same clock the strobe goes out.
        S_REG: begin
          rdata[8*lane +: 8] <= dat_i;
          st <= S_ACK;
        end

        // Pseudo-DMA.  The handshaking window waits for the chip to say it
        // has a byte, or has room for one; the other window does not, because
        // the driver using it has already satisfied itself.
        S_WAIT: begin
          if (!hsk || drq_i) begin
            st <= S_XFER;
          end else if (tcnt == N_DRQ) begin
            st <= S_ERR;
          end else begin
            tcnt <= tcnt + 1'b1;
          end
        end

        S_XFER: begin
          rdata[8*lane +: 8] <= dat_i;
          if (more) begin
            lane <= next_lane;
            tcnt <= '0;
            st   <= S_WAIT;
          end else begin
            st <= S_ACK;
          end
        end

        S_ACK: begin
          wb_ack <= 1'b1;
          st <= S_END;
        end

        S_ERR: begin
          wb_err <= 1'b1;
          st <= S_END;
        end

        // Wait for the master to drop the cycle before another can start.  A
        // master that held STB through the acknowledge would otherwise be
        // given a second access, and on this chip a second access is a second
        // strobe.
        default: if (!(wb_i.cyc && wb_i.stb)) st <= S_IDLE;
      endcase
    end
  end

endmodule

// SPDX-License-Identifier: MIT
//
// Everything of the NCR 5380 that is not a register: what it drives onto the
// SCSI bus, what it reports back, the arbitration it does on its own, the
// REQ/ACK handshake it automates in DMA mode, and the six things that make it
// interrupt.
//
// The chip is unusually thin.  It has no address counter and no byte counter,
// and above a single handshake it does almost nothing: the driver drives SEL,
// releases BSY, counts the bytes and decides when a phase is over, by writing
// registers one at a time.  Section 7 (p. 18) lists the whole of what is in
// hardware - DMA transfers, arbitration, phase change monitoring, bus
// disconnection, bus reset, parity checking, and selection/reselection - and
// that list is what this module is.
//
// The part is clockless (p. 18): its bus free, bus set and bus settle delays
// come out of gate propagation and "may differ between devices because of
// inherent process variations".  A clocked replica counts them out instead,
// so they are derived from CLK_PERIOD_PS rather than written down, and a
// build for a slower machine moves them together.
//
// The 2.2 us arbitration delay is deliberately absent.  The datasheet is
// explicit that the chip does not implement it - "This delay must be
// implemented in the controlling software driver" (p. 18) - and both
// reference drivers do, Linux as udelay(3) for the SCSI-2 value of 2.4 us.
// Counting it here would make a driver that omitted it work when on real
// silicon it would not.

module sci_bus #(
  // The system clock period in picoseconds.  See the note above.
  parameter int CLK_PERIOD_PS = 20000
) (
  input  logic clk_i,
  input  logic rst_i,

  // ---- from the register file --------------------------------------------
  input  logic [7:0] odr_i,
  input  logic [7:0] icr_i,
  input  logic [7:0] mr_i,
  input  logic [7:0] tcr_i,
  input  logic [7:0] ser_i,

  // Accesses, one cycle each.
  input  logic       csd_rd_i,    // read of register 0: check parity (p. 21)
  input  logic       rpi_i,       // read of register 7: empty the latches
  input  logic       sds_i,       // write 5: start DMA send
  input  logic       sdtr_i,      // write 6: start DMA target receive
  input  logic       sdir_i,      // write 7: start DMA initiator receive
  input  logic       dack_rd_i,   // a DMA acknowledge read  took a byte
  input  logic       dack_wr_i,   // a DMA acknowledge write gave a byte
  input  logic       eop_i,       // the End of Process pin, active high here

  // ---- to the register file ----------------------------------------------
  output logic       aip_o,
  output logic       la_o,
  output logic [7:0] csd_o,
  output logic [7:0] csb_o,
  output logic [7:0] idr_o,
  output logic       end_dma_o,
  output logic       drq_o,
  output logic       par_err_o,
  output logic       irq_o,
  output logic       phase_match_o,
  output logic       busy_err_o,
  output logic       atn_o,
  output logic       ack_o,

  output logic       sclr_o,        // an SCSI reset: clear the registers
  output logic       icr_clr_lo_o,  // loss of BSY: clear ICR[5:0]
  output logic       mr_dma_clr_o,  // loss of BSY: clear MR.DMA

  // ---- the internal SCSI bus ---------------------------------------------
  output scsi_t drive_o,
  input  scsi_t bus_i
);

  // ---------------------------------------------------------------------------
  // Delays, in clocks.  The two the chip implements happen both to be 400 ns
  // and are kept apart anyway, because they answer different questions.
  // ---------------------------------------------------------------------------

  localparam int T_BUS_FREE   = (400_000 + CLK_PERIOD_PS - 1) / CLK_PERIOD_PS;
  localparam int T_BUS_SETTLE = (400_000 + CLK_PERIOD_PS - 1) / CLK_PERIOD_PS;
  localparam int T_MAX = (T_BUS_FREE > T_BUS_SETTLE) ? T_BUS_FREE : T_BUS_SETTLE;
  localparam int CNT_W = $clog2(T_MAX + 1);

  localparam logic [CNT_W-1:0] N_MAX        = T_MAX[CNT_W-1:0];
  localparam logic [CNT_W-1:0] N_BUS_FREE   = T_BUS_FREE[CNT_W-1:0];
  localparam logic [CNT_W-1:0] N_BUS_SETTLE = T_BUS_SETTLE[CNT_W-1:0];

  // ---------------------------------------------------------------------------
  // Declarations first, because several of these are read by more than one of
  // the blocks below and SystemVerilog wants them before their use.
  // ---------------------------------------------------------------------------

  logic target_mode, dma_mode, arbitrate, monitor_bsy, test_mode;
  logic par_chk, par_intr_en, eop_intr_en;
  // What this chip drives depends on what it sees, because as an initiator it
  // only drives the data lines when the phase the target is asking for
  // matches the Target Command Register.  Field by field that is not a loop:
  // the data lines depend on MSG, C/D and I/O, and those depend only on the
  // Target Command Register and on the other devices.  Verilator treats a
  // packed struct as one signal, though, so `drive_o` depending on `bus_i`
  // and `bus_i` depending on `drive_o` reads as circular even when no field
  // depends on itself.  Synthesis sees the real, acyclic netlist; this
  // suppression is for the analysis granularity and nothing else.
  /* verilator lint_off UNOPTFLAT */
  logic drv_en, drive_data;
  /* verilator lint_on UNOPTFLAT */
  logic arb_active, la_q, arbitrating;
  logic dma_ack, dma_req, recv_latch;
  logic req_q, rst_q;
  logic [CNT_W-1:0] free_cnt;
  logic bus_free, bsy_settled_false;
  logic parity_bad, par_err_q;
  logic sel_match, sel_int;
  logic busy_err_q, busy_lost;
  logic phase_mismatch_int, scsi_rst_int;
  logic irq_q, eop_int, parity_int;

  assign target_mode = mr_i[wish5380_pkg::MR_TARGET_B];
  assign dma_mode    = mr_i[wish5380_pkg::MR_DMA_B];
  assign arbitrate   = mr_i[wish5380_pkg::MR_ARB_B];
  assign monitor_bsy = mr_i[wish5380_pkg::MR_MON_BSY_B];
  assign par_chk     = mr_i[wish5380_pkg::MR_PAR_CHK_B];
  assign par_intr_en = mr_i[wish5380_pkg::MR_PAR_INTR_B];
  assign eop_intr_en = mr_i[wish5380_pkg::MR_EOP_INTR_B];
  assign test_mode   = icr_i[wish5380_pkg::ICR_TEST_B];

  // ---------------------------------------------------------------------------
  // Phase match (p. 16).
  //
  // Continuously updated, and only meaningful as an initiator.  The Target
  // Command Register carries the phase in bits 2:0 as MSG, C/D, I/O from the
  // top down, which is the same order the bus presents them in.
  // ---------------------------------------------------------------------------

  assign phase_match_o = ({bus_i.msg, bus_i.cd, bus_i.io} == tcr_i[2:0]);

  // ---------------------------------------------------------------------------
  // What this chip drives onto the bus.
  // ---------------------------------------------------------------------------

  // The Output Data Register reaches the bus in two quite different
  // situations, and conflating them is the classic way to make selection
  // fail:
  //
  //  * during arbitration the chip asserts it by itself, without ASSERT DATA
  //    BUS being set at all (p. 12, the AIP description).  Linux relies on
  //    this - NCR5380_select writes the ID to register 0 and MR_ARBITRATE to
  //    register 2, and does not touch Initiator Command until arbitration has
  //    been won;
  //  * afterwards it needs ASSERT DATA BUS, and as an initiator also needs
  //    the bus not to be in an input phase and the phase to match the Target
  //    Command Register (p. 12).  That is why the same function writes zero
  //    to the Target Command Register first, with the comment "otherwise the
  //    NCR5380 won't drive the data bus during SELECTION".
  assign drive_data = arbitrating ||
                      (icr_i[wish5380_pkg::ICR_DATA_B] &&
                       (target_mode || (!bus_i.io && phase_match_o)));

  // TEST MODE floats every output driver, "effectively removing the NCR 5380
  // from the circuit" (p. 12).
  assign drv_en = !test_mode;

  always_comb begin
    drive_o      = '0;
    drive_o.rst  = drv_en && icr_i[wish5380_pkg::ICR_RST_B];
    // BSY comes from the register and from arbitration both, which is what
    // lets a driver take it over: Linux sets ASSERT BSY while MR_ARBITRATE is
    // still on, then clears the Mode Register, and BSY never glitches.
    drive_o.bsy  = drv_en && (icr_i[wish5380_pkg::ICR_BSY_B] || arbitrating);
    drive_o.sel  = drv_en && icr_i[wish5380_pkg::ICR_SEL_B];
    // ATN and ACK are asserted only in the initiator role, and REQ, MSG, C/D
    // and I/O only in the target role (p. 13).
    drive_o.atn  = drv_en && !target_mode && icr_i[wish5380_pkg::ICR_ATN_B];
    drive_o.ack  = drv_en && !target_mode &&
                   (icr_i[wish5380_pkg::ICR_ACK_B] || dma_ack);
    drive_o.req  = drv_en && target_mode &&
                   (tcr_i[wish5380_pkg::TCR_REQ_B] || dma_req);
    drive_o.msg  = drv_en && target_mode && tcr_i[wish5380_pkg::TCR_MSG_B];
    drive_o.cd   = drv_en && target_mode && tcr_i[wish5380_pkg::TCR_CD_B];
    drive_o.io   = drv_en && target_mode && tcr_i[wish5380_pkg::TCR_IO_B];
    drive_o.data = (drv_en && drive_data) ? odr_i : 8'h00;
    // Odd parity, always generated (p. 8).  It is not meaningful during
    // arbitration, where the data lines are a set of IDs rather than a byte,
    // and the datasheet says so (p. 10); generating it anyway costs nothing
    // and is what the silicon does.
    drive_o.dbp  = (drv_en && drive_data) ? ~(^odr_i) : 1'b0;
  end

  // ---------------------------------------------------------------------------
  // The two windows onto the bus, and the two bus signals the Bus and Status
  // Register carries instead of the Current SCSI Bus Status one (p. 16).
  // ---------------------------------------------------------------------------

  assign csd_o = bus_i.data;
  assign atn_o = bus_i.atn;
  assign ack_o = bus_i.ack;

  always_comb begin
    csb_o = '0;
    csb_o[wish5380_pkg::CSB_RST_B] = bus_i.rst;
    csb_o[wish5380_pkg::CSB_BSY_B] = bus_i.bsy;
    csb_o[wish5380_pkg::CSB_REQ_B] = bus_i.req;
    csb_o[wish5380_pkg::CSB_MSG_B] = bus_i.msg;
    csb_o[wish5380_pkg::CSB_CD_B]  = bus_i.cd;
    csb_o[wish5380_pkg::CSB_IO_B]  = bus_i.io;
    csb_o[wish5380_pkg::CSB_SEL_B] = bus_i.sel;
    csb_o[wish5380_pkg::CSB_DBP_B] = bus_i.dbp;
  end

  // ---------------------------------------------------------------------------
  // How long BSY has been false.
  //
  // One counter answers all three questions that ask it: the bus free filter
  // that lets arbitration start (p. 18), the bus settle delay before a
  // selection is believed (p. 19), and the same delay before a loss of BSY is
  // called unexpected (p. 22).
  // ---------------------------------------------------------------------------

  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      req_q <= 1'b0;
      rst_q <= 1'b0;
    end else begin
      req_q <= bus_i.req;
      rst_q <= bus_i.rst;
    end
  end

  always_ff @(posedge clk_i) begin
    if (rst_i) free_cnt <= '0;
    else if (bus_i.bsy) free_cnt <= '0;
    else if (free_cnt != N_MAX) free_cnt <= free_cnt + 1'b1;
  end

  assign bus_free          = (free_cnt >= N_BUS_FREE);
  assign bsy_settled_false = (free_cnt >= N_BUS_SETTLE);

  // ---------------------------------------------------------------------------
  // Arbitration (p. 18).
  //
  // "Arbitration will begin if the bus is free, SEL is inactive and the
  // ARBITRATION bit (port 2, bit 0) is active."  The chip then asserts BSY
  // and the contents of the Output Data Register, and raises AIP.
  //
  // What the chip does *not* do is decide who won.  It reports two things -
  // AIP, and LOST ARBITRATION when another device asserts SEL - and the
  // driver does the rest, comparing the data bus against its own ID mask.
  // Linux's NCR5380_select reads Current SCSI Data and tests it against
  // id_higher_mask for exactly that reason.
  // ---------------------------------------------------------------------------

  always_ff @(posedge clk_i) begin
    if (rst_i || sclr_o) begin
      arb_active <= 1'b0;
      la_q       <= 1'b0;
    end else if (!arbitrate) begin
      // "AIP will remain active until the ARBITRATE bit is reset" (p. 12), so
      // this is the only thing that ends arbitration.  It is also how a
      // driver keeps BSY across the handover: Linux asserts ASSERT BSY first
      // and clears the Mode Register second.
      arb_active <= 1'b0;
      la_q       <= 1'b0;
    end else begin
      if (!arb_active && bus_free && !bus_i.sel) arb_active <= 1'b1;
      // Lost only to *another* device's SEL (p. 12).  Our own does not count,
      // and a driver asserts it while still arbitrating.
      if (arb_active && bus_i.sel && !drive_o.sel) la_q <= 1'b1;
    end
  end

  // Both bits go the moment ARBITRATE does rather than a clock later, which
  // is what "AIP will remain active until the ARBITRATE bit is reset" says
  // and what stops the chip driving BSY and the ID for an extra clock after
  // a driver has given up.
  assign arbitrating = arb_active && arbitrate;
  assign aip_o = arbitrating;
  assign la_o  = la_q && arbitrate;

  // ---------------------------------------------------------------------------
  // The DMA handshake (pp. 14, 24).
  //
  // "In the DMA mode, REQ (pin 20) and ACK (pin 14) are automatically
  // controlled."  Three registers start a transfer and the role decides which
  // of the two this chip drives: an initiator answers REQ with ACK, a target
  // offers REQ and waits for ACK.  There is no byte count anywhere - the
  // transfer runs until the DMA MODE bit is reset, EOP arrives, or the phase
  // changes.
  //
  // The states are named for what the chip is waiting for, and the two roles
  // share all but two of them.  Nothing here is edge triggered: each state is
  // only reachable after the signal it tests has been seen false, so a level
  // test cannot catch the previous byte's handshake.
  // ---------------------------------------------------------------------------

  localparam logic [2:0] D_IDLE   = 3'd0;
  localparam logic [2:0] D_S_WANT = 3'd1;  // send: waiting for the host's byte
  localparam logic [2:0] D_S_HAVE = 3'd2;  // send: byte in ODR, not offered yet
  localparam logic [2:0] D_S_HS   = 3'd3;  // send: our handshake asserted
  localparam logic [2:0] D_S_TREL = 3'd4;  // send, target: waiting for ACK to go
  localparam logic [2:0] D_R_WANT = 3'd5;  // recv: waiting for a byte
  localparam logic [2:0] D_R_HAVE = 3'd6;  // recv: byte in IDR, DRQ up
  localparam logic [2:0] D_R_HS   = 3'd7;  // recv: our handshake asserted

  logic [2:0] dma_st;
  logic       dma_tgt;   // the role the transfer was started in
  logic       drq_q;
  logic [7:0] idr_q;
  logic       end_dma_q;

  assign idr_o = idr_q;
  // Resetting the DMA MODE bit halts the transfer "at any time" (p. 25), so
  // DRQ and the handshake follow it in the same cycle rather than waiting for
  // the state machine's next edge.
  assign drq_o = drq_q && dma_mode;

  // What we drive, by role.  A target holds REQ up through D_R_WANT because
  // it is REQ that asks the initiator for the byte in the first place; an
  // initiator has nothing to drive there, because it is waiting to be asked.
  assign dma_ack = dma_mode && !dma_tgt &&
                   (dma_st == D_S_HS || dma_st == D_R_HS);
  assign dma_req = dma_mode &&  dma_tgt &&
                   (dma_st == D_S_HS || dma_st == D_R_WANT);

  // A phase mismatch "prevents the recognition of REQ" (p. 22).  A target
  // sets the phase itself, so the rule is an initiator's alone.
  logic req_ok;
  assign req_ok = bus_i.req && phase_match_o;

  // "Data is latched either during a DMA Target receive operation when ACK
  // goes active or during a DMA Initiator receive when REQ goes active"
  // (p. 11).
  assign recv_latch = (dma_st == D_R_WANT) &&
                      (dma_tgt ? bus_i.ack : req_ok);

  always_ff @(posedge clk_i) begin
    if (rst_i || sclr_o) begin
      dma_st  <= D_IDLE;
      dma_tgt <= 1'b0;
      drq_q   <= 1'b0;
      idr_q   <= '0;
    end else if (!dma_mode) begin
      // "A DMA operation may be halted at any time simply by resetting the
      // DMA MODE bit" (p. 25), and both drivers use that rather than EOP.
      dma_st <= D_IDLE;
      drq_q  <= 1'b0;
    end else begin
      unique case (dma_st)
        D_IDLE: begin
          // Writing one of the three start registers is the whole trigger;
          // the data written is meaningless (p. 16).
          if (sds_i) begin
            dma_st  <= D_S_WANT;
            dma_tgt <= target_mode;
            // The chip asks for a byte before it has anywhere to put it: the
            // Output Data Register is its one byte of buffer, and filling it
            // early is what lets END OF DMA be set while "the SCSI transfer
            // may still be in progress" (p. 20).
            drq_q   <= 1'b1;
          end else if (sdtr_i) begin
            dma_st  <= D_R_WANT;
            dma_tgt <= 1'b1;
          end else if (sdir_i) begin
            dma_st  <= D_R_WANT;
            dma_tgt <= 1'b0;
          end
        end

        D_S_WANT: begin
          if (dack_wr_i) begin
            drq_q  <= 1'b0;
            dma_st <= D_S_HAVE;
          end
        end

        D_S_HAVE: begin
          // A target offers the byte by raising REQ; an initiator waits to be
          // asked for it.
          if (dma_tgt) dma_st <= D_S_HS;
          else if (req_ok) dma_st <= D_S_HS;
        end

        D_S_HS: begin
          if (dma_tgt) begin
            if (bus_i.ack) dma_st <= D_S_TREL;
          end else if (!bus_i.req) begin
            dma_st <= D_S_WANT;
            drq_q  <= 1'b1;
          end
        end

        D_S_TREL: begin
          if (!bus_i.ack) begin
            dma_st <= D_S_WANT;
            drq_q  <= 1'b1;
          end
        end

        D_R_WANT: begin
          if (recv_latch) begin
            idr_q  <= bus_i.data;
            drq_q  <= 1'b1;
            dma_st <= D_R_HAVE;
          end
        end

        D_R_HAVE: begin
          if (dack_rd_i) begin
            drq_q  <= 1'b0;
            dma_st <= D_R_HS;
          end
        end

        D_R_HS: begin
          if (dma_tgt ? !bus_i.ack : !bus_i.req) dma_st <= D_R_WANT;
        end

        default: dma_st <= D_IDLE;
      endcase
    end
  end

  // END OF DMA (p. 16): "set if EOP, DACK and either IOR or IOW are
  // simultaneously active", and "reset when the DMA MODE bit is reset".
  // Reading register 7 does *not* clear it, which is why it is not in
  // BSR_RPI_CLEARS; a driver that expected it to would spin.
  always_ff @(posedge clk_i) begin
    if (rst_i || sclr_o) end_dma_q <= 1'b0;
    else if (!dma_mode) end_dma_q <= 1'b0;
    else if (eop_i && (dack_rd_i || dack_wr_i)) end_dma_q <= 1'b1;
  end

  assign end_dma_o = end_dma_q && dma_mode;

  // ---------------------------------------------------------------------------
  // Selection and reselection (p. 19).
  //
  // "The NCR 5380 can generate a select interrupt if SEL is true, its device
  // ID is true and BSY is false for at least a bus settle delay (400 ns) ...
  // Only a single bit match is required ... This interrupt may be disabled by
  // writing zeros into all bits of the Select Enable Register."
  //
  // Whether it was a selection or a reselection is left to the driver, which
  // reads I/O out of the Current SCSI Bus Status Register.  The chip does not
  // distinguish them, and neither does this.
  // ---------------------------------------------------------------------------

  assign sel_match = bus_i.sel && !bus_i.bsy && ((bus_i.data & ser_i) != 8'h00);
  assign sel_int   = sel_match && bsy_settled_false;

  // ---------------------------------------------------------------------------
  // Parity (p. 21).
  //
  // "Parity is checked during a read of the Current SCSI Data Register (port
  // 0) and during a DMA receive operation", and also during selection, where
  // the datasheet makes a point of saying that ENABLE PARITY INTERRUPT "need
  // not be set for this interrupt to be generated" - only ENABLE PARITY
  // CHECKING, so a driver can tell a good selection from a corrupt one
  // (p. 19).  The latch is emptied by reading register 7 (p. 17).
  // ---------------------------------------------------------------------------

  assign parity_bad = (bus_i.dbp != ~(^bus_i.data));

  always_ff @(posedge clk_i) begin
    if (rst_i || sclr_o) par_err_q <= 1'b0;
    else if (rpi_i) par_err_q <= 1'b0;
    else if (par_chk && parity_bad && (csd_rd_i || recv_latch || sel_int))
      par_err_q <= 1'b1;
  end

  assign par_err_o = par_err_q;

  // ---------------------------------------------------------------------------
  // Unexpected loss of BSY (pp. 13, 16, 22).
  //
  // "If the MONITOR BUSY bit is active, an interrupt will be generated if the
  // BSY signal goes false for at least a bus settle delay."  It takes the bus
  // away as it goes: the lower six bits of Initiator Command are reset and
  // every signal is removed (p. 13), and the DMA MODE bit is reset (p. 16).
  //
  // The chip has no idea which losses are expected.  MONITOR BUSY is how the
  // driver says it expects BSY to stay, and Linux sets it together with
  // MR_DMA_MODE at the top of NCR5380_transfer_dma, when a target is
  // certainly still connected.
  // ---------------------------------------------------------------------------

  assign busy_lost = monitor_bsy && bsy_settled_false && !busy_err_q;

  always_ff @(posedge clk_i) begin
    if (rst_i || sclr_o) busy_err_q <= 1'b0;
    else if (rpi_i) busy_err_q <= 1'b0;
    else if (busy_lost) busy_err_q <= 1'b1;
  end

  assign busy_err_o   = busy_err_q;
  assign icr_clr_lo_o = busy_lost;
  assign mr_dma_clr_o = busy_lost;

  // ---------------------------------------------------------------------------
  // Bus phase mismatch (p. 22).
  //
  // "If the DMA MODE bit is active and a phase mismatch occurs when REQ
  // transitions from false to true, an interrupt is generated."  Only an
  // initiator can have one; a target sets the phase itself.
  // ---------------------------------------------------------------------------

  assign phase_mismatch_int = dma_mode && !target_mode &&
                              bus_i.req && !req_q && !phase_match_o;

  // ---------------------------------------------------------------------------
  // SCSI bus reset (p. 23).
  //
  // Whether it came from another device or from this chip's own ASSERT RST
  // bit, an SCSI reset clears the internal logic and every register "except
  // for the IRQ interrupt latch and the ASSERT RST bit".  The two cases are
  // the same edge here, because what this chip drives is part of what it
  // sees - which is also why the register file spares ASSERT RST rather than
  // this module having to.
  //
  // The interrupt "cannot be disabled": there is no enable bit for it in the
  // Mode Register.
  // ---------------------------------------------------------------------------

  assign scsi_rst_int = bus_i.rst && !rst_q;
  assign sclr_o = scsi_rst_int;

  // ---------------------------------------------------------------------------
  // The interrupt latch (p. 19).
  //
  // Set by any enabled condition, cleared by reading register 7 or by the
  // RESET pin.  An SCSI reset is deliberately not in the clear term: it
  // spares the latch, so a driver that pulses RST still learns why it was
  // interrupted.
  // ---------------------------------------------------------------------------

  assign eop_int    = eop_intr_en && eop_i && dma_mode;
  assign parity_int = par_chk && par_intr_en && parity_bad &&
                      (csd_rd_i || recv_latch);

  always_ff @(posedge clk_i) begin
    if (rst_i) irq_q <= 1'b0;
    else if (rpi_i) irq_q <= 1'b0;
    else if (sel_int || eop_int || scsi_rst_int || parity_int ||
             phase_mismatch_int || busy_lost)
      irq_q <= 1'b1;
  end

  assign irq_o = irq_q;

endmodule

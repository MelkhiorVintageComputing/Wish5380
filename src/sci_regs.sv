// SPDX-License-Identifier: MIT
//
// The NCR 5380's eight registers, and the port they hide behind (p. 10).
//
// This block holds the five registers that are storage - Output Data,
// Initiator Command, Mode, Target Command and Select Enable - and presents
// the four that are windows onto the SCSI engine: Current SCSI Data, Current
// SCSI Bus Status, Bus and Status, and Input Data.  It decodes the four
// strobes as events rather than as values, because writing register 5, 6 or 7
// starts a DMA transfer whatever the data bus happens to carry, and reading
// register 7 clears three status bits (p. 17).
//
// The real part is clockless and decodes an asynchronous /CS with /IOR or
// /IOW.  Here an access is a single cycle with `stb_i` high; `wb_5380` is
// what turns a Wishbone cycle into one.  `doc/interface.md` records that
// bargain, and why a synchronous port is enough to satisfy drivers written
// for the asynchronous one.
//
// Three things about this register file are easy to get wrong, and each is
// pinned by a test:
//
//  * Initiator Command reads back different bits than were written in
//    positions 6 and 5 - AIP and LA out, TEST MODE and DIFF ENBL in (p. 12) -
//    so a read/modify/write of it does not preserve what was there.  Both
//    reference drivers work around it, so the chip must have the fault.
//  * an access with DACK asserted goes to a data register whatever the
//    address lines say (p. 6), so the address is not decoded at all then.
//  * an SCSI bus reset clears every register except the interrupt latch and
//    ASSERT RST (p. 23), which is a different reset from the RESET pin.

module sci_regs (
  input  logic       clk_i,
  input  logic       rst_i,       // the RESET pin: clears everything (p. 23)
  // An SCSI reset condition, from the bus or from ASSERT RST.  Clears the
  // registers but leaves ASSERT RST alone, so the driver's own reset pulse
  // does not switch itself off.
  input  logic       sclr_i,

  // ---- MPU port ----------------------------------------------------------
  input  logic       stb_i,       // one access, this cycle
  input  logic       we_i,        // write when high, read when low
  input  logic       dack_i,      // DMA acknowledge: a data register, no decode
  input  logic [2:0] adr_i,
  input  logic [7:0] dat_i,
  output logic [7:0] dat_o,

  // ---- what the registers drive ------------------------------------------
  output logic [7:0] odr_o,       // Output Data Register
  output logic [7:0] icr_o,       // Initiator Command, as written
  output logic [7:0] mr_o,        // Mode
  output logic [7:0] tcr_o,       // Target Command, bits 3:0 only
  output logic [7:0] ser_o,       // Select Enable

  // ---- what the engine reports -------------------------------------------
  input  logic       aip_i,           // ICR bit 6 on read
  input  logic       la_i,            // ICR bit 5 on read
  input  logic [7:0] csd_i,           // Current SCSI Data
  input  logic [7:0] csb_i,           // Current SCSI Bus Status
  input  logic [7:0] idr_i,           // Input Data
  input  logic       end_dma_i,       // BSR bit 7
  input  logic       drq_i,           // BSR bit 6
  input  logic       par_err_i,       // BSR bit 5
  input  logic       irq_i,           // BSR bit 4
  input  logic       phase_match_i,   // BSR bit 3
  input  logic       busy_err_i,      // BSR bit 2
  input  logic       atn_i,           // BSR bit 1
  input  logic       ack_i,           // BSR bit 0

  // ---- what the engine takes back ----------------------------------------
  //
  // An unexpected loss of BSY resets the lower six bits of Initiator Command
  // and removes every signal from the bus (p. 13), and resets the Mode
  // Register's DMA bit as well (p. 16).  The engine detects it; the registers
  // are where it lands.  Both beat whatever the host was writing in the same
  // cycle, because on the real part the host is not there at all.
  input  logic       icr_clr_lo_i,
  input  logic       mr_dma_clr_i,

  // BSY as it stands on the bus.  "Note: BSY must be active in order to set
  // the DMA Mode bit" (p. 14) - the one place a register write is refused
  // rather than merely ignored, and a driver that started a DMA transfer with
  // no target connected would find it had not started one.
  input  logic       bsy_i,

  // ---- strobes, one cycle each -------------------------------------------
  output logic       csd_rd_o,    // read 0:  where parity is checked (p. 21)
  output logic       rpi_o,       // read 7:  Reset Parity/Interrupt
  output logic       sds_o,       // write 5: Start DMA Send
  output logic       sdtr_o,      // write 6: Start DMA Target Receive
  output logic       sdir_o       // write 7: Start DMA Initiator Receive
);

  logic [7:0] odr_q, icr_q, mr_q, tcr_q, ser_q;

  assign odr_o = odr_q;
  assign icr_o = icr_q;
  assign mr_o  = mr_q;
  assign tcr_o = tcr_q;
  assign ser_o = ser_q;

  // ---- read ---------------------------------------------------------------
  //
  // A DACK access reaches the Input Data Register regardless of the address
  // (p. 6): the DMA controller has no address lines to offer.

  logic [7:0] icr_rd, bsr_rd;

  always_comb begin
    // Bits 6 and 5 come from the arbitration logic rather than from what was
    // written there (p. 12).
    icr_rd = icr_q;
    icr_rd[wish5380_pkg::ICR_AIP_B] = aip_i;
    icr_rd[wish5380_pkg::ICR_LA_B]  = la_i;
  end

  always_comb begin
    bsr_rd = '0;
    bsr_rd[wish5380_pkg::BSR_END_DMA_B]     = end_dma_i;
    bsr_rd[wish5380_pkg::BSR_DRQ_B]         = drq_i;
    bsr_rd[wish5380_pkg::BSR_PAR_ERR_B]     = par_err_i;
    bsr_rd[wish5380_pkg::BSR_IRQ_B]         = irq_i;
    bsr_rd[wish5380_pkg::BSR_PHASE_MATCH_B] = phase_match_i;
    bsr_rd[wish5380_pkg::BSR_BUSY_ERR_B]    = busy_err_i;
    bsr_rd[wish5380_pkg::BSR_ATN_B]         = atn_i;
    bsr_rd[wish5380_pkg::BSR_ACK_B]         = ack_i;
  end

  always_comb begin
    if (dack_i) begin
      dat_o = idr_i;
    end else begin
      unique case (adr_i)
        wish5380_pkg::A_CSD: dat_o = csd_i;
        wish5380_pkg::A_ICR: dat_o = icr_rd;
        wish5380_pkg::A_MR:  dat_o = mr_q;
        // Bits 7 and 6:4 of Target Command are not implemented on the NMOS
        // part and read as zero (appendix A7).
        wish5380_pkg::A_TCR: dat_o = tcr_q & wish5380_pkg::TCR_RW_MASK;
        wish5380_pkg::A_CSB: dat_o = csb_i;
        wish5380_pkg::A_BSR: dat_o = bsr_rd;
        wish5380_pkg::A_IDR: dat_o = idr_i;
        // Reading 7 is the strobe; the data it returns is not specified, and
        // both drivers throw it away.  Zero is as good as anything and is
        // what makes a stray read visible in a waveform.
        default:             dat_o = 8'h00;
      endcase
    end
  end

  // ---- strobes ------------------------------------------------------------
  //
  // A DACK access is a data transfer and never a strobe, however the address
  // lines happen to be sitting.

  logic decode;
  assign decode = stb_i && !dack_i;

  assign csd_rd_o = decode && !we_i && (adr_i == wish5380_pkg::A_CSD);
  assign rpi_o  = decode && !we_i && (adr_i == wish5380_pkg::A_RPI);
  assign sds_o  = decode &&  we_i && (adr_i == wish5380_pkg::A_SDS);
  assign sdtr_o = decode &&  we_i && (adr_i == wish5380_pkg::A_SDTR);
  assign sdir_o = decode &&  we_i && (adr_i == wish5380_pkg::A_SDIR);

  // ---- write --------------------------------------------------------------

  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      odr_q <= '0;
      icr_q <= '0;
      mr_q  <= '0;
      tcr_q <= '0;
      ser_q <= '0;
    end else begin
      if (sclr_i) begin
      // An SCSI reset clears the registers but leaves ASSERT RST standing, so
      // a driver holding the bus in reset is not cut off by its own pulse
      // (p. 23).
        odr_q <= '0;
        icr_q <= icr_q & (8'b1 << wish5380_pkg::ICR_RST_B);
        mr_q  <= '0;
        tcr_q <= '0;
        ser_q <= '0;
      end else if (stb_i && we_i) begin
        if (dack_i) begin
          odr_q <= dat_i;
        end else begin
          unique case (adr_i)
            wish5380_pkg::A_ODR: odr_q <= dat_i;
            wish5380_pkg::A_ICR: icr_q <= dat_i;
            wish5380_pkg::A_MR: begin
              mr_q <= dat_i;
              if (!bsy_i) mr_q[wish5380_pkg::MR_DMA_B] <= 1'b0;
            end
            wish5380_pkg::A_TCR: tcr_q <= dat_i;
            wish5380_pkg::A_SER: ser_q <= dat_i;
            // 5, 6 and 7 are strobes and hold nothing.
            default: ;
          endcase
        end
      end

      // The engine's own clears come last so they win a tie with the host.
      if (icr_clr_lo_i) icr_q[5:0] <= 6'b0;
      if (mr_dma_clr_i) mr_q[wish5380_pkg::MR_DMA_B] <= 1'b0;
    end
  end

endmodule

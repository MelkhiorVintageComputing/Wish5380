// SPDX-License-Identifier: MIT
//
// The part: the NCR 5380's eight registers and the engine behind them.
//
// This is the whole chip and nothing else.  Where the registers sit in a
// machine's address space, which byte lane they land on and whether there is
// a pseudo-DMA aperture in front of them are the board's business and belong
// to `wb_5380`; the datasheet's own pins stop here.
//
// The one liberty taken is the port: the real part decodes an asynchronous
// /CS with /IOR or /IOW (p. 6) and this one takes a single-cycle synchronous
// access instead.  `doc/interface.md` sets out why nothing a driver can do
// distinguishes the two.

module wish5380 #(
  parameter int CLK_PERIOD_PS = 20000
) (
  input  logic       clk_i,
  input  logic       rst_i,      // the RESET pin: clears everything (p. 23)

  // ---- the microprocessor port -------------------------------------------
  input  logic       stb_i,      // one access, this cycle
  input  logic       we_i,
  input  logic       dack_i,     // a DMA acknowledge, not a register access
  input  logic [2:0] adr_i,
  input  logic [7:0] dat_i,
  output logic [7:0] dat_o,

  // ---- the DMA handshake pins --------------------------------------------
  input  logic       eop_i,      // End of Process, active high here
  output logic       drq_o,
  output logic       irq_o,

  // ---- the SCSI bus ------------------------------------------------------
  output scsi_t drive_o,
  input  scsi_t bus_i
);

  logic [7:0] odr, icr, mr, tcr, ser;
  logic [7:0] csd_w, csb_w, idr_w;
  logic aip, la;
  logic csd_rd, rpi, sds, sdtr, sdir;
  logic end_dma, drq, par_err, irq, phase_match, busy_err, atn, ack;
  logic sclr, icr_clr_lo, mr_dma_clr;

  // An access with DACK is a data transfer whichever way it goes, and the
  // address lines are not decoded at all (p. 6).
  logic dack_rd, dack_wr;
  assign dack_rd = stb_i && !we_i && dack_i;
  assign dack_wr = stb_i &&  we_i && dack_i;

  assign drq_o = drq;
  assign irq_o = irq;

  sci_regs u_regs (
    .clk_i          (clk_i),
    .rst_i          (rst_i),
    .sclr_i         (sclr),
    .stb_i          (stb_i),
    .we_i           (we_i),
    .dack_i         (dack_i),
    .adr_i          (adr_i),
    .dat_i          (dat_i),
    .dat_o          (dat_o),
    .odr_o          (odr),
    .icr_o          (icr),
    .mr_o           (mr),
    .tcr_o          (tcr),
    .ser_o          (ser),
    .aip_i          (aip),
    .la_i           (la),
    .csd_i          (csd_w),
    .csb_i          (csb_w),
    .idr_i          (idr_w),
    .end_dma_i      (end_dma),
    .drq_i          (drq),
    .par_err_i      (par_err),
    .irq_i          (irq),
    .phase_match_i  (phase_match),
    .busy_err_i     (busy_err),
    .atn_i          (atn),
    .ack_i          (ack),
    .icr_clr_lo_i   (icr_clr_lo),
    .mr_dma_clr_i   (mr_dma_clr),
    .bsy_i          (bus_i.bsy),
    .csd_rd_o       (csd_rd),
    .rpi_o          (rpi),
    .sds_o          (sds),
    .sdtr_o         (sdtr),
    .sdir_o         (sdir)
  );

  sci_bus #(
    .CLK_PERIOD_PS (CLK_PERIOD_PS)
  ) u_bus (
    .clk_i          (clk_i),
    .rst_i          (rst_i),
    .odr_i          (odr),
    .icr_i          (icr),
    .mr_i           (mr),
    .tcr_i          (tcr),
    .ser_i          (ser),
    .csd_rd_i       (csd_rd),
    .rpi_i          (rpi),
    .sds_i          (sds),
    .sdtr_i         (sdtr),
    .sdir_i         (sdir),
    .dack_rd_i      (dack_rd),
    .dack_wr_i      (dack_wr),
    .eop_i          (eop_i),
    .aip_o          (aip),
    .la_o           (la),
    .csd_o          (csd_w),
    .csb_o          (csb_w),
    .idr_o          (idr_w),
    .end_dma_o      (end_dma),
    .drq_o          (drq),
    .par_err_o      (par_err),
    .irq_o          (irq),
    .phase_match_o  (phase_match),
    .atn_o          (atn),
    .ack_o          (ack),
    .busy_err_o     (busy_err),
    .sclr_o         (sclr),
    .icr_clr_lo_o   (icr_clr_lo),
    .mr_dma_clr_o   (mr_dma_clr),
    .drive_o        (drive_o),
    .bus_i          (bus_i)
  );

endmodule

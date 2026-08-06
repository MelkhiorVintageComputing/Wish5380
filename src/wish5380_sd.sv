// SPDX-License-Identifier: MIT
//
// The card with the card in it: `wish5380_wb` with an SD card behind its block
// interface.
//
// This is what an FPGA design instantiates.  The two halves are kept apart
// because the block interface is the seam that makes the rest testable - the
// regression exercises SCSI against a software disk and the SD layer against a
// software card, and only a handful of tests pay for both at once.

module wish5380_sd #(
  parameter int CLK_PERIOD_PS = 20000,

  // The machine's register spacing and window layout.  See `wb_5380`.
  parameter int REG_STRIDE = 16,
  parameter int REG_BASE = 'h000,
  parameter int HSK_BASE = 'h100,
  parameter int DMA_BASE = 'h200,
  parameter int PDMA_SIZE = 'h100,
  parameter int DRQ_TIMEOUT_NS = 16000,

  // The disks.  See `scsi_targ` and `wish5380_wb`.
  parameter int TARGETS = 2,
  parameter int TARGET_ID = 0,
  parameter int TARGET1_ID = 1,
  parameter logic [63:0]  VENDOR   = "DOLBEAU ",
  parameter logic [127:0] PRODUCT  = "WISH5380 SD CARD",
  parameter logic [31:0]  REVISION = "0001"
) (
  input  logic clk_i,
  input  logic rst_i,

  // ---- Wishbone B4 classic slave -----------------------------------------
  input  wb_req_t wb_i,
  output wb_rsp_t wb_o,

  // ---- the part's own pins -----------------------------------------------
  output logic irq_o,
  output logic drq_o,
  input  logic eop_i,

  // ---- the card slots -----------------------------------------------------
  //
  // One per drive.  The second is not driven at all when TARGETS is one, so a
  // board that carries a single slot leaves those pins unbonded.
  output logic sd_clk_o,
  output logic sd_cs_n_o,
  output logic sd_mosi_o,
  input  logic sd_miso_i,

  output logic sd1_clk_o,
  output logic sd1_cs_n_o,
  output logic sd1_mosi_o,
  // Nobody reads it when there is no second drive to read it for.
  /* verilator lint_off UNUSEDSIGNAL */
  input  logic sd1_miso_i,
  /* verilator lint_on UNUSEDSIGNAL */

  // ---- the private SCSI bus, for debug -----------------------------------
  output scsi_t bus_o,
  input  scsi_t peer_i
);

  blk_req_t blk_req;
  blk_rsp_t blk_rsp;

  // The second drive's wires exist whether or not there is a second drive on
  // the end of them; with TARGETS of one the target block ties its side low
  // and nothing reads them back.
  /* verilator lint_off UNUSEDSIGNAL */
  blk_req_t blk1_req;
  /* verilator lint_on UNUSEDSIGNAL */
  blk_rsp_t blk1_rsp;

  wish5380_wb #(
    .CLK_PERIOD_PS  (CLK_PERIOD_PS),
    .TARGETS        (TARGETS),
    .TARGET1_ID     (TARGET1_ID),
    .REG_STRIDE     (REG_STRIDE),
    .REG_BASE       (REG_BASE),
    .HSK_BASE       (HSK_BASE),
    .DMA_BASE       (DMA_BASE),
    .PDMA_SIZE      (PDMA_SIZE),
    .DRQ_TIMEOUT_NS (DRQ_TIMEOUT_NS),
    .TARGET_ID      (TARGET_ID),
    .VENDOR         (VENDOR),
    .PRODUCT        (PRODUCT),
    .REVISION       (REVISION)
  ) u_card (
    .clk_i        (clk_i),
    .rst_i        (rst_i),
    .wb_i         (wb_i),
    .wb_o         (wb_o),
    .irq_o        (irq_o),
    .drq_o        (drq_o),
    .eop_i        (eop_i),
    .blk_o        (blk_req),
    .blk_i        (blk_rsp),
    .blk1_o       (blk1_req),
    .blk1_i       (blk1_rsp),
    .bus_o        (bus_o),
    .peer_i       (peer_i)
  );

  blk_sd #(
    .CLK_PERIOD_PS (CLK_PERIOD_PS)
  ) u_sd (
    .clk_i       (clk_i),
    .rst_i       (rst_i),
    .blk_i       (blk_req),
    .blk_o       (blk_rsp),
    .sd_clk_o    (sd_clk_o),
    .sd_cs_n_o   (sd_cs_n_o),
    .sd_mosi_o   (sd_mosi_o),
    .sd_miso_i   (sd_miso_i)
  );

  generate
    if (TARGETS > 1) begin : g_sd1
      blk_sd #(
        .CLK_PERIOD_PS (CLK_PERIOD_PS)
      ) u_sd1 (
        .clk_i       (clk_i),
        .rst_i       (rst_i),
        .blk_i       (blk1_req),
        .blk_o       (blk1_rsp),
        .sd_clk_o    (sd1_clk_o),
        .sd_cs_n_o   (sd1_cs_n_o),
        .sd_mosi_o   (sd1_mosi_o),
        .sd_miso_i   (sd1_miso_i)
      );
    end else begin : g_no_sd1
      assign blk1_rsp   = '0;
      assign sd1_clk_o  = 1'b0;
      assign sd1_cs_n_o = 1'b1;
      assign sd1_mosi_o = 1'b1;
    end
  endgenerate

endmodule

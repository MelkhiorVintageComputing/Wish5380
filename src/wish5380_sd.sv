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

  // The disk.  See `scsi_targ`.
  parameter int TARGET_ID = 0,
  parameter logic [63:0]  VENDOR   = "DOLBEAU ",
  parameter logic [127:0] PRODUCT  = "WISH5380 SD CARD",
  parameter logic [31:0]  REVISION = "0001"
) (
  input  logic clk_i,
  input  logic rst_i,

  // ---- Wishbone B4 classic slave -----------------------------------------
  input  logic        wb_cyc_i,
  input  logic        wb_stb_i,
  input  logic        wb_we_i,
  input  logic [3:0]  wb_sel_i,
  input  logic [29:0] wb_adr_i,
  input  logic [31:0] wb_dat_i,
  output logic [31:0] wb_dat_o,
  output logic        wb_ack_o,
  output logic        wb_err_o,

  // ---- the part's own pins -----------------------------------------------
  output logic irq_o,
  output logic drq_o,
  input  logic eop_i,

  // ---- the card slot ------------------------------------------------------
  output logic sd_clk_o,
  output logic sd_cs_n_o,
  output logic sd_mosi_o,
  input  logic sd_miso_i,

  // ---- the private SCSI bus, for debug -----------------------------------
  output scsi_t bus_o,
  input  scsi_t peer_i
);

  logic        blk_start, blk_we, blk_done, blk_err, blk_ready;
  logic [31:0] blk_lba, blk_count;
  logic        bbuf_we;
  logic [8:0]  bbuf_addr;
  logic [7:0]  bbuf_wdata, bbuf_rdata;

  wish5380_wb #(
    .CLK_PERIOD_PS  (CLK_PERIOD_PS),
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
    .wb_cyc_i     (wb_cyc_i),
    .wb_stb_i     (wb_stb_i),
    .wb_we_i      (wb_we_i),
    .wb_sel_i     (wb_sel_i),
    .wb_adr_i     (wb_adr_i),
    .wb_dat_i     (wb_dat_i),
    .wb_dat_o     (wb_dat_o),
    .wb_ack_o     (wb_ack_o),
    .wb_err_o     (wb_err_o),
    .irq_o        (irq_o),
    .drq_o        (drq_o),
    .eop_i        (eop_i),
    .blk_start_o  (blk_start),
    .blk_we_o     (blk_we),
    .blk_lba_o    (blk_lba),
    .blk_done_i   (blk_done),
    .blk_err_i    (blk_err),
    .blk_ready_i  (blk_ready),
    .blk_count_i  (blk_count),
    .bbuf_we_i    (bbuf_we),
    .bbuf_addr_i  (bbuf_addr),
    .bbuf_wdata_i (bbuf_wdata),
    .bbuf_rdata_o (bbuf_rdata),
    .bus_o        (bus_o),
    .peer_i       (peer_i)
  );

  blk_sd #(
    .CLK_PERIOD_PS (CLK_PERIOD_PS)
  ) u_sd (
    .clk_i       (clk_i),
    .rst_i       (rst_i),
    .start_i     (blk_start),
    .we_i        (blk_we),
    .lba_i       (blk_lba),
    .done_o      (blk_done),
    .err_o       (blk_err),
    .ready_o     (blk_ready),
    .count_o     (blk_count),
    .buf_we_o    (bbuf_we),
    .buf_addr_o  (bbuf_addr),
    .buf_wdata_o (bbuf_wdata),
    .buf_rdata_i (bbuf_rdata),
    .sd_clk_o    (sd_clk_o),
    .sd_cs_n_o   (sd_cs_n_o),
    .sd_mosi_o   (sd_mosi_o),
    .sd_miso_i   (sd_miso_i)
  );

endmodule

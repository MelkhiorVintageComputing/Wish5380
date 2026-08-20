// SPDX-License-Identifier: MIT
//
// The SD back end on its own: `blk_sd` over `sd_spi`, with the block
// interface and the four card pins brought out and nothing else.
//
// No SCSI, no Wishbone, no target.  The regression reaches this stack the way
// a machine does - through the register window and a SCSI command - which is
// right for testing the design and wrong for testing the *card protocol*,
// because a disagreement about a response byte arrives as a timeout eight
// layers up.  Here the block interface is driven directly, so a card that
// answers differently says so at the byte that differs.
//
// The sector buffer lives in the target in the real design (`doc/block.md`),
// so it lives in the C++ here, behind `buf_rdata`.

module sdcheck_top (
  input  logic clk,
  input  logic rst,

  // Flat, not `blk_req_t`/`blk_rsp_t`: the C++ on the other side holds a
  // pointer per signal, the same reason `tb/sv/tb_top.sv` takes them apart.
  input  logic        blk_start_i,
  input  logic        blk_we_i,
  input  logic [31:0] blk_lba_i,
  input  logic [7:0]  blk_buf_rdata_i,

  output logic        blk_done_o,
  output logic        blk_err_o,
  output logic        blk_ready_o,
  output logic [31:0] blk_count_o,
  output logic        blk_buf_we_o,
  output logic [8:0]  blk_buf_addr_o,
  output logic [7:0]  blk_buf_wdata_o,

  output logic sd_clk_o,
  output logic sd_cs_n_o,
  output logic sd_mosi_o,
  input  logic sd_miso_i
);

  blk_req_t req;
  blk_rsp_t rsp;

  always_comb begin
    req.start     = blk_start_i;
    req.we        = blk_we_i;
    req.lba       = blk_lba_i;
    req.buf_rdata = blk_buf_rdata_i;
  end

  always_comb begin
    blk_done_o      = rsp.done;
    blk_err_o       = rsp.err;
    blk_ready_o     = rsp.ready;
    blk_count_o     = rsp.count;
    blk_buf_we_o    = rsp.buf_we;
    blk_buf_addr_o  = rsp.buf_addr;
    blk_buf_wdata_o = rsp.buf_wdata;
  end

  blk_sd #(
    .CLK_PERIOD_PS (20000)
  ) u_blk (
    .clk_i     (clk),
    .rst_i     (rst),
    .blk_i     (req),
    .blk_o     (rsp),
    .sd_clk_o  (sd_clk_o),
    .sd_cs_n_o (sd_cs_n_o),
    .sd_mosi_o (sd_mosi_o),
    .sd_miso_i (sd_miso_i)
  );

endmodule

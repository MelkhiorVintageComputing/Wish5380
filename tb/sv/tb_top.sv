// SPDX-License-Identifier: MIT
//
// The Verilator top level.
//
// It wires the device under test plus every leaf module that has a unit test,
// so one binary covers both levels and there is no second build to keep in
// step.  Adding a unit-tested leaf means adding its ports here.
//
// The device under test is the whole product, `wish5380_wb`, so every test
// reaches the chip the way a machine does - through the Wishbone slave and its
// register window.  The private SCSI bus comes out of the design as a debug
// port, which is what lets a test watch the fabric and stand a second device
// on it: the chip reads back the OR of what everybody is driving, including
// itself, and both reference drivers depend on seeing their own BSY.

module tb_top #(
  parameter int CLK_PERIOD_PS = 20000,
  // Bytes between one register and the next: sixteen for the Macintosh, one
  // for a generic ISA card.  Both instances below take the same one, so a
  // build is a board rather than a mixture.
  parameter int REG_STRIDE = 16
) (
  input  logic clk_i,
  input  logic rst_i,

  // ---- the Wishbone slave, driven by the host model ------------------------
  input  logic        wbs_cyc_i,
  input  logic        wbs_stb_i,
  input  logic        wbs_we_i,
  input  logic [3:0]  wbs_sel_i,
  input  logic [29:0] wbs_adr_i,
  input  logic [31:0] wbs_dat_i,
  output logic [31:0] wbs_dat_o,
  output logic        wbs_ack_o,
  output logic        wbs_err_o,

  // ---- the part's own pins -------------------------------------------------
  output logic dut_irq_o,
  output logic dut_drq_o,
  input  logic dut_eop_i,

  // ---- the target's block back end, modelled by the testbench --------------
  output logic        tg_blk_start_o,
  output logic        tg_blk_we_o,
  output logic [31:0] tg_blk_lba_o,
  input  logic        tg_blk_done_i,
  input  logic        tg_blk_err_i,
  input  logic        tg_blk_ready_i,
  input  logic [31:0] tg_blk_count_i,
  input  logic        tg_bbuf_we_i,
  input  logic [8:0]  tg_bbuf_addr_i,
  input  logic [7:0]  tg_bbuf_wdata_i,
  output logic [7:0]  tg_bbuf_rdata_o,

  // ...and the same again for the second drive on the bus.
  output logic        tg1_blk_start_o,
  output logic        tg1_blk_we_o,
  output logic [31:0] tg1_blk_lba_o,
  input  logic        tg1_blk_done_i,
  input  logic        tg1_blk_err_i,
  input  logic        tg1_blk_ready_i,
  input  logic [31:0] tg1_blk_count_i,
  input  logic        tg1_bbuf_we_i,
  input  logic [8:0]  tg1_bbuf_addr_i,
  input  logic [7:0]  tg1_bbuf_wdata_i,
  output logic [7:0]  tg1_bbuf_rdata_o,

  // ---- the peer device on the fabric, driven by the testbench --------------
  input  logic       pr_rst_i,
  input  logic       pr_bsy_i,
  input  logic       pr_sel_i,
  input  logic       pr_req_i,
  input  logic       pr_ack_i,
  input  logic       pr_atn_i,
  input  logic       pr_msg_i,
  input  logic       pr_cd_i,
  input  logic       pr_io_i,
  input  logic [7:0] pr_data_i,
  input  logic       pr_dbp_i,

  // ---- the bus as everybody sees it ----------------------------------------
  output logic       bus_rst_o,
  output logic       bus_bsy_o,
  output logic       bus_sel_o,
  output logic       bus_req_o,
  output logic       bus_ack_o,
  output logic       bus_atn_o,
  output logic       bus_msg_o,
  output logic       bus_cd_o,
  output logic       bus_io_o,
  output logic [7:0] bus_data_o,
  output logic       bus_dbp_o,

  // ---- the same design again, with a real card behind it -------------------
  //
  // A second instance, because the block interface is the seam that makes the
  // rest testable: the regression exercises SCSI against a software disk and
  // the SD layer against a software card, and only the sd_ tests pay for both
  // at once.
  input  logic        sdb_cyc_i,
  input  logic        sdb_stb_i,
  input  logic        sdb_we_i,
  input  logic [3:0]  sdb_sel_i,
  input  logic [29:0] sdb_adr_i,
  input  logic [31:0] sdb_dat_i,
  output logic [31:0] sdb_dat_o,
  output logic        sdb_ack_o,
  output logic        sdb_err_o,
  output logic        sd_irq_o,
  output logic        sd_drq_o,
  output logic        sd_bus_bsy_o,

  output logic sd_clk_o,
  output logic sd_cs_n_o,
  output logic sd_mosi_o,
  input  logic sd_miso_i,
  output logic sd1_clk_o,
  output logic sd1_cs_n_o,
  output logic sd1_mosi_o,
  input  logic sd1_miso_i,

  // ---- sci_regs, on its own ------------------------------------------------
  input  logic       rg_sclr_i,
  input  logic       rg_stb_i,
  input  logic       rg_we_i,
  input  logic       rg_dack_i,
  input  logic [2:0] rg_adr_i,
  input  logic [7:0] rg_dat_i,
  output logic [7:0] rg_dat_o,

  output logic [7:0] rg_odr_o,
  output logic [7:0] rg_icr_o,
  output logic [7:0] rg_mr_o,
  output logic [7:0] rg_tcr_o,
  output logic [7:0] rg_ser_o,

  input  logic       rg_aip_i,
  input  logic       rg_la_i,
  input  logic [7:0] rg_csd_i,
  input  logic [7:0] rg_csb_i,
  input  logic [7:0] rg_idr_i,
  input  logic       rg_end_dma_i,
  input  logic       rg_drq_i,
  input  logic       rg_par_err_i,
  input  logic       rg_irq_i,
  input  logic       rg_phase_match_i,
  input  logic       rg_busy_err_i,
  input  logic       rg_atn_i,
  input  logic       rg_ack_i,
  input  logic       rg_icr_clr_lo_i,
  input  logic       rg_mr_dma_clr_i,
  input  logic       rg_bsy_i,

  output logic       rg_csd_rd_o,
  output logic       rg_rpi_o,
  output logic       rg_sds_o,
  output logic       rg_sdtr_o,
  output logic       rg_sdir_o
);

  // The Wishbone and block interfaces are packed structs inside the design and
  // flat signals here, because the C++ models on the other side of these ports
  // hold a pointer per signal.  Verilator presents a packed struct as one wide
  // vector, so a model would have to slice bit ranges out of it - and a model
  // that had to be told a field's position would drift from the RTL silently.
  // The same reason `scsi_t` is taken apart below.

  wb_req_t  wbs_req, sdb_req;
  wb_rsp_t  wbs_rsp, sdb_rsp;
  blk_req_t tg_req, tg1_req;
  blk_rsp_t tg_rsp, tg1_rsp;

  always_comb begin
    wbs_req.cyc = wbs_cyc_i;
    wbs_req.stb = wbs_stb_i;
    wbs_req.we  = wbs_we_i;
    wbs_req.sel = wbs_sel_i;
    wbs_req.adr = wbs_adr_i;
    wbs_req.dat = wbs_dat_i;

    sdb_req.cyc = sdb_cyc_i;
    sdb_req.stb = sdb_stb_i;
    sdb_req.we  = sdb_we_i;
    sdb_req.sel = sdb_sel_i;
    sdb_req.adr = sdb_adr_i;
    sdb_req.dat = sdb_dat_i;

    tg_rsp.done      = tg_blk_done_i;
    tg_rsp.err       = tg_blk_err_i;
    tg_rsp.ready     = tg_blk_ready_i;
    tg_rsp.count     = tg_blk_count_i;
    tg_rsp.buf_we    = tg_bbuf_we_i;
    tg_rsp.buf_addr  = tg_bbuf_addr_i;
    tg_rsp.buf_wdata = tg_bbuf_wdata_i;

    tg1_rsp.done      = tg1_blk_done_i;
    tg1_rsp.err       = tg1_blk_err_i;
    tg1_rsp.ready     = tg1_blk_ready_i;
    tg1_rsp.count     = tg1_blk_count_i;
    tg1_rsp.buf_we    = tg1_bbuf_we_i;
    tg1_rsp.buf_addr  = tg1_bbuf_addr_i;
    tg1_rsp.buf_wdata = tg1_bbuf_wdata_i;
  end

  assign wbs_dat_o = wbs_rsp.dat;
  assign wbs_ack_o = wbs_rsp.ack;
  assign wbs_err_o = wbs_rsp.err;
  assign sdb_dat_o = sdb_rsp.dat;
  assign sdb_ack_o = sdb_rsp.ack;
  assign sdb_err_o = sdb_rsp.err;

  assign tg_blk_start_o  = tg_req.start;
  assign tg_blk_we_o     = tg_req.we;
  assign tg_blk_lba_o    = tg_req.lba;
  assign tg_bbuf_rdata_o = tg_req.buf_rdata;

  assign tg1_blk_start_o  = tg1_req.start;
  assign tg1_blk_we_o     = tg1_req.we;
  assign tg1_blk_lba_o    = tg1_req.lba;
  assign tg1_bbuf_rdata_o = tg1_req.buf_rdata;

  scsi_t peer_drive, bus;

  always_comb begin
    peer_drive.rst  = pr_rst_i;
    peer_drive.bsy  = pr_bsy_i;
    peer_drive.sel  = pr_sel_i;
    peer_drive.req  = pr_req_i;
    peer_drive.ack  = pr_ack_i;
    peer_drive.atn  = pr_atn_i;
    peer_drive.msg  = pr_msg_i;
    peer_drive.cd   = pr_cd_i;
    peer_drive.io   = pr_io_i;
    peer_drive.data = pr_data_i;
    peer_drive.dbp  = pr_dbp_i;
  end

  assign bus_rst_o  = bus.rst;
  assign bus_bsy_o  = bus.bsy;
  assign bus_sel_o  = bus.sel;
  assign bus_req_o  = bus.req;
  assign bus_ack_o  = bus.ack;
  assign bus_atn_o  = bus.atn;
  assign bus_msg_o  = bus.msg;
  assign bus_cd_o   = bus.cd;
  assign bus_io_o   = bus.io;
  assign bus_data_o = bus.data;
  assign bus_dbp_o  = bus.dbp;

  wish5380_wb #(
    .CLK_PERIOD_PS (CLK_PERIOD_PS),
    .REG_STRIDE    (REG_STRIDE)
  ) u_dut (
    .clk_i        (clk_i),
    .rst_i        (rst_i),
    .wb_i         (wbs_req),
    .wb_o         (wbs_rsp),
    .irq_o        (dut_irq_o),
    .drq_o        (dut_drq_o),
    .eop_i        (dut_eop_i),
    .blk_o        (tg_req),
    .blk_i        (tg_rsp),
    .blk1_o       (tg1_req),
    .blk1_i       (tg1_rsp),
    .bus_o        (bus),
    .peer_i       (peer_drive)
  );

  // The second instance's bus is watched only for BSY, which is enough for a
  // test to see that a command is in progress; the sd_ tests are about the
  // card behind it, and the SCSI side is covered against the fast disk.
  /* verilator lint_off UNUSEDSIGNAL */
  scsi_t sd_bus;
  /* verilator lint_on UNUSEDSIGNAL */
  scsi_t sd_peer;
  assign sd_peer = '0;
  assign sd_bus_bsy_o = sd_bus.bsy;

  wish5380_sd #(
    .CLK_PERIOD_PS (CLK_PERIOD_PS),
    .REG_STRIDE    (REG_STRIDE)
  ) u_sd (
    .clk_i     (clk_i),
    .rst_i     (rst_i),
    .wb_i      (sdb_req),
    .wb_o      (sdb_rsp),
    .irq_o     (sd_irq_o),
    .drq_o     (sd_drq_o),
    .eop_i     (1'b0),
    .sd_clk_o  (sd_clk_o),
    .sd_cs_n_o (sd_cs_n_o),
    .sd_mosi_o (sd_mosi_o),
    .sd_miso_i (sd_miso_i),
    .sd1_clk_o  (sd1_clk_o),
    .sd1_cs_n_o (sd1_cs_n_o),
    .sd1_mosi_o (sd1_mosi_o),
    .sd1_miso_i (sd1_miso_i),
    .bus_o     (sd_bus),
    .peer_i    (sd_peer)
  );

  sci_regs u_regs (
    .clk_i          (clk_i),
    .rst_i          (rst_i),
    .sclr_i         (rg_sclr_i),
    .stb_i          (rg_stb_i),
    .we_i           (rg_we_i),
    .dack_i         (rg_dack_i),
    .adr_i          (rg_adr_i),
    .dat_i          (rg_dat_i),
    .dat_o          (rg_dat_o),
    .odr_o          (rg_odr_o),
    .icr_o          (rg_icr_o),
    .mr_o           (rg_mr_o),
    .tcr_o          (rg_tcr_o),
    .ser_o          (rg_ser_o),
    .aip_i          (rg_aip_i),
    .la_i           (rg_la_i),
    .csd_i          (rg_csd_i),
    .csb_i          (rg_csb_i),
    .idr_i          (rg_idr_i),
    .end_dma_i      (rg_end_dma_i),
    .drq_i          (rg_drq_i),
    .par_err_i      (rg_par_err_i),
    .irq_i          (rg_irq_i),
    .phase_match_i  (rg_phase_match_i),
    .busy_err_i     (rg_busy_err_i),
    .atn_i          (rg_atn_i),
    .ack_i          (rg_ack_i),
    .icr_clr_lo_i   (rg_icr_clr_lo_i),
    .mr_dma_clr_i   (rg_mr_dma_clr_i),
    .bsy_i          (rg_bsy_i),
    .csd_rd_o       (rg_csd_rd_o),
    .rpi_o          (rg_rpi_o),
    .sds_o          (rg_sds_o),
    .sdtr_o         (rg_sdtr_o),
    .sdir_o         (rg_sdir_o)
  );

endmodule

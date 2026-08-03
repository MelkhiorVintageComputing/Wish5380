// SPDX-License-Identifier: MIT
//
// The co-simulation's view of the design: `wish5380_sd` configured as a
// generic ISA card - eight registers one byte apart - with its Wishbone slave
// and its card slot brought out and nothing else.
//
// The stride of one is not a convenience.  It is the configuration Linux's
// `g_NCR5380` drives with `board=0`, where `NCR5380_read(reg)` is
// `inb(base + reg)`, and it is the one thing about the design the regression
// does not otherwise exercise: everything in `tb/` is built for the Mac's
// stride of sixteen.

module rtl_top (
  input  logic clk,
  input  logic rst,

  input  logic        wb_cyc_i,
  input  logic        wb_stb_i,
  input  logic        wb_we_i,
  input  logic [3:0]  wb_sel_i,
  input  logic [29:0] wb_adr_i,
  input  logic [31:0] wb_dat_i,
  output logic [31:0] wb_dat_o,
  output logic        wb_ack_o,
  output logic        wb_err_o,

  output logic irq_o,
  output logic drq_o,

  output logic sd_clk_o,
  output logic sd_cs_n_o,
  output logic sd_mosi_o,
  input  logic sd_miso_i
);

  // The private SCSI bus is not brought out here.  A co-simulation that
  // reached inside would be checking the design against itself; the only view
  // it gets is the one the guest has.
  /* verilator lint_off UNUSEDSIGNAL */
  scsi_t bus;
  /* verilator lint_on UNUSEDSIGNAL */
  scsi_t peer;
  assign peer = '0;

  // The second card slot, which this build has no drive for.  The pins exist
  // because the module has them; nothing is bonded to them.
  /* verilator lint_off UNUSEDSIGNAL */
  logic sd1_clk, sd1_cs_n, sd1_mosi;
  /* verilator lint_on UNUSEDSIGNAL */

  wish5380_sd #(
    .CLK_PERIOD_PS (20000),
    // One drive.  The library's C interface has one card behind it, and an
    // empty second slot would only cost the guest a device reporting no
    // medium and this simulation the seconds a card takes to give up.
    .TARGETS       (1),
    .REG_STRIDE    (1),
    .REG_BASE      ('h000),
    .HSK_BASE      ('h100),
    .DMA_BASE      ('h200),
    .TARGET_ID     (0)
  ) u_dut (
    .clk_i     (clk),
    .rst_i     (rst),
    .wb_cyc_i  (wb_cyc_i),
    .wb_stb_i  (wb_stb_i),
    .wb_we_i   (wb_we_i),
    .wb_sel_i  (wb_sel_i),
    .wb_adr_i  (wb_adr_i),
    .wb_dat_i  (wb_dat_i),
    .wb_dat_o  (wb_dat_o),
    .wb_ack_o  (wb_ack_o),
    .wb_err_o  (wb_err_o),
    .irq_o     (irq_o),
    .drq_o     (drq_o),
    .eop_i     (1'b0),
    .sd_clk_o  (sd_clk_o),
    .sd_cs_n_o (sd_cs_n_o),
    .sd_mosi_o (sd_mosi_o),
    .sd_miso_i (sd_miso_i),
    .sd1_clk_o  (sd1_clk),
    .sd1_cs_n_o (sd1_cs_n),
    .sd1_mosi_o (sd1_mosi),
    .sd1_miso_i (1'b1),
    .bus_o     (bus),
    .peer_i    (peer)
  );

endmodule

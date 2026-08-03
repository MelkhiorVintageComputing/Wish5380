// SPDX-License-Identifier: MIT
//
// The Verilator top level.
//
// It wires the device under test plus every leaf module that has a unit test,
// so one binary covers both levels and there is no second build to keep in
// step.  Adding a unit-tested leaf means adding its ports here.

module tb_top (
  input  logic clk_i,
  input  logic rst_i,

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

  output logic       rg_rpi_o,
  output logic       rg_sds_o,
  output logic       rg_sdtr_o,
  output logic       rg_sdir_o
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
    .rpi_o          (rg_rpi_o),
    .sds_o          (rg_sds_o),
    .sdtr_o         (rg_sdtr_o),
    .sdir_o         (rg_sdir_o)
  );

endmodule

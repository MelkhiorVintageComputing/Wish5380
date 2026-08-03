// SPDX-License-Identifier: MIT
//
// The whole thing: a Wishbone B4 slave that a vintage machine's driver sees as
// an NCR 5380 with a disk on it, and that a board sees as a card slot.
//
//   wb_5380       the machine glue: three windows, byte lanes, pseudo-DMA
//   wish5380      the part itself
//   scsi_fabric   the wired-OR that joins everything on the bus
//   scsi_targ     the disk, reaching an SD card through a block interface
//
// There are no SCSI pads, so the bus between the chip and the target is
// private.  It is still brought out, as `bus_o` to watch and `peer_i` to
// drive: an interconnect that cannot be seen cannot be debugged on hardware
// either, and an integrator will want it on a logic analyser.  Tie `peer_i`
// to zero in a real design - a device driving nothing is a device that is not
// there, which is what an open-collector bus means by it - and leave `bus_o`
// unconnected.
//
// `drq_o` and `eop_i` are the part's own pins.  The Mac drives neither: its
// pseudo-DMA uses the DRQ that `wb_5380` already watches internally, and it
// has no End of Process at all.  A board with a real DMA controller in front
// of the chip wires both.

module wish5380_wb #(
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

  // ---- the block back end behind the target ------------------------------
  output logic        blk_start_o,
  output logic        blk_we_o,
  output logic [31:0] blk_lba_o,
  input  logic        blk_done_i,
  input  logic        blk_err_i,
  input  logic        blk_ready_i,
  input  logic [31:0] blk_count_i,
  input  logic        bbuf_we_i,
  input  logic [8:0]  bbuf_addr_i,
  input  logic [7:0]  bbuf_wdata_i,
  output logic [7:0]  bbuf_rdata_o,

  // ---- the private SCSI bus, for debug -----------------------------------
  output scsi_t bus_o,
  input  scsi_t peer_i
);

  logic       stb, we, dack;
  logic [2:0] adr;
  logic [7:0] wdata, rdata;
  logic       drq;

  scsi_t chip_drive, targ_drive, bus;

  assign drq_o = drq;
  assign bus_o = bus;

  wb_5380 #(
    .CLK_PERIOD_PS   (CLK_PERIOD_PS),
    .REG_STRIDE      (REG_STRIDE),
    .REG_BASE        (REG_BASE),
    .HSK_BASE        (HSK_BASE),
    .DMA_BASE        (DMA_BASE),
    .PDMA_SIZE       (PDMA_SIZE),
    .DRQ_TIMEOUT_NS  (DRQ_TIMEOUT_NS)
  ) u_glue (
    .clk_i    (clk_i),
    .rst_i    (rst_i),
    .wb_cyc_i (wb_cyc_i),
    .wb_stb_i (wb_stb_i),
    .wb_we_i  (wb_we_i),
    .wb_sel_i (wb_sel_i),
    .wb_adr_i (wb_adr_i),
    .wb_dat_i (wb_dat_i),
    .wb_dat_o (wb_dat_o),
    .wb_ack_o (wb_ack_o),
    .wb_err_o (wb_err_o),
    .stb_o    (stb),
    .we_o     (we),
    .dack_o   (dack),
    .adr_o    (adr),
    .dat_o    (wdata),
    .dat_i    (rdata),
    .drq_i    (drq)
  );

  wish5380 #(
    .CLK_PERIOD_PS (CLK_PERIOD_PS)
  ) u_chip (
    .clk_i   (clk_i),
    .rst_i   (rst_i),
    .stb_i   (stb),
    .we_i    (we),
    .dack_i  (dack),
    .adr_i   (adr),
    .dat_i   (wdata),
    .dat_o   (rdata),
    .eop_i   (eop_i),
    .drq_o   (drq),
    .irq_o   (irq_o),
    .drive_o (chip_drive),
    .bus_i   (bus)
  );

  scsi_targ #(
    .CLK_PERIOD_PS (CLK_PERIOD_PS),
    .TARGET_ID     (TARGET_ID),
    .VENDOR        (VENDOR),
    .PRODUCT       (PRODUCT),
    .REVISION      (REVISION)
  ) u_targ (
    .clk_i        (clk_i),
    .rst_i        (rst_i),
    .drive_o      (targ_drive),
    .bus_i        (bus),
    .blk_start_o  (blk_start_o),
    .blk_we_o     (blk_we_o),
    .blk_lba_o    (blk_lba_o),
    .blk_done_i   (blk_done_i),
    .blk_err_i    (blk_err_i),
    .blk_ready_i  (blk_ready_i),
    .blk_count_i  (blk_count_i),
    .bbuf_we_i    (bbuf_we_i),
    .bbuf_addr_i  (bbuf_addr_i),
    .bbuf_wdata_i (bbuf_wdata_i),
    .bbuf_rdata_o (bbuf_rdata_o)
  );

  scsi_fabric u_fabric (
    .a_i   (chip_drive),
    .b_i   (targ_drive),
    .c_i   (peer_i),
    .bus_o (bus)
  );

endmodule

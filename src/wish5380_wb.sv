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

  // How many drives are on the bus, one or two, and what they answer to.
  // Two is the interesting case: a SCSI bus with one device never exercises
  // the ID decode against anything that could get it wrong, because the only
  // other outcome is silence.
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

  // ---- the block back end behind each target -----------------------------
  //
  // One pair per drive; `doc/block.md` is the contract.  The second pair is
  // driven low and ignored when TARGETS is one.
  output blk_req_t blk_o,
  input  blk_rsp_t blk_i,

  // Nothing reads this when there is no second target to read it for.
  output blk_req_t blk1_o,
  /* verilator lint_off UNUSEDSIGNAL */
  input  blk_rsp_t blk1_i,
  /* verilator lint_on UNUSEDSIGNAL */

  // ---- the private SCSI bus, for debug -----------------------------------
  output scsi_t bus_o,
  input  scsi_t peer_i
);

  logic       stb, we, dack;
  logic [2:0] adr;
  logic [7:0] wdata, rdata;
  logic       drq;

  scsi_t chip_drive, targ_drive, targ1_drive, bus;

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
    .wb_i     (wb_i),
    .wb_o     (wb_o),
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
    .blk_o        (blk_o),
    .blk_i        (blk_i)
  );

  // The second drive.  A board that carries one leaves it out entirely rather
  // than wiring an empty slot to it: a device driving nothing is a device that
  // is not there, and a target that is not there is silence, which is what an
  // initiator selecting an absent ID is meant to hear.
  generate
    if (TARGETS > 1) begin : g_targ1
      scsi_targ #(
        .CLK_PERIOD_PS (CLK_PERIOD_PS),
        .TARGET_ID     (TARGET1_ID),
        .VENDOR        (VENDOR),
        .PRODUCT       (PRODUCT),
        .REVISION      (REVISION)
      ) u_targ1 (
        .clk_i        (clk_i),
        .rst_i        (rst_i),
        .drive_o      (targ1_drive),
        .bus_i        (bus),
        .blk_o        (blk1_o),
        .blk_i        (blk1_i)
      );
    end else begin : g_no_targ1
      assign targ1_drive = '0;
      assign blk1_o      = '0;
    end
  endgenerate

  scsi_fabric u_fabric (
    .a_i   (chip_drive),
    .b_i   (targ_drive),
    .c_i   (targ1_drive),
    .d_i   (peer_i),
    .bus_o (bus)
  );

endmodule

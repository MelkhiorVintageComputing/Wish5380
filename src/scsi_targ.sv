// SPDX-License-Identifier: MIT
//
// A SCSI direct-access device: the disk on the other end of the fabric.
//
// This is the half of the conversation the NCR 5380 does not do.  The chip is
// deliberately thin - it handshakes one byte and reports what it sees, and the
// driver drives everything above that - so something has to answer a
// selection, sequence the phases and understand a command descriptor block.
// On a real machine that is the firmware inside the drive.  Here it is this
// module, and behind it an SD card.
//
// The command set is the part of SCSI-1 a vintage driver actually uses to find
// a disk and boot from it.  Anything else is answered with CHECK CONDITION and
// ILLEGAL REQUEST, which is what a drive of the period did with a command it
// had never heard of.
//
// Nothing here disconnects.  A target is allowed not to - it keeps BSY and
// finishes the command - and every driver in `doc/drivers/` copes, because a
// target that never disconnects is the easy case for all of them.  The 5380
// side supports reselection; there is simply nothing that reselects.

module scsi_targ #(
  parameter int CLK_PERIOD_PS = 20000,
  // The SCSI ID this device answers to.  Apple's internal drive is 0 and the
  // host adapter is conventionally 7.
  parameter int TARGET_ID = 0,
  // INQUIRY strings, as fixed-width byte vectors so a top level can override
  // them without SystemVerilog string parameters.
  //
  // Some Apple utilities - HD SC Setup in particular - refuse to touch a drive
  // whose vendor is not "APPLE   ".  A machine that needs to be formatted by
  // one overrides these; booting from an already formatted card does not care.
  parameter logic [63:0]  VENDOR   = "DOLBEAU ",
  parameter logic [127:0] PRODUCT  = "WISH5380 SD CARD",
  parameter logic [31:0]  REVISION = "0001"
) (
  input  logic clk_i,
  input  logic rst_i,

  // ---- the SCSI bus ------------------------------------------------------
  output scsi_t drive_o,
  // A target drives REQ, MSG, C/D and I/O and never reads them back, and this
  // one does not check incoming parity, so four bits of the bus arrive unused.
  /* verilator lint_off UNUSEDSIGNAL */
  input  scsi_t bus_i,
  /* verilator lint_on UNUSEDSIGNAL */

  // ---- the block back end ------------------------------------------------
  //
  // A whole 512-byte block at a time, through the sector buffer below: the
  // back end fills it before a READ and drains it after a WRITE.  The SCSI
  // side and the back end never reach the buffer at the same time.
  output logic        blk_start_o,   // one cycle: begin
  output logic        blk_we_o,      // 1 = write the buffer to the media
  output logic [31:0] blk_lba_o,
  input  logic        blk_done_i,    // one cycle: finished
  input  logic        blk_err_i,     // sampled with blk_done_i
  input  logic        blk_ready_i,   // media present and initialised
  input  logic [31:0] blk_count_i,   // capacity, in 512-byte blocks

  // The back end's port into the sector buffer.
  input  logic        bbuf_we_i,
  input  logic [8:0]  bbuf_addr_i,
  input  logic [7:0]  bbuf_wdata_i,
  output logic [7:0]  bbuf_rdata_o
);

  // ---------------------------------------------------------------------------
  // SCSI-1 opcodes, statuses and sense codes, transcribed from the standard
  // rather than from any driver.  Only what this device answers to is here.
  // ---------------------------------------------------------------------------

  localparam logic [7:0] C_TEST_UNIT_READY  = 8'h00;
  localparam logic [7:0] C_REZERO           = 8'h01;
  localparam logic [7:0] C_REQUEST_SENSE    = 8'h03;
  localparam logic [7:0] C_FORMAT_UNIT      = 8'h04;
  localparam logic [7:0] C_READ6            = 8'h08;
  localparam logic [7:0] C_WRITE6           = 8'h0a;
  localparam logic [7:0] C_SEEK6            = 8'h0b;
  localparam logic [7:0] C_INQUIRY          = 8'h12;
  localparam logic [7:0] C_MODE_SELECT6     = 8'h15;
  localparam logic [7:0] C_RESERVE          = 8'h16;
  localparam logic [7:0] C_RELEASE          = 8'h17;
  localparam logic [7:0] C_MODE_SENSE6      = 8'h1a;
  localparam logic [7:0] C_START_STOP       = 8'h1b;
  localparam logic [7:0] C_SEND_DIAGNOSTIC  = 8'h1d;
  localparam logic [7:0] C_PREVENT_ALLOW    = 8'h1e;
  localparam logic [7:0] C_READ_CAPACITY    = 8'h25;
  localparam logic [7:0] C_READ10           = 8'h28;
  localparam logic [7:0] C_WRITE10          = 8'h2a;
  localparam logic [7:0] C_SEEK10           = 8'h2b;
  localparam logic [7:0] C_VERIFY10         = 8'h2f;

  localparam logic [7:0] ST_GOOD  = 8'h00;
  localparam logic [7:0] ST_CHECK = 8'h02;

  localparam logic [3:0] SK_NO_SENSE        = 4'h0;
  localparam logic [3:0] SK_NOT_READY       = 4'h2;
  localparam logic [3:0] SK_MEDIUM_ERROR    = 4'h3;
  localparam logic [3:0] SK_ILLEGAL_REQUEST = 4'h5;

  localparam logic [7:0] ASC_UNRECOVERED_READ   = 8'h11;
  localparam logic [7:0] ASC_INVALID_COMMAND    = 8'h20;
  localparam logic [7:0] ASC_LBA_OUT_OF_RANGE   = 8'h21;
  localparam logic [7:0] ASC_LUN_NOT_SUPPORTED  = 8'h25;
  localparam logic [7:0] ASC_MEDIUM_NOT_PRESENT = 8'h3a;

  localparam logic [7:0] M_COMMAND_COMPLETE = 8'h00;
  localparam logic [7:0] M_ABORT            = 8'h06;
  localparam logic [7:0] M_BUS_DEVICE_RESET = 8'h0c;

  // The bus settle delay before a selection is believed.  The same 400 ns the
  // chip itself waits (5380 manual p. 19); it is a property of the bus, not of
  // either device.
  localparam int T_SETTLE = (400_000 + CLK_PERIOD_PS - 1) / CLK_PERIOD_PS;
  localparam int SCNT_W = $clog2(T_SETTLE + 1);
  localparam logic [SCNT_W-1:0] N_SETTLE = T_SETTLE[SCNT_W-1:0];

  localparam logic [7:0] ID_MASK = 8'(1 << TARGET_ID);

  // ---------------------------------------------------------------------------
  // The sector buffer: one 512-byte block, dual ported so the back end can
  // fill or drain it without going through the SCSI side.
  // ---------------------------------------------------------------------------

  logic [7:0] mem [0:511];
  logic [8:0] a_addr;
  logic       a_we;
  logic [7:0] a_wdata, a_rdata;

  always_ff @(posedge clk_i) begin
    if (a_we) mem[a_addr] <= a_wdata;
    if (bbuf_we_i) mem[bbuf_addr_i] <= bbuf_wdata_i;
    a_rdata      <= mem[a_addr];
    bbuf_rdata_o <= mem[bbuf_addr_i];
  end

  // ---------------------------------------------------------------------------
  // State
  // ---------------------------------------------------------------------------

  localparam logic [3:0] S_IDLE    = 4'd0;   // not selected
  localparam logic [3:0] S_SELWAIT = 4'd1;   // BSY asserted, waiting for SEL
  localparam logic [3:0] S_MSGOUT  = 4'd2;
  localparam logic [3:0] S_CMD     = 4'd3;
  localparam logic [3:0] S_EXEC    = 4'd4;
  localparam logic [3:0] S_RDWAIT  = 4'd5;   // back end filling the buffer
  localparam logic [3:0] S_WRWAIT  = 4'd6;   // back end draining it
  localparam logic [3:0] S_DATAIN  = 4'd7;
  localparam logic [3:0] S_DATAOUT = 4'd8;
  localparam logic [3:0] S_STATUS  = 4'd9;
  localparam logic [3:0] S_MSGIN   = 4'd10;
  localparam logic [3:0] S_FREE    = 4'd11;

  // The byte handshake, shared by every information transfer phase.  SETUP
  // exists so the phase lines and the data are settled before REQ goes true,
  // which is what the standard asks of a target and what the sector buffer's
  // registered read needs anyway.
  localparam logic [1:0] H_SETUP = 2'd0;
  localparam logic [1:0] H_REQ   = 2'd1;
  localparam logic [1:0] H_REL   = 2'd2;

  logic [3:0] st;
  logic [1:0] hs;
  logic [SCNT_W-1:0] sel_cnt;

  logic [9:0]  idx;        // byte within the current phase or block
  logic [9:0]  xfer_len;   // how many bytes this phase moves
  // Up to twelve bytes, byte 0 in the low end.  The whole block is captured
  // whatever its length, and the fields this device ignores - the reserved
  // bytes, the control byte, and the logical unit in byte 1, which is read off
  // the bus as it arrives - are simply never looked at again.
  /* verilator lint_off UNUSEDSIGNAL */
  logic [95:0] cdb;
  /* verilator lint_on UNUSEDSIGNAL */
  logic [3:0]  cdb_len;
  logic [31:0] lba;
  logic [23:0] blk_left;   // blocks still to transfer
  logic [2:0]  lun;
  logic        atn_seen;   // ATN was up when we were selected
  logic [7:0]  status;
  logic [7:0]  msg_in;     // what MESSAGE IN will send
  logic [7:0]  msg_rx;     // the last message received

  logic [3:0]  sense_key;
  logic [7:0]  sense_asc, sense_ascq;
  logic [31:0] sense_lba;

  // Where DATA IN sources its bytes.
  localparam logic [2:0] R_NONE     = 3'd0;
  localparam logic [2:0] R_MEDIA    = 3'd1;
  localparam logic [2:0] R_INQUIRY  = 3'd2;
  localparam logic [2:0] R_SENSE    = 3'd3;
  localparam logic [2:0] R_CAPACITY = 3'd4;
  localparam logic [2:0] R_MODE     = 3'd5;

  logic [2:0] resp_kind;

  // ---------------------------------------------------------------------------
  // What we drive
  // ---------------------------------------------------------------------------

  logic connected;    // we own the bus: BSY is ours
  logic [2:0] phase;  // MSG, C/D, I/O - the Target Command Register's encoding
  logic [7:0] data_out;
  logic       drive_bus;

  assign connected = (st != S_IDLE) && (st != S_FREE);

  // The three phases where the target sources data are exactly those with I/O
  // set, which is what I/O means.
  assign drive_bus = connected && phase[0];

  always_comb begin
    drive_o      = '0;
    drive_o.bsy  = connected;
    drive_o.msg  = connected && phase[2];
    drive_o.cd   = connected && phase[1];
    drive_o.io   = connected && phase[0];
    drive_o.req  = connected && (hs == H_REQ);
    drive_o.data = drive_bus ? data_out : 8'h00;
    drive_o.dbp  = drive_bus ? ~(^data_out) : 1'b0;
  end

  // ---------------------------------------------------------------------------
  // Generated responses.
  //
  // These are pure functions of the byte index, so DATA IN can source them
  // without staging anything into the sector buffer first.
  // ---------------------------------------------------------------------------

  logic [31:0] last_lba;
  assign last_lba = (blk_count_i == 32'd0) ? 32'd0 : (blk_count_i - 32'd1);

  logic [2:0] vend_i;
  logic [3:0] prod_i;
  logic [1:0] rev_i;
  assign vend_i = 3'd7  - idx[2:0];
  assign prod_i = 4'd15 - idx[3:0];
  assign rev_i  = 2'd3  - idx[1:0];

  logic [7:0] resp;

  always_comb begin
    resp = 8'h00;
    unique case (resp_kind)
      R_INQUIRY: begin
        // Thirty-six bytes: the standard INQUIRY data every driver reads to
        // decide what it has found.
        if (idx >= 10'd32) resp = REVISION[8*rev_i +: 8];
        else if (idx >= 10'd16) resp = PRODUCT[8*prod_i +: 8];
        else if (idx >= 10'd8) resp = VENDOR[8*vend_i +: 8];
        else begin
          unique case (idx[2:0])
            // A logical unit this device does not have still answers INQUIRY,
            // with peripheral qualifier 3 and device type 0x1f: "nothing
            // here".  Failing the command instead makes some drivers give up
            // on the whole target rather than just the one unit.
            3'd0: resp = (lun == 3'd0) ? 8'h00 : 8'h7f;
            3'd1: resp = 8'h00;   // not removable
            3'd2: resp = 8'h02;   // claims SCSI-2, which SCSI-1 drivers accept
            3'd3: resp = 8'h02;   // standard INQUIRY data format
            3'd4: resp = 8'd31;   // additional length: thirty-six in all
            default: resp = 8'h00;
          endcase
        end
      end

      R_SENSE: begin
        // Extended sense, eighteen bytes.
        unique case (idx[4:0])
          5'd0:  resp = 8'h70;               // current error, no valid LBA
          5'd2:  resp = {4'h0, sense_key};
          5'd3:  resp = sense_lba[31:24];
          5'd4:  resp = sense_lba[23:16];
          5'd5:  resp = sense_lba[15:8];
          5'd6:  resp = sense_lba[7:0];
          5'd7:  resp = 8'd10;               // additional sense length
          5'd12: resp = sense_asc;
          5'd13: resp = sense_ascq;
          default: resp = 8'h00;
        endcase
      end

      R_CAPACITY: begin
        // The last addressable block, then the block length.  It is the last
        // block and not the count, which is the one thing about READ CAPACITY
        // everybody gets wrong once.
        unique case (idx[2:0])
          3'd0: resp = last_lba[31:24];
          3'd1: resp = last_lba[23:16];
          3'd2: resp = last_lba[15:8];
          3'd3: resp = last_lba[7:0];
          3'd6: resp = 8'h02;                // 512, big endian
          default: resp = 8'h00;
        endcase
      end

      R_MODE: begin
        // A four-byte header and one block descriptor, and no pages.  Enough
        // for a driver to learn the geometry and nothing more.
        unique case (idx[3:0])
          4'd0:  resp = 8'd11;               // length of what follows
          4'd3:  resp = 8'd8;                // block descriptor length
          4'd5:  resp = blk_count_i[23:16];
          4'd6:  resp = blk_count_i[15:8];
          4'd7:  resp = blk_count_i[7:0];
          4'd10: resp = 8'h02;               // 512 again
          default: resp = 8'h00;
        endcase
      end

      default: resp = 8'h00;
    endcase
  end

  always_comb begin
    unique case (st)
      S_STATUS: data_out = status;
      S_MSGIN:  data_out = msg_in;
      default:  data_out = (resp_kind == R_MEDIA) ? a_rdata : resp;
    endcase
  end

  // The buffer is addressed by the byte index throughout, so its registered
  // read has settled by the time REQ goes true.  The write happens in the
  // cycle ACK is seen, while the initiator is still driving the byte.
  assign a_addr  = idx[8:0];
  assign a_wdata = bus_i.data;
  assign a_we    = (st == S_DATAOUT) && (hs == H_REQ) && bus_i.ack &&
                   (resp_kind == R_MEDIA);

  // ---------------------------------------------------------------------------
  // Command decode
  // ---------------------------------------------------------------------------

  logic [7:0] op;
  assign op = cdb[7:0];

  // A six-byte command carries a 21-bit address and an eight-bit length, with
  // zero meaning 256 blocks - the one place SCSI-1 counts that way.  An
  // allocation length of zero, by contrast, means zero, so the two cannot
  // share a decode.
  logic [31:0] cdb_lba6, cdb_lba10, cmd_lba;
  logic [23:0] cdb_len6, cdb_len10, cmd_blocks;

  assign cdb_lba6  = {11'd0, cdb[12:8], cdb[23:16], cdb[31:24]};
  assign cdb_len6  = (cdb[39:32] == 8'd0) ? 24'd256 : {16'd0, cdb[39:32]};
  assign cdb_lba10 = {cdb[23:16], cdb[31:24], cdb[39:32], cdb[47:40]};
  assign cdb_len10 = {8'd0, cdb[63:56], cdb[71:64]};

  logic cdb_is_read, cdb_is_write, cdb_is_ten;
  assign cdb_is_read  = (op == C_READ6) || (op == C_READ10);
  assign cdb_is_write = (op == C_WRITE6) || (op == C_WRITE10);
  assign cdb_is_ten   = (op == C_READ10) || (op == C_WRITE10);

  assign cmd_lba    = cdb_is_ten ? cdb_lba10 : cdb_lba6;
  assign cmd_blocks = cdb_is_ten ? cdb_len10 : cdb_len6;

  // How many bytes the initiator is prepared to take back: byte 4 of a
  // six-byte command.  Zero means zero.
  logic [9:0] alloc_len;
  assign alloc_len = {2'd0, cdb[39:32]};

  // The command descriptor block's length comes from the opcode's group code.
  logic [3:0] len_of_op;
  always_comb begin
    unique case (bus_i.data[7:5])
      3'd1,
      3'd2:    len_of_op = 4'd10;
      3'd5:    len_of_op = 4'd12;
      default: len_of_op = 4'd6;
    endcase
  end

  logic sel_match;
  // A target answers a selection, never a reselection: I/O true means another
  // device is reselecting an initiator and this is not for us.
  assign sel_match = bus_i.sel && !bus_i.bsy && !bus_i.io &&
                     ((bus_i.data & ID_MASK) != 8'h00);

  logic media_fault;
  assign media_fault = blk_done_i && blk_err_i;

  // ---------------------------------------------------------------------------
  // The sequencer
  // ---------------------------------------------------------------------------

  always_ff @(posedge clk_i) begin
    if (rst_i) begin
      st          <= S_IDLE;
      hs          <= H_SETUP;
      sel_cnt     <= '0;
      idx         <= '0;
      xfer_len    <= '0;
      cdb         <= '0;
      cdb_len     <= 4'd6;
      lba         <= '0;
      blk_left    <= '0;
      lun         <= '0;
      atn_seen    <= 1'b0;
      status      <= ST_GOOD;
      msg_in      <= M_COMMAND_COMPLETE;
      msg_rx      <= '0;
      resp_kind   <= R_NONE;
      sense_key   <= SK_NO_SENSE;
      sense_asc   <= 8'h00;
      sense_ascq  <= 8'h00;
      sense_lba   <= '0;
      blk_start_o <= 1'b0;
      blk_we_o    <= 1'b0;
      blk_lba_o   <= '0;
    end else begin
      blk_start_o <= 1'b0;

      // A bus reset outranks everything: release the bus and forget the
      // command.  This is the target's side of what the 5380 does with RST.
      if (bus_i.rst) begin
        st      <= S_IDLE;
        hs      <= H_SETUP;
        sel_cnt <= '0;
      end else begin
        unique case (st)
          // ---- waiting to be selected -------------------------------------
          S_IDLE: begin
            if (sel_match) begin
              if (sel_cnt == N_SETTLE) begin
                st       <= S_SELWAIT;
                atn_seen <= bus_i.atn;
                lun      <= '0;
                status   <= ST_GOOD;
                msg_in   <= M_COMMAND_COMPLETE;
                idx      <= '0;
              end else begin
                sel_cnt <= sel_cnt + 1'b1;
              end
            end else begin
              sel_cnt <= '0;
            end
          end

          // ---- selected: BSY is ours, wait for the initiator to drop SEL ---
          S_SELWAIT: begin
            // Linux raises ATN again after it sees BSY and before the first
            // REQ, so it is watched here as well as at selection.
            if (bus_i.atn) atn_seen <= 1'b1;
            if (!bus_i.sel) begin
              hs  <= H_SETUP;
              idx <= '0;
              st  <= (bus_i.atn || atn_seen) ? S_MSGOUT : S_CMD;
            end
          end

          // ---- message out -------------------------------------------------
          //
          // The initiator drops ATN before acknowledging the last byte of the
          // message, which is how a target knows when to stop asking.
          S_MSGOUT: begin
            unique case (hs)
              H_SETUP: hs <= H_REQ;
              H_REQ: if (bus_i.ack) begin
                hs     <= H_REL;
                msg_rx <= bus_i.data;
                // IDENTIFY carries the logical unit in its low three bits.
                if (bus_i.data[7]) lun <= bus_i.data[2:0];
              end
              default: if (!bus_i.ack) begin
                hs <= H_SETUP;
                if (msg_rx == M_ABORT || msg_rx == M_BUS_DEVICE_RESET) begin
                  // No status and no message: the bus just goes free, which
                  // is what do_abort in NCR5380.c expects to happen next.
                  st <= S_FREE;
                end else if (!bus_i.atn) begin
                  idx <= '0;
                  st  <= S_CMD;
                end
              end
            endcase
          end

          // ---- command -----------------------------------------------------
          S_CMD: begin
            unique case (hs)
              H_SETUP: hs <= H_REQ;
              H_REQ: if (bus_i.ack) begin
                hs <= H_REL;
                cdb[8*idx[3:0] +: 8] <= bus_i.data;
                // The length is known as soon as the opcode is in, so it is
                // taken from the bus rather than from the register that has
                // not been written yet.
                if (idx == 10'd0) cdb_len <= len_of_op;
                // SCSI-1 also carries the logical unit in the top three bits
                // of byte 1, for initiators that send no IDENTIFY.
                if (idx == 10'd1 && !atn_seen) lun <= bus_i.data[7:5];
              end
              default: if (!bus_i.ack) begin
                hs <= H_SETUP;
                if (idx + 10'd1 == {6'd0, cdb_len}) begin
                  st  <= S_EXEC;
                  idx <= '0;
                end else begin
                  idx <= idx + 10'd1;
                end
              end
            endcase
          end

          // ---- decide what the command means -------------------------------
          S_EXEC: begin
            hs        <= H_SETUP;
            idx       <= '0;
            blk_left  <= '0;
            resp_kind <= R_NONE;
            status    <= ST_GOOD;
            // Sense is about the command before this one, and every command
            // but REQUEST SENSE itself clears it.
            if (op != C_REQUEST_SENSE) begin
              sense_key  <= SK_NO_SENSE;
              sense_asc  <= 8'h00;
              sense_ascq <= 8'h00;
              sense_lba  <= '0;
            end

            if (lun != 3'd0 && op != C_INQUIRY && op != C_REQUEST_SENSE) begin
              // Everything but INQUIRY and REQUEST SENSE is refused for a
              // logical unit that is not there.
              sense_key  <= SK_ILLEGAL_REQUEST;
              sense_asc  <= ASC_LUN_NOT_SUPPORTED;
              sense_ascq <= 8'h00;
              status     <= ST_CHECK;
              st         <= S_STATUS;
            end else begin
              unique case (op)
                C_INQUIRY: begin
                  resp_kind <= R_INQUIRY;
                  xfer_len  <= (alloc_len < 10'd36) ? alloc_len : 10'd36;
                  st <= (alloc_len == 10'd0) ? S_STATUS : S_DATAIN;
                end

                C_REQUEST_SENSE: begin
                  resp_kind <= R_SENSE;
                  xfer_len  <= (alloc_len < 10'd18) ? alloc_len : 10'd18;
                  st <= (alloc_len == 10'd0) ? S_STATUS : S_DATAIN;
                end

                C_READ_CAPACITY: begin
                  if (!blk_ready_i) begin
                    sense_key  <= SK_NOT_READY;
                    sense_asc  <= ASC_MEDIUM_NOT_PRESENT;
                    sense_ascq <= 8'h00;
                    status     <= ST_CHECK;
                    st         <= S_STATUS;
                  end else begin
                    resp_kind <= R_CAPACITY;
                    xfer_len  <= 10'd8;
                    st        <= S_DATAIN;
                  end
                end

                C_MODE_SENSE6: begin
                  resp_kind <= R_MODE;
                  xfer_len  <= (alloc_len < 10'd12) ? alloc_len : 10'd12;
                  st <= (alloc_len == 10'd0) ? S_STATUS : S_DATAIN;
                end

                C_TEST_UNIT_READY: begin
                  if (!blk_ready_i) begin
                    sense_key  <= SK_NOT_READY;
                    sense_asc  <= ASC_MEDIUM_NOT_PRESENT;
                    sense_ascq <= 8'h00;
                    status     <= ST_CHECK;
                  end
                  st <= S_STATUS;
                end

                // Commands with nothing to do here that must nevertheless
                // succeed, or a driver decides the disk is broken.
                C_REZERO, C_SEEK6, C_SEEK10, C_START_STOP, C_PREVENT_ALLOW,
                C_SEND_DIAGNOSTIC, C_VERIFY10, C_FORMAT_UNIT,
                C_RESERVE, C_RELEASE: st <= S_STATUS;

                // MODE SELECT is taken and thrown away: there is no geometry
                // here to change.
                C_MODE_SELECT6: begin
                  if (alloc_len == 10'd0) begin
                    st <= S_STATUS;
                  end else begin
                    xfer_len <= alloc_len;
                    st       <= S_DATAOUT;
                  end
                end

                default: begin
                  if (!cdb_is_read && !cdb_is_write) begin
                    sense_key  <= SK_ILLEGAL_REQUEST;
                    sense_asc  <= ASC_INVALID_COMMAND;
                    sense_ascq <= 8'h00;
                    status     <= ST_CHECK;
                    st         <= S_STATUS;
                  end else if (!blk_ready_i) begin
                    sense_key  <= SK_NOT_READY;
                    sense_asc  <= ASC_MEDIUM_NOT_PRESENT;
                    sense_ascq <= 8'h00;
                    status     <= ST_CHECK;
                    st         <= S_STATUS;
                  end else if (({8'd0, cmd_blocks} + cmd_lba) > blk_count_i) begin
                    sense_key  <= SK_ILLEGAL_REQUEST;
                    sense_asc  <= ASC_LBA_OUT_OF_RANGE;
                    sense_ascq <= 8'h00;
                    sense_lba  <= cmd_lba;
                    status     <= ST_CHECK;
                    st         <= S_STATUS;
                  end else begin
                    lba       <= cmd_lba;
                    blk_left  <= cmd_blocks;
                    resp_kind <= R_MEDIA;
                    xfer_len  <= 10'd512;
                    if (cdb_is_read) begin
                      blk_lba_o   <= cmd_lba;
                      blk_we_o    <= 1'b0;
                      blk_start_o <= 1'b1;
                      st          <= S_RDWAIT;
                    end else begin
                      st <= S_DATAOUT;
                    end
                  end
                end
              endcase
            end
          end

          // ---- the back end filling the buffer ------------------------------
          S_RDWAIT: begin
            if (media_fault) begin
              sense_key  <= SK_MEDIUM_ERROR;
              sense_asc  <= ASC_UNRECOVERED_READ;
              sense_ascq <= 8'h00;
              sense_lba  <= lba;
              status     <= ST_CHECK;
              st         <= S_STATUS;
            end else if (blk_done_i) begin
              idx <= '0;
              hs  <= H_SETUP;
              st  <= S_DATAIN;
            end
          end

          // ---- the back end draining it -------------------------------------
          S_WRWAIT: begin
            if (media_fault) begin
              sense_key  <= SK_MEDIUM_ERROR;
              sense_asc  <= ASC_UNRECOVERED_READ;
              sense_ascq <= 8'h00;
              sense_lba  <= lba;
              status     <= ST_CHECK;
              st         <= S_STATUS;
            end else if (blk_done_i) begin
              if (blk_left == 24'd1) begin
                st <= S_STATUS;
              end else begin
                blk_left <= blk_left - 24'd1;
                lba      <= lba + 32'd1;
                idx      <= '0;
                hs       <= H_SETUP;
                st       <= S_DATAOUT;
              end
            end
          end

          // ---- data in --------------------------------------------------------
          S_DATAIN: begin
            unique case (hs)
              H_SETUP: hs <= H_REQ;
              H_REQ: if (bus_i.ack) hs <= H_REL;
              default: if (!bus_i.ack) begin
                // Every exit from a byte leaves the handshake at SETUP: a
                // phase entered with it still at REL takes its own release
                // branch before ever asserting REQ, and the phase is skipped.
                hs <= H_SETUP;
                if (idx + 10'd1 == xfer_len) begin
                  idx <= '0;
                  if (resp_kind == R_MEDIA && blk_left != 24'd1) begin
                    blk_left    <= blk_left - 24'd1;
                    lba         <= lba + 32'd1;
                    blk_lba_o   <= lba + 32'd1;
                    blk_we_o    <= 1'b0;
                    blk_start_o <= 1'b1;
                    st          <= S_RDWAIT;
                  end else begin
                    st <= S_STATUS;
                  end
                end else begin
                  idx <= idx + 10'd1;
                end
              end
            endcase
          end

          // ---- data out --------------------------------------------------------
          S_DATAOUT: begin
            unique case (hs)
              H_SETUP: hs <= H_REQ;
              H_REQ: if (bus_i.ack) hs <= H_REL;
              default: if (!bus_i.ack) begin
                hs <= H_SETUP;
                if (idx + 10'd1 == xfer_len) begin
                  idx <= '0;
                  if (resp_kind == R_MEDIA) begin
                    blk_lba_o   <= lba;
                    blk_we_o    <= 1'b1;
                    blk_start_o <= 1'b1;
                    st          <= S_WRWAIT;
                  end else begin
                    st <= S_STATUS;
                  end
                end else begin
                  idx <= idx + 10'd1;
                end
              end
            endcase
          end

          // ---- status ----------------------------------------------------------
          S_STATUS: begin
            unique case (hs)
              H_SETUP: hs <= H_REQ;
              H_REQ: if (bus_i.ack) hs <= H_REL;
              default: if (!bus_i.ack) begin
                hs     <= H_SETUP;
                msg_in <= M_COMMAND_COMPLETE;
                st     <= S_MSGIN;
              end
            endcase
          end

          // ---- message in, and the end of the command --------------------------
          S_MSGIN: begin
            unique case (hs)
              H_SETUP: hs <= H_REQ;
              H_REQ: if (bus_i.ack) hs <= H_REL;
              default: if (!bus_i.ack) begin
                hs <= H_SETUP;
                st <= S_FREE;
                // Sense is preserved "until retrieved by a REQUEST SENSE
                // command or until the next command": retrieving it is what
                // consumes it, so the clear happens here and not at the top
                // of the command, where it would erase the very thing the
                // driver is asking about.
                if (op == C_REQUEST_SENSE) begin
                  sense_key  <= SK_NO_SENSE;
                  sense_asc  <= 8'h00;
                  sense_ascq <= 8'h00;
                  sense_lba  <= '0;
                end
              end
            endcase
          end

          // ---- release the bus --------------------------------------------------
          default: begin
            hs      <= H_SETUP;
            sel_cnt <= '0;
            msg_rx  <= '0;
            st      <= S_IDLE;
          end
        endcase
      end
    end
  end

  // The phase lines, by state.  DATA OUT is zero, which is also what the bus
  // reads as with nobody driving them; on a real bus the two are
  // indistinguishable as well.
  always_comb begin
    unique case (st)
      S_MSGOUT:  phase = 3'b110;  // MSG, C/D
      S_CMD:     phase = 3'b010;  //      C/D
      S_DATAIN:  phase = 3'b001;  //                I/O
      S_STATUS:  phase = 3'b011;  //      C/D,      I/O
      S_MSGIN:   phase = 3'b111;  // MSG, C/D,      I/O
      default:   phase = 3'b000;  // DATA OUT, and the idle states
    endcase
  end

endmodule

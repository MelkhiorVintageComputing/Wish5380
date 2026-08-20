// SPDX-License-Identifier: MIT
//
// An SD or micro-SD card in SPI mode, behind the block interface the SCSI
// target reaches storage through.
//
// The target asks for one 512-byte block at a time and never sees a card: it
// starts a transfer, waits for `blk_o.done`, and finds the block in the sector
// buffer this module fills or drains through its own port.  That is what lets
// an SD 4-bit layer replace this one later without the SCSI side noticing,
// and what lets the regression test SCSI without simulating a card.
//
// The initialisation sequence is the one every SPI-mode driver performs:
//
//   * at least 74 clocks with the card deselected and the line held high, so
//     it finishes its internal power-up;
//   * CMD0 to put it into SPI mode, which arrives while the card is still
//     checking CRCs;
//   * CMD8 to ask whether it understands the 2.00 spec.  A card that answers
//     "illegal command" is a version 1 card and is addressed in bytes;
//   * ACMD41 until it stops reporting idle, which is where the seconds go: a
//     card may take most of one to come up;
//   * CMD58 to read the OCR, whose Card Capacity Status bit says whether
//     blocks are addressed by number or by byte offset;
//   * CMD16 to fix the block length at 512, which a high-capacity card
//     ignores because it has no other length;
//   * CMD9 to read the CSD, which is where the capacity comes from.
//
// CRC7 is computed for every command.  A card in SPI mode does not check it
// until CMD59 turns checking on, so the two constants every small driver
// hard-codes - 0x95 for CMD0, 0x87 for CMD8 - would be enough for a card in
// its default state; but Linux and U-Boot both compute it for every command
// anyway, and a host that does not is a host that breaks the moment checking
// is on.  doc/drivers/SD/README.md sets the three drivers side by side.  The
// CRC16 on read data *is* checked, because it is the only thing standing
// between a marginal card and a silently corrupt sector.

module blk_sd #(
  parameter int CLK_PERIOD_PS = 20000
) (
  input  logic clk_i,
  input  logic rst_i,

  // ---- the block interface, as `scsi_targ` sees it -----------------------
  //
  // `doc/block.md` is the contract.  The sector buffer lives in the target,
  // so `blk_i.buf_rdata` is the answer to `blk_o.buf_addr` and arrives one
  // cycle after it.
  input  blk_req_t blk_i,
  output blk_rsp_t blk_o,

  // ---- the card ----------------------------------------------------------
  output logic sd_clk_o,
  output logic sd_cs_n_o,
  output logic sd_mosi_o,
  input  logic sd_miso_i
);

  // ---------------------------------------------------------------------------
  // The response, assembled at the port.
  //
  // The sequencer drives these as ordinary signals and they are gathered into
  // `blk_o` here, rather than the sequencer writing struct fields directly.
  // Two of them come from continuous assignments and five from the state
  // machine, and a struct with both kinds of driver on it is multiply driven
  // as far as a linter is concerned even though no bit of it is.
  // ---------------------------------------------------------------------------

  logic        blk_done, blk_err, blk_ready;
  logic [31:0] blk_count;
  logic        sbuf_we;
  logic [8:0]  sbuf_addr;
  logic [7:0]  sbuf_wdata;

  always_comb begin
    blk_o.done      = blk_done;
    blk_o.err       = blk_err;
    blk_o.ready     = blk_ready;
    blk_o.count     = blk_count;
    blk_o.buf_we    = sbuf_we;
    blk_o.buf_addr  = sbuf_addr;
    blk_o.buf_wdata = sbuf_wdata;
  end

  // ---------------------------------------------------------------------------
  // Clocking.
  //
  // A card must see no more than 400 kHz until initialisation is over, and
  // will take 25 MHz afterwards.  Both are derived from the system clock so a
  // build for a slower machine stays legal.
  // ---------------------------------------------------------------------------

  // Half period in system clocks, less one, because the divider counts to it.
  localparam int I_SLOW = (1_250_000 + CLK_PERIOD_PS - 1) / CLK_PERIOD_PS - 1;
  localparam int I_FAST = (20_000 + CLK_PERIOD_PS - 1) / CLK_PERIOD_PS - 1;
  localparam logic [15:0] DIV_SLOW = I_SLOW[15:0];
  localparam logic [15:0] DIV_FAST = (I_FAST < 0) ? 16'd0 : I_FAST[15:0];

  localparam int I_MS = 1_000_000_000 / CLK_PERIOD_PS;
  localparam logic [31:0] TICKS_PER_MS = I_MS[31:0];

  // ---------------------------------------------------------------------------
  // The transceiver
  // ---------------------------------------------------------------------------

  logic [15:0] div;
  logic        cs, spi_go, spi_busy, spi_done;
  logic [7:0]  spi_tx, spi_rx;

  sd_spi u_spi (
    .clk_i  (clk_i),
    .rst_i  (rst_i),
    .div_i  (div),
    .cs_i   (cs),
    .go_i   (spi_go),
    .tx_i   (spi_tx),
    .rx_o   (spi_rx),
    .done_o (spi_done),
    .busy_o (spi_busy),
    .sclk_o (sd_clk_o),
    .mosi_o (sd_mosi_o),
    .cs_n_o (sd_cs_n_o),
    .miso_i (sd_miso_i)
  );

  // The whole sequencer runs only while the transceiver is idle, so every
  // state issues one byte and the next visit sees the answer in `spi_rx`.
  logic spi_idle;
  assign spi_idle = !spi_go && !spi_busy && !spi_done;

  // ---------------------------------------------------------------------------
  // CRC16-CCITT, over the data of a read block.  x^16 + x^12 + x^5 + 1,
  // preset to zero, most significant bit first - which is what the card
  // computes over the same bytes and sends after them.
  // ---------------------------------------------------------------------------

  function automatic logic [15:0] crc16_byte(input logic [15:0] c,
                                             input logic [7:0] d);
    logic [15:0] r;
    integer i;
    begin
      r = c;
      for (i = 0; i < 8; i = i + 1) begin
        if ((r[15] ^ d[7 - i]) == 1'b1) r = {r[14:0], 1'b0} ^ 16'h1021;
        else                            r = {r[14:0], 1'b0};
      end
      crc16_byte = r;
    end
  endfunction

  // ---------------------------------------------------------------------------
  // CRC7, over the five bytes that precede it in a command.  x^7 + x^3 + 1,
  // preset to zero, most significant bit first; the frame carries it shifted
  // up by one with the stop bit underneath.
  //
  // It is computed for every command rather than written down for the two the
  // card is still checking.  That is what Linux and U-Boot both do - "crc7
  // (plus end bit) ... always computed, it's cheap"
  // (doc/drivers/SD/Linux/mmc_spi.c:415) - and it is the only version that
  // survives a card with CRC checking switched on by CMD59.  The two famous
  // constants fall out of it: 0x95 for CMD0 and 0x87 for CMD8(0x1AA), which
  // is what layout_the_two_known_command_crcs checks.
  // ---------------------------------------------------------------------------

  function automatic logic [6:0] crc7_byte(input logic [6:0] c,
                                           input logic [7:0] d);
    logic [6:0] r;
    integer i;
    begin
      r = c;
      for (i = 0; i < 8; i = i + 1) begin
        if ((r[6] ^ d[7 - i]) == 1'b1) r = {r[5:0], 1'b0} ^ 7'h09;
        else                           r = {r[5:0], 1'b0};
      end
      crc7_byte = r;
    end
  endfunction

  // ---------------------------------------------------------------------------
  // States
  // ---------------------------------------------------------------------------

  localparam logic [5:0] S_POWER    = 6'd0;   // let the card wake up
  localparam logic [5:0] S_DUMMY    = 6'd1;   // 74+ clocks, deselected
  localparam logic [5:0] S_CMD0     = 6'd2;
  localparam logic [5:0] S_CMD0_R   = 6'd3;
  localparam logic [5:0] S_CMD8     = 6'd4;
  localparam logic [5:0] S_CMD8_R   = 6'd5;
  localparam logic [5:0] S_CMD55    = 6'd6;
  localparam logic [5:0] S_CMD55_R  = 6'd7;
  localparam logic [5:0] S_ACMD41   = 6'd8;
  localparam logic [5:0] S_ACMD41_R = 6'd9;
  localparam logic [5:0] S_CMD58    = 6'd10;
  localparam logic [5:0] S_CMD58_R  = 6'd11;
  localparam logic [5:0] S_CMD16    = 6'd12;
  localparam logic [5:0] S_CMD16_R  = 6'd13;
  localparam logic [5:0] S_CMD9     = 6'd14;
  localparam logic [5:0] S_CMD9_R   = 6'd15;
  localparam logic [5:0] S_CSD_TOK  = 6'd16;
  localparam logic [5:0] S_CSD_DAT  = 6'd17;
  localparam logic [5:0] S_CSD_CRC  = 6'd18;
  localparam logic [5:0] S_READY    = 6'd19;

  localparam logic [5:0] S_RD_CMD   = 6'd20;
  localparam logic [5:0] S_RD_CMD_R = 6'd21;
  localparam logic [5:0] S_RD_TOK   = 6'd22;
  localparam logic [5:0] S_RD_DAT   = 6'd23;
  localparam logic [5:0] S_RD_CRC   = 6'd24;

  localparam logic [5:0] S_WR_CMD   = 6'd25;
  localparam logic [5:0] S_WR_CMD_R = 6'd26;
  localparam logic [5:0] S_WR_PRE   = 6'd27;
  localparam logic [5:0] S_WR_TOK   = 6'd28;
  localparam logic [5:0] S_WR_DAT   = 6'd29;
  localparam logic [5:0] S_WR_CRC   = 6'd30;
  localparam logic [5:0] S_WR_RESP  = 6'd31;
  localparam logic [5:0] S_WR_BUSY  = 6'd32;

  localparam logic [5:0] S_FINISH   = 6'd33;  // pulse blk_done
  localparam logic [5:0] S_DEAD     = 6'd34;  // no usable card

  // The command helper, which every command above jumps into and comes back
  // from through `ret`.
  localparam logic [5:0] C_SEND     = 6'd35;
  localparam logic [5:0] C_R1       = 6'd36;
  localparam logic [5:0] C_EXTRA    = 6'd37;

  logic [5:0] st, ret;

  logic [5:0]  cmd_idx;
  logic [31:0] cmd_arg;
  logic [7:0]  cmd_crc;
  logic [6:0]  cmd_crc7;
  logic        cmd_extra;      // the response carries four more bytes
  logic [7:0]  r1;

  logic [9:0]  bcnt;           // bytes within whatever is being moved
  logic [15:0] crc16;

  // The four-byte tail of an R3 or R7 response, and the card's 128-bit CSD.
  // Both are registers the card defines, and this only reads the handful of
  // fields it needs out of them - the capacity, the check pattern, and the
  // Card Capacity Status bit.  The rest is reserved or is about voltages
  // nobody here can change.
  /* verilator lint_off UNUSEDSIGNAL */
  logic [31:0]  r_extra;
  logic [127:0] csd;
  /* verilator lint_on UNUSEDSIGNAL */

  logic v2, ccs;               // a 2.00 card; block rather than byte addressed
  logic card_ready;

  logic [31:0] tick_cnt;
  logic [15:0] ms;             // milliseconds since the timeout was armed
  logic        ms_pulse;

  assign blk_ready = card_ready;

  // ---------------------------------------------------------------------------
  // Capacity, from the CSD.
  //
  // Two layouts, and the difference is not a detail: a version 1 card states a
  // size and a multiplier and a block length, and a version 2 card states a
  // count of half-megabytes.  Getting the wrong one gives a disk the right
  // shape and the wrong size.
  // ---------------------------------------------------------------------------

  logic [1:0]  csd_ver;
  logic [21:0] c_size_v2;
  logic [11:0] c_size_v1;
  logic [2:0]  c_mult;
  logic [3:0]  rd_bl_len;
  logic [31:0] blocks;

  assign csd_ver   = csd[127:126];
  assign c_size_v2 = csd[69:48];
  assign c_size_v1 = csd[73:62];
  assign c_mult    = csd[49:47];
  assign rd_bl_len = csd[83:80];

  always_comb begin
    if (csd_ver == 2'b01) begin
      // (C_SIZE + 1) half-megabytes, which is (C_SIZE + 1) * 1024 blocks.
      blocks = ({10'd0, c_size_v2} + 32'd1) << 10;
    end else begin
      // (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) blocks of 2^READ_BL_LEN bytes,
      // restated in 512-byte blocks.
      blocks = ({20'd0, c_size_v1} + 32'd1) <<
               ({29'd0, c_mult} + 32'd2 + {28'd0, rd_bl_len} - 32'd9);
    end
  end

  assign blk_count = card_ready ? blocks : 32'd0;

  // ---------------------------------------------------------------------------
  // The next byte of a command, by position.
  // ---------------------------------------------------------------------------

  always_comb begin
    logic [6:0] c;
    c = 7'd0;
    c = crc7_byte(c, {2'b01, cmd_idx});
    c = crc7_byte(c, cmd_arg[31:24]);
    c = crc7_byte(c, cmd_arg[23:16]);
    c = crc7_byte(c, cmd_arg[15:8]);
    c = crc7_byte(c, cmd_arg[7:0]);
    cmd_crc7 = c;
  end
  assign cmd_crc = {cmd_crc7, 1'b1};

  logic [7:0] cmd_byte;
  always_comb begin
    unique case (bcnt[2:0])
      3'd0:    cmd_byte = {2'b01, cmd_idx};
      3'd1:    cmd_byte = cmd_arg[31:24];
      3'd2:    cmd_byte = cmd_arg[23:16];
      3'd3:    cmd_byte = cmd_arg[15:8];
      3'd4:    cmd_byte = cmd_arg[7:0];
      default: cmd_byte = cmd_crc;
    endcase
  end

  // ---------------------------------------------------------------------------
  // The sequencer
  // ---------------------------------------------------------------------------

  always_ff @(posedge clk_i) begin
    spi_go   <= 1'b0;
    sbuf_we <= 1'b0;
    blk_done   <= 1'b0;

    // One pulse per millisecond, which is what every timeout here is counted
    // in.  A card may take most of a second to finish initialising and a
    // quarter of one to finish a write.
    if (tick_cnt >= TICKS_PER_MS - 32'd1) begin
      tick_cnt <= '0;
      ms_pulse <= 1'b1;
    end else begin
      tick_cnt <= tick_cnt + 32'd1;
      ms_pulse <= 1'b0;
    end
    if (ms_pulse) ms <= ms + 16'd1;

    if (rst_i) begin
      st         <= S_POWER;
      ret        <= S_POWER;
      div        <= DIV_SLOW;
      cs         <= 1'b0;
      spi_tx     <= 8'hff;
      cmd_idx    <= '0;
      cmd_arg    <= '0;
      cmd_extra  <= 1'b0;
      r1         <= 8'hff;
      r_extra    <= '0;
      bcnt       <= '0;
      crc16      <= '0;
      csd        <= '0;
      v2         <= 1'b0;
      ccs        <= 1'b0;
      card_ready <= 1'b0;
      blk_err      <= 1'b0;
      sbuf_addr <= '0;
      sbuf_wdata <= '0;
      tick_cnt   <= '0;
      ms         <= '0;
      ms_pulse   <= 1'b0;
    end else if (spi_idle) begin
      unique case (st)
        // ---- power up ------------------------------------------------------
        S_POWER: begin
          cs <= 1'b0;
          if (ms >= 16'd2) begin
            ms   <= '0;
            bcnt <= '0;
            st   <= S_DUMMY;
          end
        end

        // Ten bytes with the card deselected is eighty clocks, which covers
        // the seventy-four the card asks for.
        S_DUMMY: begin
          spi_tx <= 8'hff;
          spi_go <= 1'b1;
          bcnt   <= bcnt + 10'd1;
          if (bcnt == 10'd9) begin
            cs   <= 1'b1;
            ms   <= '0;
            st   <= S_CMD0;
          end
        end

        // ---- CMD0: into SPI mode -------------------------------------------
        S_CMD0: begin
          cmd_idx   <= 6'd0;
          cmd_arg   <= 32'd0;
          cmd_extra <= 1'b0;
          bcnt      <= '0;
          ret       <= S_CMD0_R;
          st        <= C_SEND;
        end

        S_CMD0_R: begin
          if (r1 == 8'h01) begin
            ms <= '0;
            st <= S_CMD8;
          end else if (ms >= 16'd500) begin
            st <= S_DEAD;       // nothing is answering
          end else begin
            st <= S_CMD0;       // try again
          end
        end

        // ---- CMD8: does it know the 2.00 spec? -----------------------------
        S_CMD8: begin
          cmd_idx   <= 6'd8;
          cmd_arg   <= 32'h0000_01aa;   // 2.7-3.6 V, check pattern 0xAA
          cmd_extra <= 1'b1;
          bcnt      <= '0;
          ret       <= S_CMD8_R;
          st        <= C_SEND;
        end

        S_CMD8_R: begin
          // Bit 2 of R1 is "illegal command", which is how a version 1 card
          // says it has never heard of CMD8.
          if (r1[2]) begin
            v2 <= 1'b0;
            st <= S_CMD55;
          end else if (r_extra[11:0] == 12'h1aa) begin
            v2 <= 1'b1;
            st <= S_CMD55;
          end else begin
            st <= S_DEAD;       // answered, but not with our check pattern
          end
          ms <= '0;
        end

        // ---- ACMD41 until it comes out of idle -----------------------------
        S_CMD55: begin
          cmd_idx   <= 6'd55;
          cmd_arg   <= 32'd0;
          cmd_extra <= 1'b0;
          bcnt      <= '0;
          ret       <= S_CMD55_R;
          st        <= C_SEND;
        end

        S_CMD55_R: st <= S_ACMD41;

        S_ACMD41: begin
          cmd_idx   <= 6'd41;
          // The host capacity support bit, which a version 2 card needs to see
          // before it will admit to being high capacity.
          cmd_arg   <= v2 ? 32'h4000_0000 : 32'd0;
          cmd_extra <= 1'b0;
          bcnt      <= '0;
          ret       <= S_ACMD41_R;
          st        <= C_SEND;
        end

        S_ACMD41_R: begin
          if (r1 == 8'h00) begin
            ms <= '0;
            st <= v2 ? S_CMD58 : S_CMD16;
          end else if (ms >= 16'd1000) begin
            st <= S_DEAD;       // a second is longer than any card needs
          end else begin
            st <= S_CMD55;
          end
        end

        // ---- CMD58: block addressed, or byte addressed? --------------------
        S_CMD58: begin
          cmd_idx   <= 6'd58;
          cmd_arg   <= 32'd0;
          cmd_extra <= 1'b1;
          bcnt      <= '0;
          ret       <= S_CMD58_R;
          st        <= C_SEND;
        end

        S_CMD58_R: begin
          // Card Capacity Status.  Set means the card is addressed by block
          // number; clear means by byte offset, and the offset has to be
          // multiplied out on every access.
          ccs <= r_extra[30];
          st  <= r_extra[30] ? S_CMD9 : S_CMD16;
        end

        // ---- CMD16: fix the block length -----------------------------------
        S_CMD16: begin
          cmd_idx   <= 6'd16;
          cmd_arg   <= 32'd512;
          cmd_extra <= 1'b0;
          bcnt      <= '0;
          ret       <= S_CMD16_R;
          st        <= C_SEND;
        end

        S_CMD16_R: st <= (r1 == 8'h00) ? S_CMD9 : S_DEAD;

        // ---- CMD9: the CSD, which is where the size is ----------------------
        S_CMD9: begin
          cmd_idx   <= 6'd9;
          cmd_arg   <= 32'd0;
          cmd_extra <= 1'b0;
          bcnt      <= '0;
          ret       <= S_CMD9_R;
          st        <= C_SEND;
        end

        S_CMD9_R: begin
          ms <= '0;
          st <= (r1 == 8'h00) ? S_CSD_TOK : S_DEAD;
          // A byte has to be clocked before the token poll looks at anything,
          // or it examines the response byte it has already read and takes a
          // perfectly good R1 of zero for an error token.
          spi_tx <= 8'hff;
          spi_go <= 1'b1;
        end

        S_CSD_TOK: begin
          if (spi_rx == 8'hfe) begin
            bcnt <= '0;
            st   <= S_CSD_DAT;
          end else if (spi_rx[7:4] == 4'h0) begin
            st <= S_DEAD;                    // an error token
          end else if (ms >= 16'd200) begin
            st <= S_DEAD;
          end
          spi_tx <= 8'hff;
          spi_go <= 1'b1;
        end

        S_CSD_DAT: begin
          csd  <= {csd[119:0], spi_rx};
          bcnt <= bcnt + 10'd1;
          if (bcnt == 10'd15) begin
            bcnt <= '0;
            st   <= S_CSD_CRC;
          end
          spi_tx <= 8'hff;
          spi_go <= 1'b1;
        end

        S_CSD_CRC: begin
          bcnt <= bcnt + 10'd1;
          if (bcnt == 10'd1) begin
            card_ready <= 1'b1;
            div        <= DIV_FAST;   // initialisation is over
            st         <= S_READY;
          end else begin
            spi_tx <= 8'hff;
            spi_go <= 1'b1;
          end
        end

        // ---- idle -----------------------------------------------------------
        S_READY: begin
          blk_err <= 1'b0;
          if (blk_i.start) begin
            // A high-capacity card is addressed by block, everything else by
            // byte offset.
            cmd_arg <= ccs ? blk_i.lba : (blk_i.lba << 9);
            ms      <= '0;
            st      <= blk_i.we ? S_WR_CMD : S_RD_CMD;
          end
        end

        // ---- read -----------------------------------------------------------
        S_RD_CMD: begin
          cmd_idx   <= 6'd17;
          cmd_extra <= 1'b0;
          bcnt      <= '0;
          ret       <= S_RD_CMD_R;
          st        <= C_SEND;
        end

        S_RD_CMD_R: begin
          ms <= '0;
          if (r1 != 8'h00) begin
            blk_err <= 1'b1;
            st    <= S_FINISH;
          end else begin
            st <= S_RD_TOK;
          end
          spi_tx <= 8'hff;
          spi_go <= 1'b1;
        end

        S_RD_TOK: begin
          if (spi_rx == 8'hfe) begin
            bcnt       <= '0;
            crc16      <= '0;
            sbuf_addr <= '0;
            st         <= S_RD_DAT;
            spi_tx     <= 8'hff;
            spi_go     <= 1'b1;
          end else if (spi_rx[7:4] == 4'h0) begin
            blk_err <= 1'b1;      // the card refused: address, or a bad sector
            st    <= S_FINISH;
          end else if (ms >= 16'd200) begin
            blk_err <= 1'b1;
            st    <= S_FINISH;
          end else begin
            spi_tx <= 8'hff;
            spi_go <= 1'b1;
          end
        end

        S_RD_DAT: begin
          sbuf_addr  <= bcnt[8:0];
          sbuf_wdata <= spi_rx;
          sbuf_we    <= 1'b1;
          crc16       <= crc16_byte(crc16, spi_rx);
          bcnt        <= bcnt + 10'd1;
          if (bcnt == 10'd511) begin
            bcnt <= '0;
            st   <= S_RD_CRC;
          end
          spi_tx <= 8'hff;
          spi_go <= 1'b1;
        end

        S_RD_CRC: begin
          // The two bytes the card computed over the same data.  In SPI mode
          // nothing obliges the host to look at them, which is exactly why
          // this does: it is the only check on the sector between the card
          // and the SCSI bus.
          //
          // They are fed through the same register rather than compared
          // against it, because a CRC taken over the data and its own CRC
          // comes out zero.  That is a property of this polynomial with no
          // preset and no final inversion, and it saves holding both values.
          crc16 <= crc16_byte(crc16, spi_rx);
          bcnt  <= bcnt + 10'd1;
          if (bcnt == 10'd1) begin
            if (crc16_byte(crc16, spi_rx) != 16'h0000) blk_err <= 1'b1;
            st <= S_FINISH;
          end else begin
            spi_tx <= 8'hff;
            spi_go <= 1'b1;
          end
        end

        // ---- write ----------------------------------------------------------
        S_WR_CMD: begin
          cmd_idx   <= 6'd24;
          cmd_extra <= 1'b0;
          bcnt      <= '0;
          ret       <= S_WR_CMD_R;
          st        <= C_SEND;
        end

        S_WR_CMD_R: begin
          if (r1 != 8'h00) begin
            blk_err <= 1'b1;
            st    <= S_FINISH;
          end else begin
            // One idle byte between the response and the data packet, and the
            // buffer's address set so its registered read has settled long
            // before the first data byte is wanted.
            sbuf_addr <= '0;
            st         <= S_WR_PRE;
            spi_tx     <= 8'hff;
            spi_go     <= 1'b1;
          end
        end

        S_WR_PRE: begin
          bcnt   <= '0;
          crc16  <= '0;
          st     <= S_WR_TOK;
          spi_tx <= 8'hfe;      // single block start token
          spi_go <= 1'b1;
        end

        S_WR_TOK: begin
          st     <= S_WR_DAT;
          spi_tx <= blk_i.buf_rdata;
          spi_go <= 1'b1;
          crc16  <= crc16_byte(crc16, blk_i.buf_rdata);
          sbuf_addr <= 9'd1;
        end

        // The token byte carried data byte zero out with it, so this issues
        // bytes one to five hundred and eleven: five hundred and eleven
        // visits, the last of them with bcnt at 510.
        S_WR_DAT: begin
          spi_tx     <= blk_i.buf_rdata;
          spi_go     <= 1'b1;
          crc16      <= crc16_byte(crc16, blk_i.buf_rdata);
          sbuf_addr <= sbuf_addr + 9'd1;
          bcnt       <= bcnt + 10'd1;
          if (bcnt == 10'd510) begin
            bcnt <= '0;
            st   <= S_WR_CRC;
          end
        end

        S_WR_CRC: begin
          bcnt <= bcnt + 10'd1;
          if (bcnt == 10'd0) begin
            spi_tx <= crc16[15:8];
          end else begin
            spi_tx <= crc16[7:0];
            st     <= S_WR_RESP;
            ms     <= '0;
          end
          spi_go <= 1'b1;
        end

        S_WR_RESP: begin
          // The data response token is xxx0sss1, and 010 in the middle means
          // the card took it.
          if ((spi_rx & 8'h11) == 8'h01) begin
            if ((spi_rx & 8'h0e) != 8'h04) blk_err <= 1'b1;
            ms <= '0;
            st <= S_WR_BUSY;
          end else if (ms >= 16'd200) begin
            blk_err <= 1'b1;
            st    <= S_FINISH;
          end
          spi_tx <= 8'hff;
          spi_go <= 1'b1;
        end

        S_WR_BUSY: begin
          // The card holds the line low while it programmes the block, which
          // is the one place a write is allowed to take a quarter of a second.
          if (spi_rx != 8'h00) begin
            st <= S_FINISH;
          end else if (ms >= 16'd500) begin
            blk_err <= 1'b1;
            st    <= S_FINISH;
          end else begin
            spi_tx <= 8'hff;
            spi_go <= 1'b1;
          end
        end

        S_FINISH: begin
          blk_done <= 1'b1;
          st     <= S_READY;
        end

        // No card, or one that will not talk.  Every request is answered with
        // an error rather than left hanging, so the target reports NOT READY
        // and the driver says so instead of waiting for ever.
        S_DEAD: begin
          card_ready <= 1'b0;
          if (blk_i.start) begin
            blk_err  <= 1'b1;
            blk_done <= 1'b1;
          end
        end

        // ---- the command helper ---------------------------------------------
        C_SEND: begin
          spi_tx <= cmd_byte;
          spi_go <= 1'b1;
          bcnt   <= bcnt + 10'd1;
          if (bcnt == 10'd5) begin
            bcnt <= '0;
            st   <= C_R1;
          end
        end

        C_R1: begin
          if (!spi_rx[7]) begin
            r1   <= spi_rx;
            bcnt <= '0;
            if (cmd_extra) begin
              st     <= C_EXTRA;
              spi_tx <= 8'hff;
              spi_go <= 1'b1;
            end else begin
              st <= ret;
            end
          end else if (bcnt >= 10'd63) begin
            r1 <= 8'hff;        // the card never answered
            st <= ret;
          end else begin
            bcnt   <= bcnt + 10'd1;
            spi_tx <= 8'hff;
            spi_go <= 1'b1;
          end
        end

        C_EXTRA: begin
          r_extra <= {r_extra[23:0], spi_rx};
          bcnt    <= bcnt + 10'd1;
          if (bcnt == 10'd3) begin
            st <= ret;
          end else begin
            spi_tx <= 8'hff;
            spi_go <= 1'b1;
          end
        end

        default: st <= S_DEAD;
      endcase
    end
  end

endmodule

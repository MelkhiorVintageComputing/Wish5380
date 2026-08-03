// SPDX-License-Identifier: MIT
//
// One byte in and one byte out of an SD card in SPI mode.
//
// SPI mode 0: the clock idles low, the card samples MOSI on the rising edge
// and this samples MISO on the same edge, and both sides change their line on
// the falling edge.  Most significant bit first.
//
// The half period is an input rather than a parameter because it has to
// change while the design is running.  A card must be clocked at no more than
// 400 kHz until it has finished initialising and will accept 25 MHz
// afterwards, which is a factor of sixty and far too much to give away on
// every block.

module sd_spi (
  input  logic clk_i,
  input  logic rst_i,

  // System clocks per SPI half period.  One gives a clock of half the system
  // clock, which is the fastest this can go.
  input  logic [15:0] div_i,

  input  logic       cs_i,    // 1 selects the card, which drives /CS low
  input  logic       go_i,    // one cycle: move a byte
  input  logic [7:0] tx_i,
  output logic [7:0] rx_o,
  output logic       done_o,  // one cycle, as the byte completes
  output logic       busy_o,

  // ---- the card ----------------------------------------------------------
  output logic sclk_o,
  output logic mosi_o,
  output logic cs_n_o,
  input  logic miso_i
);

  logic [15:0] cnt;
  logic [2:0]  bit_cnt;
  logic [7:0]  sh_tx, sh_rx;
  logic        run, sclk_q;

  assign cs_n_o = ~cs_i;
  assign sclk_o = sclk_q;
  assign busy_o = run;
  assign rx_o   = sh_rx;

  // MOSI idles high.  A card that is not selected still sees the line, and
  // the seventy-four idle clocks it needs at power-up are specified with the
  // host driving ones.
  assign mosi_o = run ? sh_tx[7] : 1'b1;

  logic tick;
  assign tick = (cnt >= div_i);

  always_ff @(posedge clk_i) begin
    done_o <= 1'b0;

    if (rst_i) begin
      run     <= 1'b0;
      sclk_q  <= 1'b0;
      cnt     <= '0;
      bit_cnt <= '0;
      sh_tx   <= 8'hff;
      sh_rx   <= '0;
    end else if (!run) begin
      sclk_q <= 1'b0;
      cnt    <= '0;
      if (go_i) begin
        sh_tx   <= tx_i;
        bit_cnt <= '0;
        run     <= 1'b1;
      end
    end else if (!tick) begin
      cnt <= cnt + 16'd1;
    end else begin
      cnt <= '0;
      if (!sclk_q) begin
        // Rising edge: both sides sample.
        sclk_q <= 1'b1;
        sh_rx  <= {sh_rx[6:0], miso_i};
      end else begin
        // Falling edge: both sides move on.
        sclk_q <= 1'b0;
        sh_tx  <= {sh_tx[6:0], 1'b1};
        if (bit_cnt == 3'd7) begin
          run    <= 1'b0;
          done_o <= 1'b1;
        end else begin
          bit_cnt <= bit_cnt + 3'd1;
        end
      end
    end
  end

endmodule

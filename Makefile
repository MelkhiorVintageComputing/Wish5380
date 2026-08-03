# SPDX-License-Identifier: MIT
#
# Wish5380 build and regression.
#
#   make              build the testbench
#   make test         build and run the whole regression
#   make test T=reg   run the tests whose name contains "reg"
#   make wave T=...   run those tests with waveform tracing (build/waves/*.vcd)
#   make lint         Verilator lint of the RTL
#   make lint-icarus  second opinion from Icarus Verilog
#   make synth        quick Yosys elaboration check
#   make clean

VERILATOR ?= verilator
IVERILOG  ?= iverilog
YOSYS     ?= yosys

TOP       := tb_top
BUILD     := build

# The system clock the core is built for, in picoseconds.  The NCR 5380 is a
# clockless part (p. 18): its bus free filter, bus settle delay and bus clear
# delay come out of gate propagation in the silicon.  A clocked replica has to
# count them out instead, so every such delay below is derived from this
# number rather than written down.  Override it to build for a slower machine
# - a Mac Plus running the core in its 7.8336 MHz bus domain is
# SYS_PERIOD_PS=127654 - and the regression checks that clock.
SYS_PERIOD_PS ?= 20000

# The delays the datasheet guarantees, in clocks of the above.  Derived rather
# than written down, because a delay that quietly scales with the clock is the
# kind of thing that is only noticed on hardware.
BUS_FREE_TICKS   := $(shell expr 400000 / $(SYS_PERIOD_PS) + 1)
BUS_SETTLE_TICKS := $(shell expr 400000 / $(SYS_PERIOD_PS) + 1)
BUS_CLEAR_TICKS  := $(shell expr 800000 / $(SYS_PERIOD_PS) + 1)

OBJDIR    := $(BUILD)/obj_dir
BIN       := $(OBJDIR)/wish5380_tb

RTL_DIR   := src
TB_SV_DIR := tb/sv
TB_CPP    := tb/cpp

RTL       := $(RTL_DIR)/wish5380_pkg.sv \
             $(RTL_DIR)/sci_regs.sv \
             $(RTL_DIR)/sci_bus.sv \
             $(RTL_DIR)/scsi_fabric.sv \
             $(RTL_DIR)/scsi_targ.sv \
             $(RTL_DIR)/wish5380.sv \
             $(RTL_DIR)/wb_5380.sv \
             $(RTL_DIR)/wish5380_wb.sv \
             $(RTL_DIR)/sd_spi.sv \
             $(RTL_DIR)/blk_sd.sv \
             $(RTL_DIR)/wish5380_sd.sv
TB_SV     := $(TB_SV_DIR)/tb_top.sv
CPP_SRCS  := $(wildcard $(TB_CPP)/*.cpp) $(wildcard $(TB_CPP)/tests/*.cpp)
CPP_HDRS  := $(wildcard $(TB_CPP)/*.h)

# Modules Yosys elaborates one at a time, as a third opinion after Verilator
# and Icarus.  Kept explicit so a module that stops elaborating is noticed.
SYNTH_TOPS := sci_regs sci_bus scsi_fabric scsi_targ wish5380 \
              wb_5380 wish5380_wb sd_spi blk_sd wish5380_sd

# Tests to run, empty means all.  Pass extra runner flags in FLAGS.
T     ?=
FLAGS ?=

VFLAGS := --cc --exe --build --trace -Wall \
          --top-module $(TOP) \
          -GCLK_PERIOD_PS=$(SYS_PERIOD_PS) \
          -Mdir $(OBJDIR) \
          -o wish5380_tb \
          -CFLAGS "-I$(CURDIR)/$(TB_CPP) -DSYS_PERIOD_PS=$(SYS_PERIOD_PS) -O2 -Wall -Wno-unused-parameter"

.PHONY: all test wave lint lint-icarus synth list clean

all: $(BIN)

$(BIN): $(RTL) $(TB_SV) $(CPP_SRCS) $(CPP_HDRS) Makefile
	@mkdir -p $(BUILD)
	$(VERILATOR) $(VFLAGS) $(RTL) $(TB_SV) $(addprefix $(CURDIR)/,$(CPP_SRCS))

test: $(BIN)
	$(BIN) $(FLAGS) $(T)

wave: $(BIN)
	$(BIN) --trace $(FLAGS) $(T)
	@echo "waveforms in $(BUILD)/waves/"

list: $(BIN)
	@$(BIN) --list

lint:
	$(VERILATOR) --lint-only -Wall -GCLK_PERIOD_PS=$(SYS_PERIOD_PS) \
	  --top-module wish5380_sd $(RTL)
	$(VERILATOR) --lint-only -Wall -GCLK_PERIOD_PS=$(SYS_PERIOD_PS) \
	  --top-module $(TOP) $(RTL) $(TB_SV)

lint-icarus:
	$(IVERILOG) -g2012 -t null -o /dev/null $(RTL) $(TB_SV)

synth:
	@for top in $(SYNTH_TOPS); do \
	  printf '%-12s ' "$$top"; \
	  $(YOSYS) -q -p "read_verilog -sv $(RTL); hierarchy -check -top $$top; proc; opt" \
	    && echo "elaborates" || exit 1; \
	done

clean:
	rm -rf $(BUILD)

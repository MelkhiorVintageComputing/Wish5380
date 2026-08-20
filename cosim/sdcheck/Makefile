# SPDX-License-Identifier: MIT
#
# Drive the SD back end against a card model that is not ours.
#
#   make          build work/bin/sdcheck
#   make check    build and run both modes
#   make clean
#
# Deliberately not reachable from the top level Makefile, for two reasons.
# The first is the one cosim/rtl/Makefile gives: nothing in src/ or tb/ may
# grow a dependency on the co-simulation.  The second is licensing - this
# links ZipCPU's SDSPISIM, which is GPLv3-or-later, and `make test` must stay
# a thing that builds from MIT sources alone.
#
# The clone is not here and is never committed:
#   cosim/scripts/build-sdspi.sh

VERILATOR ?= verilator

HERE      := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
ROOT      := $(abspath $(HERE)../..)
WORK      ?= $(ROOT)/work
OBJDIR    := $(WORK)/sdcheck-build
BINDIR    := $(WORK)/bin
IMGDIR    := $(WORK)/images

BIN       := $(BINDIR)/sdcheck
SDSPI     := $(WORK)/sdspi-src
ZIPSIM    := $(SDSPI)/bench/cpp/sdspisim.cpp

RTL_DIR   := $(ROOT)/src
TB_CPP    := $(ROOT)/tb/cpp

# The package first, for the file-scope struct typedefs at the bottom of it.
RTL       := $(RTL_DIR)/wish5380_pkg.sv \
             $(RTL_DIR)/sd_spi.sv \
             $(RTL_DIR)/blk_sd.sv
TOP_SV    := $(HERE)sdcheck_top.sv

# Only the parts of the testbench that are not about testing, plus our own
# card model - which is one of the two things being compared.
TB_SRCS   := $(TB_CPP)/sim.cpp $(TB_CPP)/util.cpp $(TB_CPP)/sd_card.cpp

CXXFLAGS  := -std=c++14 -O2 -Wall -Wno-unused-parameter -faligned-new \
             -I$(TB_CPP) -I$(HERE) -I$(OBJDIR) -I$(SDSPI)/bench/cpp

# tb/cpp/sim.cpp refers to the VCD writer whether or not anything traces, so
# the implementation has to be linked in.  Adding the source rather than
# passing --trace keeps the model uninstrumented, which is what cosim/rtl does.
VINC      := $(shell $(VERILATOR) --getenv VERILATOR_ROOT)/include

.PHONY: all check clean

all: $(BIN)

$(ZIPSIM):
	@echo "error: $(SDSPI) is missing." >&2
	@echo "Run cosim/scripts/build-sdspi.sh first - it clones ZipCPU's" >&2
	@echo "sdspi into work/, which is not kept in this repository." >&2
	@false

$(BIN): $(RTL) $(TOP_SV) $(HERE)sdcheck.cpp $(TB_SRCS) $(ZIPSIM)
	@mkdir -p $(OBJDIR) $(BINDIR) $(IMGDIR)
	$(VERILATOR) --cc --exe --build -j 0 \
	  -CFLAGS "$(CXXFLAGS)" \
	  -Mdir $(OBJDIR) \
	  --top-module sdcheck_top \
	  -o $(BIN) \
	  $(RTL) $(TOP_SV) \
	  $(HERE)sdcheck.cpp $(TB_SRCS) $(ZIPSIM) \
	  $(VINC)/verilated_vcd_c.cpp

IMG       := $(IMGDIR)/sdcheck.img

check: $(BIN)
	@mkdir -p $(IMGDIR)
	@$(BIN) --mode swap --card ours --image $(IMG)
	@$(BIN) --mode swap --card zip  --image $(IMG)
	@$(BIN) --mode diff             --image $(IMG)

clean:
	rm -rf $(OBJDIR) $(BIN)

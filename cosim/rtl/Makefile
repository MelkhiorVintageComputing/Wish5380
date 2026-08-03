# SPDX-License-Identifier: MIT
#
# Build the Verilated wish5380 into a shared library an emulated ISA card can
# dlopen, and a self test that drives it with no emulator in the loop.
#
#   make          build work/lib/libwish5380rtl.so
#   make check    build and run the self test - do this first when the guest
#                 misbehaves, because it has one unknown instead of three
#   make lint
#   make clean
#
# Deliberately not reachable from the top level Makefile: nothing in src/ or
# tb/ may grow a dependency on the co-simulation.

VERILATOR ?= verilator

HERE      := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
ROOT      := $(abspath $(HERE)../..)
WORK      ?= $(ROOT)/work
OBJDIR    := $(WORK)/rtl-build
LIBDIR    := $(WORK)/lib
BINDIR    := $(WORK)/bin

LIB       := $(LIBDIR)/libwish5380rtl.so
SELFTEST  := $(BINDIR)/rtl-selftest

RTL_DIR   := $(ROOT)/src
TB_CPP    := $(ROOT)/tb/cpp

# The same order the top level Makefile uses: the package first, because the
# file-scope scsi_t typedef lives at the bottom of it.
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
TOP_SV    := $(HERE)rtl_top.sv

# Only the parts of the testbench that are not about testing.
TB_SRCS   := $(TB_CPP)/sim.cpp $(TB_CPP)/util.cpp $(TB_CPP)/wb.cpp \
             $(TB_CPP)/sd_card.cpp

CXXFLAGS  := -std=c++14 -O2 -Wall -Wno-unused-parameter -faligned-new -fPIC \
             -I$(TB_CPP) -I$(HERE) -I$(OBJDIR)

VFLAGS    := --cc -Wall --top-module rtl_top \
             -Mdir $(OBJDIR) \
             -CFLAGS "$(CXXFLAGS)"

VINC      := $(shell $(VERILATOR) --getenv VERILATOR_ROOT)/include

.PHONY: all check lint clean

all: $(LIB)

$(OBJDIR)/Vrtl_top__ALL.a: $(RTL) $(TOP_SV) Makefile
	@mkdir -p $(OBJDIR)
	$(VERILATOR) $(VFLAGS) $(RTL) $(TOP_SV)
	$(MAKE) -C $(OBJDIR) -f Vrtl_top.mk

$(LIB): $(OBJDIR)/Vrtl_top__ALL.a $(HERE)wish_rtl.cpp $(HERE)wish_rtl.h $(TB_SRCS) $(HERE)Makefile
	@mkdir -p $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ \
	    $(HERE)wish_rtl.cpp $(TB_SRCS) \
	    $(VINC)/verilated.cpp $(VINC)/verilated_threads.cpp \
	    $(VINC)/verilated_vcd_c.cpp \
	    -I$(VINC) -I$(VINC)/vltstd \
	    $(OBJDIR)/Vrtl_top__ALL.a -pthread -latomic

$(SELFTEST): $(LIB) $(HERE)selftest.cpp $(HERE)wish_rtl.h $(HERE)Makefile
	@mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $(HERE)selftest.cpp -ldl

check: $(SELFTEST)
	$(SELFTEST) $(LIB)

lint:
	$(VERILATOR) --lint-only -Wall --top-module rtl_top $(RTL) $(TOP_SV)

clean:
	rm -rf $(OBJDIR) $(LIB) $(SELFTEST)

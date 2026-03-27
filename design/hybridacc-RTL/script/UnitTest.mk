# UnitTest.mk - Simple VCS-based unit test runner for RTL testbenches
############################################################
# UnitTest.mk - VCS-based unit test runner for RTL testbenches
# Usage:
#   make -f UnitTest.mk PE/tb_datamemory.sv
#   make -f UnitTest.mk PE/tb_foo.sv PE/tb_bar.sv
#   make -f UnitTest.mk batch
#
# "batch" 目標會自動搜尋 tb/ 目錄下所有 tb_*.sv 並執行
############################################################

# VCS and simulation settings
VCS      = vcs
VCS_FLAGS= -full64 -sverilog -debug_access+all -timescale=1ns/1ps
SIMV     = simv

# Output and log directories
OUTDIR   = output/unit_test
LOGDIR   = $(OUTDIR)/log


# Default: do nothing, print usage
.PHONY: all
all:
	@echo "[UnitTest.mk] Usage: make -f UnitTest.mk <tb1.sv> [<tb2.sv> ...]"
	@echo "                or: make -f UnitTest.mk batch"

# Batch: run all tb_*.sv under tb/ recursively
.PHONY: batch
BATCH_TB := $(shell find ../tb -type f -name 'tb_*.sv')
batch: $(BATCH_TB)

# Pattern rule: compile & run any .sv testbench
%.sv:
	@mkdir -p $(OUTDIR) $(LOGDIR)
	@echo "[UnitTest.mk] Compiling $< ..."
	$(VCS) $(VCS_FLAGS) $< -o $(OUTDIR)/$(basename $(notdir $<)).simv > $(LOGDIR)/$(basename $(notdir $<)).compile.log 2>&1
	@echo "[UnitTest.mk] Running simulation for $< ..."
	$(OUTDIR)/$(basename $(notdir $<)).simv > $(LOGDIR)/$(basename $(notdir $<)).run.log 2>&1
	@echo "[UnitTest.mk] Logs: $(LOGDIR)/$(basename $(notdir $<)).compile.log, $(LOGDIR)/$(basename $(notdir $<)).run.log"

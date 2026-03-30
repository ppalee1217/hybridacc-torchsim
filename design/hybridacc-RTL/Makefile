syn_dir := ./syn
bld_dir := ./build
report_dir := ./report
sim_dir := ./sim
sim_bld_dir := $(sim_dir)/build
sim_log_dir := $(sim_dir)/log

VCS ?= vcs
DW_SIM_VER := /usr/cad/synopsys/synthesis/2024.09-sp2/dw/sim_ver
# Behavioral SRAM model for pre-synthesis simulation (replaces TSMC hard macro)
SRAM_SIM_MODEL := src/PE/SRAM_SP_BWEB.sv
VCS_FLAGS ?= -full64 -sverilog -debug_access+all -timescale=1ns/1ps \
	-y $(DW_SIM_VER) +libext+.v
SIM_PLUSARGS := $(if $(TEST_DATA_DIR),+DATA_DIR=$(TEST_DATA_DIR),) \
	$(if $(VERIFY_TOL),+VERIFY_TOL=$(VERIFY_TOL),) \
	$(if $(CLOCK_PERIOD_NS),+CLOCK_PERIOD_NS=$(CLOCK_PERIOD_NS),)

TB_ALL := $(shell find tb -type f -name 'tb_*.sv')
TB_PE := $(shell find tb/PE -type f -name 'tb_*.sv' 2>/dev/null)
TB_NOC := $(shell find tb/NoC -type f -name 'tb_*.sv' 2>/dev/null)
TB_CLUSTER := $(shell find tb/Cluster -type f -name 'tb_*.sv' 2>/dev/null)
SYN_TCL := $(shell find script -maxdepth 1 -type f -name 'synthesis_*.tcl')

DC_SHELL ?= dc_shell
SNPSLMD_LICENSE_FILE ?= 26585@lstn
export SNPSLMD_LICENSE_FILE

PE_UNITS := DataMemory Decoder InstructionMemory LoopController PsumRegFile \
            TransformRegFile VADDU VMULU LDMA SDMA \
            IF_ID_Stage EXE_M_Stage EXE_A_Stage PErouter ProcessElement

help:
	@echo "Available targets:"
	@echo "  sim_<tb|module>     - Compile and run a testbench, e.g. sim_pe_sim or sim_tb_pe_sim"
	@echo "  sim_all             - Run all testbenches under tb/"
	@echo "  sim_pe              - Run all testbenches under tb/PE/"
	@echo "  sim_noc             - Run all testbenches under tb/NoC/"
	@echo "  sim_cluster         - Run all testbenches under tb/Cluster/"
	@echo "  synthesize_<name>   - Run script/synthesis_<name>.tcl"
	@echo "  synall              - Run all synthesis_*.tcl scripts under script/"
	@echo "  syn_pe_<module>     - Synthesize a PE unit, e.g. syn_pe_VADDU"
	@echo "  syn_pe_all          - Synthesize all PE units"
	@echo "  postsim_<tb|module> - Summarize the latest simulation log for a testbench"
	@echo "  postsim_all         - Summarize all run logs under sim/log/"
	@echo "  clean               - Remove simulation build artifacts"
	@echo "  clean_syn           - Remove synthesized files and reports"
	@echo "  syn_report           - Generate a synthesis report for all PE units in $(report_dir)/pe_synthesis_report.md"
	@echo "Variables: TEST_DATA_DIR=... VERIFY_TOL=... CLOCK_PERIOD_NS=..."

$(bld_dir):
	mkdir -p $(bld_dir)

$(syn_dir):
	mkdir -p $(syn_dir)

$(report_dir):
	mkdir -p $(report_dir)

$(sim_bld_dir):
	mkdir -p $(sim_bld_dir)

$(sim_log_dir):
	mkdir -p $(sim_log_dir)

define run_tb
	@tb_file=$$(find tb -type f \( -name 'tb_$*.sv' -o -name '$*.sv' \) | head -n 1); \
	if [ -z "$$tb_file" ]; then \
		echo "No testbench found for '$*'"; \
		exit 1; \
	fi; \
	tb_base=$$(basename $$tb_file .sv); \
	tb_dir=$$(dirname $$tb_file); \
	echo "[SIM] Compiling $$tb_file ..."; \
	$(VCS) $(VCS_FLAGS) +incdir+$$tb_dir +incdir+tb $(SRAM_SIM_MODEL) $$tb_file -o $(sim_bld_dir)/$$tb_base.simv > $(sim_log_dir)/$$tb_base.compile.log 2>&1; \
	echo "[SIM] Running $$tb_base ..."; \
	$(sim_bld_dir)/$$tb_base.simv $(SIM_PLUSARGS) > $(sim_log_dir)/$$tb_base.run.log 2>&1; \
	echo "[SIM] Logs: $(sim_log_dir)/$$tb_base.compile.log $(sim_log_dir)/$$tb_base.run.log"
endef

define summarize_tb
	@tb_file=$$(find tb -type f \( -name 'tb_$*.sv' -o -name '$*.sv' \) | head -n 1); \
	if [ -z "$$tb_file" ]; then \
		echo "No testbench found for '$*'"; \
		exit 1; \
	fi; \
	tb_base=$$(basename $$tb_file .sv); \
	log_file=$(sim_log_dir)/$$tb_base.run.log; \
	if [ ! -f $$log_file ]; then \
		echo "No run log found for $$tb_base"; \
		exit 1; \
	fi; \
	echo "[POSTSIM] $$log_file"; \
	grep -E 'PASS|FAIL|Verification Results|Outputs collected|Output timeout|tb_.* PASS' $$log_file || true
endef

sim_%: | $(sim_bld_dir) $(sim_log_dir)
	$(run_tb)

sim_all: | $(sim_bld_dir) $(sim_log_dir)
	@set -e; \
	for tb_file in $(TB_ALL); do \
		tb_base=$$(basename $$tb_file .sv); \
		$(MAKE) sim_$$tb_base TEST_DATA_DIR="$(TEST_DATA_DIR)" VERIFY_TOL="$(VERIFY_TOL)" CLOCK_PERIOD_NS="$(CLOCK_PERIOD_NS)"; \
	done

sim_pe: | $(sim_bld_dir) $(sim_log_dir)
	@set -e; \
	for tb_file in $(TB_PE); do \
		tb_base=$$(basename $$tb_file .sv); \
		$(MAKE) sim_$$tb_base TEST_DATA_DIR="$(TEST_DATA_DIR)" VERIFY_TOL="$(VERIFY_TOL)" CLOCK_PERIOD_NS="$(CLOCK_PERIOD_NS)"; \
	done

sim_noc: | $(sim_bld_dir) $(sim_log_dir)
	@set -e; \
	for tb_file in $(TB_NOC); do \
		tb_base=$$(basename $$tb_file .sv); \
		$(MAKE) sim_$$tb_base TEST_DATA_DIR="$(TEST_DATA_DIR)" VERIFY_TOL="$(VERIFY_TOL)" CLOCK_PERIOD_NS="$(CLOCK_PERIOD_NS)"; \
	done

sim_cluster: | $(sim_bld_dir) $(sim_log_dir)
	@set -e; \
	for tb_file in $(TB_CLUSTER); do \
		tb_base=$$(basename $$tb_file .sv); \
		$(MAKE) sim_$$tb_base TEST_DATA_DIR="$(TEST_DATA_DIR)" VERIFY_TOL="$(VERIFY_TOL)" CLOCK_PERIOD_NS="$(CLOCK_PERIOD_NS)"; \
	done

synthesize_%: | $(bld_dir) $(syn_dir) $(report_dir)
	@echo "Synthesizing $*..."
	@test -f script/synthesis_$*.tcl || (echo "Missing script/synthesis_$*.tcl" && exit 1)
	mkdir -p $(syn_dir)/$*
	mkdir -p $(report_dir)/$*
	cp script/synopsys_dc.setup $(bld_dir)/.synopsys_dc.setup
	cd $(bld_dir); \
	dc_shell -no_home_init -f ../script/synthesis_$*.tcl | tee syn_compile.log

synall:
	@set -e; \
	for script_file in $(SYN_TCL); do \
		basefile=$$(basename $$script_file .tcl | sed 's/^synthesis_//'); \
		$(MAKE) synthesize_$$basefile; \
	done

# --- PE unit synthesis (driven by synthesis_pe_units.tcl) ---
syn_pe_%: | $(bld_dir) $(syn_dir) $(report_dir)
	@echo "[SYN] Synthesizing PE unit: $* ..."
	mkdir -p $(syn_dir)/$*
	mkdir -p $(report_dir)/$*
	cp script/synopsys_dc.setup $(bld_dir)/.synopsys_dc.setup
	cd $(bld_dir); \
	$(DC_SHELL) -no_home_init -x "set MOD_NAME $*" -f ../script/synthesis_pe_units.tcl \
		| tee ../$(report_dir)/$*/syn_compile_$*.log
	@echo "[SYN] Done: $*  Reports: $(report_dir)/$*/"

syn_pe_all: | $(bld_dir) $(syn_dir) $(report_dir)
	@set -e; \
	for mod in $(PE_UNITS); do \
		$(MAKE) syn_pe_$$mod; \
	done

syn_report:
	@uv run python script/syn_report.py --report-dir $(report_dir) --output $(report_dir)/pe_synthesis_report.md

postsim_%:
	$(summarize_tb)

postsim_all:
	@for log_file in $(sim_log_dir)/*.run.log; do \
		[ -f $$log_file ] || continue; \
		echo "[POSTSIM] $$log_file"; \
		grep -E 'PASS|FAIL|Verification Results|Outputs collected|Output timeout|tb_.* PASS' $$log_file || true; \
	done

# Clean build artifacts
clean:
	rm -rf csrc simv simv.daidir ucli.key DVEfiles *.vpd nWaveLog/ *.fsdb novas* *.lib++ sdfAnnotateInfo $(bld_dir) $(sim_dir)

clean_syn:
	rm -rf $(syn_dir) $(report_dir)

.PHONY: help sim_all sim_pe sim_noc sim_cluster synall syn_pe_all syn_report postsim_all clean clean_syn
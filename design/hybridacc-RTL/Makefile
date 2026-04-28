syn_dir := ./syn
bld_dir := ./build
report_dir := ./report
sim_dir := ./sim
sim_bld_dir := $(sim_dir)/build
sim_log_dir := $(sim_dir)/log
gate_sim_bld_dir := $(sim_dir)/gate_build
gate_sim_log_dir := $(sim_dir)/gate_log

VCS ?= vcs
DW_SIM_VER := /usr/cad/synopsys/synthesis/2024.09-sp2/dw/sim_ver
# Behavioral SRAM model for pre-synthesis simulation (replaces TSMC hard macro)
SRAM_SIM_MODEL := src/PE/SRAM_SP_BWEB.sv
# Standard cell and SRAM Verilog simulation models for gate-level simulation
STDCELL_V := /cad/process/ADFP/Executable_Package/Collaterals/IP/stdcell/N16ADFP_StdCell/VERILOG/N16ADFP_StdCell.v
SRAM_V    := /cad/process/ADFP/Executable_Package/Collaterals/IP/sram/N16ADFP_SRAM/VERILOG/N16ADFP_SRAM_100a.v

VCS_FLAGS ?= -full64 -sverilog -debug_access+all -timescale=1ns/1ps \
	-y $(DW_SIM_VER) +libext+.v
GATE_VCS_FLAGS := -full64 -sverilog -debug_access+all -timescale=1ns/1ps \
	+define+GATE_SIM +neg_tchk +sdfverbose +no_notifier +notimingcheck \
	-v $(STDCELL_V) -v $(SRAM_V) \
	-y $(DW_SIM_VER) +libext+.v
RTL_INCDIRS := +incdir+tb +incdir+src +incdir+src/Cluster +incdir+src/Core +incdir+src/PE +incdir+src/NoC
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

NOC_UNITS := MBUS NoCRouter NetworkOnChip

CLUSTER_UNITS := SRAM AddressGenerateUnit ScratchpadMemory HybridDataDeliverUnit ComputeCluster

# Mapping: PE module name -> gate-level netlist path (relative to project root)
GATE_NETLIST_DIR := $(syn_dir)

help:
	@echo "Available targets:"
	@echo "  sim_<tb|module>      - Pre-sim: compile and run a testbench (RTL)"
	@echo "  sim_all              - Pre-sim: run all testbenches under tb/"
	@echo "  sim_pe               - Pre-sim: run all testbenches under tb/PE/"
	@echo "  sim_noc              - Pre-sim: run all testbenches under tb/NoC/"
	@echo "  sim_noc_sim          - Pre-sim: full NoC system sim with ESL test data"
	@echo "  sim_noc_sim_all      - Pre-sim: run all NoC test cases under output/noc-sim/"
	@echo "  sim_cluster          - Pre-sim: run all testbenches under tb/Cluster/"
	@echo "  gate_sim_<tb|module> - Post-sim: compile and run a testbench (gate-level + SDF)"
	@echo "  gate_sim_pe          - Post-sim: run all PE testbenches (gate-level + SDF)"
	@echo "  gate_sim_noc         - Post-sim: run all NoC testbenches (gate-level + SDF)"
	@echo "  synthesize_<name>    - Run script/synthesis_<name>.tcl"
	@echo "  synall               - Run all synthesis_*.tcl scripts under script/"
	@echo "  syn_pe_<module>      - Synthesize a PE unit, e.g. syn_pe_VADDU"
	@echo "  syn_pe_all           - Synthesize all PE units"
	@echo "  syn_noc_<module>     - Synthesize a NoC unit, e.g. syn_noc_MBUS"
	@echo "  syn_noc_all          - Synthesize all NoC units"
	@echo "  syn_cluster_<module> - Synthesize a Cluster unit"
	@echo "  syn_cluster_all      - Synthesize all Cluster units"
	@echo "  syn_report           - Generate a synthesis report"
	@echo "  sim_report           - Generate a pre-sim report from sim/log/"
	@echo "  gate_sim_report      - Generate a post-sim report from sim/gate_log/"
	@echo "  clean                - Remove simulation build artifacts"
	@echo "  clean_syn            - Remove synthesized files and reports"
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

$(gate_sim_bld_dir):
	mkdir -p $(gate_sim_bld_dir)

$(gate_sim_log_dir):
	mkdir -p $(gate_sim_log_dir)

define run_tb
	@tb_file=$$(find tb -type f \( -name 'tb_$*.sv' -o -name '$*.sv' \) | head -n 1); \
	if [ -z "$$tb_file" ]; then \
		echo "No testbench found for '$*'"; \
		exit 1; \
	fi; \
	tb_base=$$(basename $$tb_file .sv); \
	tb_dir=$$(dirname $$tb_file); \
	echo "[SIM] Compiling $$tb_file ..."; \
	$(VCS) $(VCS_FLAGS) +incdir+$$tb_dir $(RTL_INCDIRS) $(SRAM_SIM_MODEL) $$tb_file -o $(sim_bld_dir)/$$tb_base.simv > $(sim_log_dir)/$$tb_base.compile.log 2>&1 || \
		{ echo "[SIM] ERROR: VCS compile failed. See $(sim_log_dir)/$$tb_base.compile.log"; exit 1; }; \
	if [ ! -x $(sim_bld_dir)/$$tb_base.simv ]; then \
		echo "[SIM] ERROR: simv was not generated. See $(sim_log_dir)/$$tb_base.compile.log"; \
		exit 1; \
	fi; \
	echo "[SIM] Running $$tb_base ..."; \
	$(sim_bld_dir)/$$tb_base.simv $(SIM_PLUSARGS) > $(sim_log_dir)/$$tb_base.run.log 2>&1 || \
		{ echo "[SIM] ERROR: Simulation failed. See $(sim_log_dir)/$$tb_base.run.log"; exit 1; }; \
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

# --- Gate-level (post-synthesis) simulation ---
# MOD_NAME must be set to resolve the gate netlist path, e.g.:
#   make gate_sim_tb_vaddu MOD_NAME=VADDU
# Or use the convenience PE mapping targets below.

# PE unit TB -> module name mapping for gate_sim
GATE_PE_MAP_tb_datamemory       := DataMemory
GATE_PE_MAP_tb_decoder          := Decoder
GATE_PE_MAP_tb_instructionmemory:= InstructionMemory
GATE_PE_MAP_tb_loopcontroller   := LoopController
GATE_PE_MAP_tb_psumregfile      := PsumRegFile
GATE_PE_MAP_tb_transformregfile := TransformRegFile
GATE_PE_MAP_tb_vaddu            := VADDU
GATE_PE_MAP_tb_vmulu            := VMULU
GATE_PE_MAP_tb_ldma             := LDMA
GATE_PE_MAP_tb_sdma             := SDMA
GATE_PE_MAP_tb_if_id_stage      := IF_ID_Stage
GATE_PE_MAP_tb_exe_m_stage      := EXE_M_Stage
GATE_PE_MAP_tb_exe_a_stage      := EXE_A_Stage
GATE_PE_MAP_tb_perouter         := PErouter
GATE_PE_MAP_tb_processelement   := ProcessElement
GATE_PE_MAP_tb_pe_sim           := ProcessElement

# All PE testbenches that have gate-level counterparts
GATE_PE_TBS := tb_datamemory tb_decoder tb_instructionmemory tb_loopcontroller \
               tb_psumregfile tb_transformregfile tb_vaddu tb_vmulu \
               tb_ldma tb_sdma tb_if_id_stage tb_exe_m_stage tb_exe_a_stage \
               tb_perouter tb_processelement tb_pe_sim

# NoC unit TB -> module name mapping for gate_sim
GATE_NOC_MAP_tb_mbus            := MBUS
GATE_NOC_MAP_tb_nocrouter       := NoCRouter
GATE_NOC_MAP_tb_noc_unit        := NetworkOnChip
GATE_NOC_MAP_tb_noc_sim         := NetworkOnChip

# All NoC testbenches that have gate-level counterparts
GATE_NOC_TBS := tb_mbus tb_nocrouter tb_noc_unit tb_noc_sim

define run_gate_tb
	@tb_file=$$(find tb -type f \( -name 'tb_$*.sv' -o -name '$*.sv' \) | head -n 1); \
	if [ -z "$$tb_file" ]; then \
		echo "No testbench found for '$*'"; \
		exit 1; \
	fi; \
	tb_base=$$(basename $$tb_file .sv); \
	tb_dir=$$(dirname $$tb_file); \
	mod_name=$${MOD_NAME:-$$(echo $(GATE_PE_MAP_$$(basename $$tb_file .sv))$(GATE_NOC_MAP_$$(basename $$tb_file .sv)))}; \
	if [ -z "$$mod_name" ]; then \
		echo "[GATE_SIM] ERROR: MOD_NAME not set and no mapping for $$tb_base"; \
		exit 1; \
	fi; \
	netlist=$(GATE_NETLIST_DIR)/$$mod_name/$${mod_name}_syn.v; \
	if [ ! -f "$$netlist" ]; then \
		echo "[GATE_SIM] ERROR: Gate netlist not found: $$netlist"; \
		echo "[GATE_SIM] Run 'make syn_pe_$$mod_name' or 'make syn_noc_$$mod_name' first."; \
		exit 1; \
	fi; \
	echo "[GATE_SIM] Compiling $$tb_file (gate: $$mod_name) ..."; \
	$(VCS) $(GATE_VCS_FLAGS) +incdir+$$tb_dir $(RTL_INCDIRS) \
		$(SRAM_SIM_MODEL) $$netlist $$tb_file \
		-o $(gate_sim_bld_dir)/$$tb_base.gate.simv \
		> $(gate_sim_log_dir)/$$tb_base.compile.log 2>&1 || \
		{ echo "[GATE_SIM] ERROR: VCS compile failed. See $(gate_sim_log_dir)/$$tb_base.compile.log"; exit 1; }; \
	if [ ! -x $(gate_sim_bld_dir)/$$tb_base.gate.simv ]; then \
		echo "[GATE_SIM] ERROR: gate simv was not generated. See $(gate_sim_log_dir)/$$tb_base.compile.log"; \
		exit 1; \
	fi; \
	echo "[GATE_SIM] Running $$tb_base ..."; \
	$(gate_sim_bld_dir)/$$tb_base.gate.simv $(SIM_PLUSARGS) \
		> $(gate_sim_log_dir)/$$tb_base.run.log 2>&1 || \
		{ echo "[GATE_SIM] ERROR: Simulation failed. See $(gate_sim_log_dir)/$$tb_base.run.log"; exit 1; }; \
	echo "[GATE_SIM] Logs: $(gate_sim_log_dir)/$$tb_base.compile.log $(gate_sim_log_dir)/$$tb_base.run.log"
endef

gate_sim_%: | $(gate_sim_bld_dir) $(gate_sim_log_dir)
	$(run_gate_tb)

gate_sim_pe: | $(gate_sim_bld_dir) $(gate_sim_log_dir)
	@set -e; \
	for pair in \
		tb_datamemory:DataMemory \
		tb_decoder:Decoder \
		tb_instructionmemory:InstructionMemory \
		tb_loopcontroller:LoopController \
		tb_psumregfile:PsumRegFile \
		tb_transformregfile:TransformRegFile \
		tb_vaddu:VADDU \
		tb_vmulu:VMULU \
		tb_ldma:LDMA \
		tb_sdma:SDMA \
		tb_if_id_stage:IF_ID_Stage \
		tb_exe_m_stage:EXE_M_Stage \
		tb_exe_a_stage:EXE_A_Stage \
		tb_perouter:PErouter \
		tb_processelement:ProcessElement \
	; do \
		tb=$${pair%%:*}; mod=$${pair##*:}; \
		$(MAKE) gate_sim_$$tb MOD_NAME=$$mod; \
	done

gate_sim_noc: | $(gate_sim_bld_dir) $(gate_sim_log_dir)
	@set -e; \
	for pair in \
		tb_mbus:MBUS \
		tb_nocrouter:NoCRouter \
		tb_noc_unit:NetworkOnChip \
		tb_noc_sim:NetworkOnChip \
	; do \
		tb=$${pair%%:*}; mod=$${pair##*:}; \
		$(MAKE) gate_sim_$$tb MOD_NAME=$$mod; \
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

# --- NoC unit synthesis (driven by synthesis_noc_units.tcl) ---
syn_noc_%: | $(bld_dir) $(syn_dir) $(report_dir)
	@echo "[SYN] Synthesizing NoC unit: $* ..."
	mkdir -p $(syn_dir)/$*
	mkdir -p $(report_dir)/$*
	cp script/synopsys_dc.setup $(bld_dir)/.synopsys_dc.setup
	cd $(bld_dir); \
	$(DC_SHELL) -no_home_init -x "set MOD_NAME $*" -f ../script/synthesis_noc_units.tcl \
		| tee ../$(report_dir)/$*/syn_compile_$*.log
	@echo "[SYN] Done: $*  Reports: $(report_dir)/$*/"

syn_noc_all: | $(bld_dir) $(syn_dir) $(report_dir)
	@set -e; \
	for mod in $(NOC_UNITS); do \
		$(MAKE) syn_noc_$$mod; \
	done

# --- Cluster unit synthesis (driven by synthesis_cluster_units.tcl) ---
syn_cluster_%: | $(bld_dir) $(syn_dir) $(report_dir)
	@echo "[SYN] Synthesizing Cluster unit: $* ..."
	mkdir -p $(syn_dir)/$*
	mkdir -p $(report_dir)/$*
	cp script/synopsys_dc.setup $(bld_dir)/.synopsys_dc.setup
	cd $(bld_dir); \
	$(DC_SHELL) -no_home_init -x "set MOD_NAME $*" -f ../script/synthesis_cluster_units.tcl \
		| tee ../$(report_dir)/$*/syn_compile_$*.log
	@echo "[SYN] Done: $*  Reports: $(report_dir)/$*/"

syn_cluster_all: | $(bld_dir) $(syn_dir) $(report_dir)
	@set -e; \
	for mod in $(CLUSTER_UNITS); do \
		$(MAKE) syn_cluster_$$mod; \
	done

VENV_PYTHON := /home/easonyeh/hybridacc/.venv/bin/python

TIMESTAMP := $(shell date +%Y_%m_%d_%H_%M_%S)

syn_report:
	@$(VENV_PYTHON) script/syn_report.py --report-dir $(report_dir) --output $(report_dir)/pe_synthesis_report_$(TIMESTAMP).md

sim_report:
	@$(VENV_PYTHON) script/sim_report.py --log-dir $(sim_log_dir) --mode pre-sim --output $(report_dir)/pre_sim_report_$(TIMESTAMP).md

gate_sim_report:
	@$(VENV_PYTHON) script/sim_report.py --log-dir $(gate_sim_log_dir) --mode post-sim --output $(report_dir)/post_sim_report_$(TIMESTAMP).md

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

# ---------------------------------------------------------------------------
# sim_noc_sim — Full NoC system-level simulation using ESL test data
#   make sim_noc_sim TEST_DATA_DIR=output/noc-sim/conv_k3c4
#   make sim_noc_sim TEST_DATA_DIR=output/noc-sim/gemm VERIFY_TOL=0.05
#   make sim_noc_sim_all                      → run all cases under output/noc-sim/
# ---------------------------------------------------------------------------
sim_noc_sim: | $(sim_bld_dir) $(sim_log_dir)
	@echo "[NOC_SIM] Compiling tb_noc_sim.sv ..."; \
	$(VCS) $(VCS_FLAGS) +incdir+tb/NoC +incdir+tb \
		$(SRAM_SIM_MODEL) tb/NoC/tb_noc_sim.sv \
		-o $(sim_bld_dir)/tb_noc_sim.simv \
		> $(sim_log_dir)/tb_noc_sim.compile.log 2>&1; \
	if [ $$? -ne 0 ]; then \
		echo "[NOC_SIM] Compilation FAILED — see $(sim_log_dir)/tb_noc_sim.compile.log"; \
		tail -40 $(sim_log_dir)/tb_noc_sim.compile.log; \
		exit 1; \
	fi; \
	echo "[NOC_SIM] Running tb_noc_sim (data=$(TEST_DATA_DIR)) ..."; \
	$(sim_bld_dir)/tb_noc_sim.simv $(SIM_PLUSARGS) \
		> $(sim_log_dir)/tb_noc_sim.run.log 2>&1; \
	echo "[NOC_SIM] Logs: $(sim_log_dir)/tb_noc_sim.compile.log $(sim_log_dir)/tb_noc_sim.run.log"; \
	tail -30 $(sim_log_dir)/tb_noc_sim.run.log

sim_noc_sim_all: | $(sim_bld_dir) $(sim_log_dir)
	@echo "[NOC_SIM] Compiling tb_noc_sim.sv ..."; \
	$(VCS) $(VCS_FLAGS) +incdir+tb/NoC +incdir+tb \
		$(SRAM_SIM_MODEL) tb/NoC/tb_noc_sim.sv \
		-o $(sim_bld_dir)/tb_noc_sim.simv \
		> $(sim_log_dir)/tb_noc_sim.compile.log 2>&1; \
	if [ $$? -ne 0 ]; then \
		echo "[NOC_SIM] Compilation FAILED"; \
		tail -40 $(sim_log_dir)/tb_noc_sim.compile.log; \
		exit 1; \
	fi; \
	pass=0; fail=0; \
	for case_dir in output/noc-sim/*/; do \
		case_name=$$(basename $$case_dir); \
		echo "[NOC_SIM] Running $$case_name ..."; \
		$(sim_bld_dir)/tb_noc_sim.simv +DATA_DIR=$$case_dir \
			$(if $(VERIFY_TOL),+VERIFY_TOL=$(VERIFY_TOL),) \
			$(if $(CLOCK_PERIOD_NS),+CLOCK_PERIOD_NS=$(CLOCK_PERIOD_NS),) \
			> $(sim_log_dir)/tb_noc_sim_$$case_name.run.log 2>&1; \
		if grep -q 'Test PASSED' $(sim_log_dir)/tb_noc_sim_$$case_name.run.log; then \
			echo "[NOC_SIM] $$case_name: PASSED"; pass=$$((pass+1)); \
		else \
			echo "[NOC_SIM] $$case_name: FAILED"; fail=$$((fail+1)); \
		fi; \
	done; \
	echo "[NOC_SIM] Summary: $$pass PASSED, $$fail FAILED"

.PHONY: help sim_all sim_pe sim_noc sim_cluster sim_noc_fast sim_noc_sim sim_noc_sim_all synall syn_pe_all syn_noc_all syn_cluster_all syn_report sim_report gate_sim_report gate_sim_pe gate_sim_noc postsim_all clean clean_syn
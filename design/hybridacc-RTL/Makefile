syn_dir := ./syn
bld_dir := ./build
report_dir := ./report
sim_dir := ./sim
sim_bld_dir := $(sim_dir)/build
sim_log_dir := $(sim_dir)/log
gate_sim_bld_dir := $(sim_dir)/gate_build
gate_sim_log_dir := $(sim_dir)/gate_log
REPO_ROOT := $(abspath ../..)
WAVE_DUMP ?= 0
WAVE_DEPTH ?= 5
WAVE_DUMP_ENABLED := $(filter 1 y yes true TRUE YES,$(WAVE_DUMP))
VERDI_HOME ?= /usr/cad/synopsys/verdi/2024.09-sp1
NOVAS_HOME ?= $(VERDI_HOME)
VERDI_PLI_DIR ?= $(NOVAS_HOME)/share/PLI/VCS/LINUX64
TB_FSDB_VCS_FLAGS := $(if $(WAVE_DUMP_ENABLED),-P $(VERDI_PLI_DIR)/verdi.tab -load $(VERDI_PLI_DIR)/libnovas.so:FSDBDumpCmd +define+TB_ENABLE_FSDB_DUMP,)

VCS ?= vcs
DW_SIM_VER := /usr/cad/synopsys/synthesis/2024.09-sp2/dw/sim_ver
# Standard cell and TSMC SRAM Verilog simulation models
STDCELL_V := /cad/process/ADFP/Executable_Package/Collaterals/IP/stdcell/N16ADFP_StdCell/VERILOG/N16ADFP_StdCell.v
SRAM_V    := /cad/process/ADFP/Executable_Package/Collaterals/IP/sram/N16ADFP_SRAM/VERILOG/N16ADFP_SRAM_100a.v
TSMC_SRAM_MODEL_DEFINE := +define+TSMC_SRAM_MODEL_LOADED
RTL_MACRO_SIM_FLAGS := +no_notifier +notimingcheck
SRAM_WRAPPER_SRC := src/utils/SRAM_Wrapper.sv
USE_FOUNDRY_SRAM_MODEL ?= 0

ifeq ($(USE_FOUNDRY_SRAM_MODEL),1)
RTL_SRAM_FLAGS := $(TSMC_SRAM_MODEL_DEFINE) $(RTL_MACRO_SIM_FLAGS) -v $(SRAM_V)
RTL_SRAM_SRCS :=
else
RTL_SRAM_FLAGS := $(RTL_MACRO_SIM_FLAGS)
RTL_SRAM_SRCS := $(SRAM_WRAPPER_SRC)
endif

VCS_FLAGS ?= -full64 -sverilog -debug_access+all -timescale=1ns/1ps \
	$(TB_FSDB_VCS_FLAGS) \
	$(RTL_SRAM_FLAGS) \
	-y $(DW_SIM_VER) +libext+.v
GATE_VCS_FLAGS := -full64 -sverilog -debug_access+all -timescale=1ns/1ps \
	$(TB_FSDB_VCS_FLAGS) \
	+define+GATE_SIM $(TSMC_SRAM_MODEL_DEFINE) +neg_tchk +sdfverbose +no_notifier +notimingcheck \
	-v $(STDCELL_V) -v $(SRAM_V) \
	-y $(DW_SIM_VER) +libext+.v
GATE_SIM_COMPILE_FILTER_REGEX ?= ^(Parsing design file|Parsing included file|Back to file )
GATE_SIM_RUN_FILTER_REGEX ?= CLK_OPERATION : input CEB unknown/high-Z|^Info : Set Memory Content to all x at
GATE_TOP_NETLIST_DIR ?= $(TOP_SYN_OUT_ROOT)
GATE_TOP_SDF_FILE ?= $(GATE_TOP_NETLIST_DIR)/HybridAcc/HybridAcc.sdf
GATE_TOP_LOG_PREFIX ?= $(gate_sim_log_dir)/tb_hybridacc_sim_gate
GATE_TOP_COMPARE_RTOL ?= $(RTL_FW_COMPARE_RTOL)
GATE_TOP_COMPARE_ATOL ?= $(RTL_FW_COMPARE_ATOL)
GATE_TOP_COMPARE_SHOW ?= $(RTL_FW_COMPARE_SHOW)
GATE_PKG_SRCS := src/hybridacc_utils_pkg.sv src/Core/core_pkg.sv src/Cluster/cluster_pkg.sv
RTL_INCDIRS := +incdir+tb +incdir+src +incdir+src/utils +incdir+src/Cluster +incdir+src/Core +incdir+src/PE +incdir+src/NoC
SIM_COMMON_PLUSARGS := $(if $(TEST_DATA_DIR),+DATA_DIR=$(TEST_DATA_DIR),) \
	$(if $(VERIFY_TOL),+VERIFY_TOL=$(VERIFY_TOL),) \
	$(if $(CLOCK_PERIOD_NS),+CLOCK_PERIOD_NS=$(CLOCK_PERIOD_NS),) \
	$(if $(WAVE_DUMP_ENABLED),+WAVE_DUMP +WAVE_DEPTH=$(WAVE_DEPTH),)
SIM_PLUSARGS ?=
UV ?= uv
HACC_COMPILE := $(UV) run hacc-compile
RISCV_TOOL_PREFIX ?= riscv32-unknown-elf-
RISCV_OBJCOPY ?= $(RISCV_TOOL_PREFIX)objcopy
RISCV_OBJDUMP ?= $(RISCV_TOOL_PREFIX)objdump
RISCV_SIZE ?= $(RISCV_TOOL_PREFIX)size
RTL_FW_REGRESS_ROOT ?= $(REPO_ROOT)/output/rtl-fw-regress
RTL_FW_IMAGE_TOOL := $(REPO_ROOT)/scripts/gen_flat_fw_mem.py
RTL_FW_ISRAM_BYTES ?= 65536
RTL_FW_DSRAM_BASE ?= 0x10000000
RTL_FW_DSRAM_BYTES ?= 65536
RTL_FW_MAX_LOADER_CYCLES ?= 262144
RTL_FW_MAX_CORE_CYCLES ?= 500000
RTL_FW_TEST_SEED ?= 42
# Leave empty to suppress trace display.
# Example: RTL_FW_DEBUG_PLUSARGS="+TRACE_CLUSTER_RUNTIME +TRACE_CLUSTER_MMIO"
# Example: RTL_FW_DEBUG_PLUSARGS="+TRACE_CLUSTER_DEBUG"
RTL_FW_DEBUG_PLUSARGS ?=
RTL_FW_COMPARE_RTOL ?= 3e-2
RTL_FW_COMPARE_ATOL ?= 1.5e-2
RTL_FW_COMPARE_SHOW ?= 10
JASPER ?= jg
JASPER_CSHRC ?= /usr/cad/cadence/CIC/jasper.cshrc
JASPER_FLAGS ?= -superlint -allow_unsupported_OS -batch
SUPERLINT_REPORT_QUERY_TCL := $(abspath script/superlint/tcl/jasper_report_query.tcl)
SUPERLINT_HOTSPOT_QUERY_TCL := $(abspath script/superlint/tcl/jasper_error_hotspot_query.tcl)
SUPERLINT_REPORT_OUT ?= $(REPO_ROOT)/output/jasper_report_current.out
SUPERLINT_HOTSPOT_OUT ?= $(REPO_ROOT)/output/jasper_error_hotspot_current.out
HACC_CC_EXAMPLE_ROOT := $(REPO_ROOT)/design/hybridacc-cc/example
RTL_REGRESS_CONV2D_1X1_SINGLE_WAVE_YAML := $(HACC_CC_EXAMPLE_ROOT)/conv1x1/conv2d_1x1_single_wave.yaml
RTL_REGRESS_CONV2D_3X3_SINGLE_WAVE_YAML := $(HACC_CC_EXAMPLE_ROOT)/conv3x3/conv2d_3x3_single_wave.yaml
RTL_REGRESS_GEMM_SINGLE_WAVE_YAML := $(HACC_CC_EXAMPLE_ROOT)/gemm/gemm_single_wave.yaml
RTL_REGRESS_CONV2D_1X1_SINGLE_WAVE_OUT := $(RTL_FW_REGRESS_ROOT)/conv2d_1x1_single_wave
RTL_REGRESS_CONV2D_3X3_SINGLE_WAVE_OUT := $(RTL_FW_REGRESS_ROOT)/conv2d_3x3_single_wave
RTL_REGRESS_GEMM_SINGLE_WAVE_OUT := $(RTL_FW_REGRESS_ROOT)/gemm_single_wave
RTL_REGRESS_CONV2D_1X1_SINGLE_WAVE_LOG_PREFIX := $(sim_log_dir)/tb_hybridacc_sim_conv2d_1x1_single_wave
RTL_REGRESS_CONV2D_3X3_SINGLE_WAVE_LOG_PREFIX := $(sim_log_dir)/tb_hybridacc_sim_conv2d_3x3_single_wave
RTL_REGRESS_GEMM_SINGLE_WAVE_LOG_PREFIX := $(sim_log_dir)/tb_hybridacc_sim_gemm_single_wave

TB_ALL := $(shell find tb -type f -name 'tb_*.sv' ! -path 'tb/Cluster/tb_sram.sv')
TB_PE := $(shell find tb/PE -type f -name 'tb_*.sv' 2>/dev/null)
TB_NOC := $(shell find tb/NoC -type f -name 'tb_*.sv' 2>/dev/null)
TB_CLUSTER := $(shell find tb/Cluster -type f -name 'tb_*.sv' ! -name 'tb_sram.sv' 2>/dev/null)
SYN_TCL := $(shell find script -maxdepth 1 -type f -name 'synthesis_*.tcl')

DC_SHELL ?= dc_shell
SYN_MAX_CORES ?= 8
SYN_MAX_MEMORY_GB ?= 128
SYN_RESOURCE_LIMIT_DESC := cores=$(SYN_MAX_CORES) mem=$(SYN_MAX_MEMORY_GB)GB
SYN_BASH_RESOURCE_LIMIT := ulimit -Sv $$(( $(SYN_MAX_MEMORY_GB) * 1024 * 1024 ))
# Top-level synthesis defaults derive from the effective clock so runs do not collide.
TOP_SYN_CLOCK_PERIOD_NS := $(if $(strip $(CLOCK_PERIOD_NS)),$(strip $(CLOCK_PERIOD_NS)),1.0)
TOP_SYN_CLK_TAG := clk_$(shell printf '%s' "$(TOP_SYN_CLOCK_PERIOD_NS)" | sed -E 's/^\./0./; /\./! s/$$/.00/; /\.[0-9]$$/ s/$$/0/; s/\./p/g')ns
SYN_BUILD_DIR ?= $(bld_dir)/$(TOP_SYN_CLK_TAG)
SYN_LOG_NAME ?= syn_compile_hybridacc_$(TOP_SYN_CLK_TAG).log
TOP_SYN_RUN_TAG ?= $(TOP_SYN_CLK_TAG)
TOP_SYN_OUT_ROOT ?= $(abspath $(syn_dir)/$(TOP_SYN_CLK_TAG))
TOP_SYN_REPORT_OUT_ROOT ?= $(abspath $(report_dir)/$(TOP_SYN_CLK_TAG))
SYN_DC_ENV := CLOCK_PERIOD_NS="$(CLOCK_PERIOD_NS)" RUN_TAG="$(RUN_TAG)" SYN_OUT_ROOT="$(SYN_OUT_ROOT)" REPORT_OUT_ROOT="$(REPORT_OUT_ROOT)" SYN_MAX_CORES="$(SYN_MAX_CORES)"
TOP_SYN_DC_ENV := CLOCK_PERIOD_NS="$(TOP_SYN_CLOCK_PERIOD_NS)" RUN_TAG="$(TOP_SYN_RUN_TAG)" SYN_OUT_ROOT="$(TOP_SYN_OUT_ROOT)" REPORT_OUT_ROOT="$(TOP_SYN_REPORT_OUT_ROOT)" SYN_MAX_CORES="$(SYN_MAX_CORES)" TOP_SYN_MAX_CORES="$(SYN_MAX_CORES)"
TOP_SYN_TCL := $(abspath script/syn_hybridacc.tcl)
DC_SETUP_FILE := $(abspath script/synopsys_dc.setup)
SNPSLMD_LICENSE_FILE ?= 26585@lstn
export SNPSLMD_LICENSE_FILE

PE_UNITS := DataMemory Decoder InstructionMemory LoopController PsumRegFile \
            TransformRegFile VADDU VMULU LDMA SDMA \
            IF_ID_Stage EXE_M_Stage EXE_A_Stage PErouter ProcessElement

NOC_UNITS := MBUS NoCRouter NetworkOnChip

CLUSTER_UNITS := AddressGenerateUnit ScratchpadMemory HybridDataDeliverUnit ComputeCluster

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
	@echo "  superlint_report     - Run Jasper Superlint whole-design report query"
	@echo "  superlint_hotspot    - Run Jasper Superlint error-hotspot query"
	@echo "  superlint            - Run both Jasper Superlint report and hotspot queries"
	@echo "  rtl_regress_single_wave             - Compile 3 hybridacc-cc single-wave YAMLs and run RTL/VCS firmware regression"
	@echo "  rtl_regress_conv2d_1x1_single_wave  - Run RTL/VCS firmware regression for conv2d_1x1_single_wave.yaml"
	@echo "  rtl_regress_conv2d_3x3_single_wave  - Run RTL/VCS firmware regression for conv2d_3x3_single_wave.yaml"
	@echo "  rtl_regress_gemm_single_wave        - Run RTL/VCS firmware regression for gemm_single_wave.yaml"
	@echo "  gate_sim_<tb|module> - Post-sim: compile and run a testbench (gate-level + SDF)"
	@echo "  gate_sim_pe          - Post-sim: run all PE testbenches (gate-level + SDF)"
	@echo "  gate_sim_noc         - Post-sim: run all NoC testbenches (gate-level + SDF)"
	@echo "  gate_sim_regress_single_wave             - Compile 3 hybridacc-cc single-wave YAMLs and run gate-level/VCS firmware regression"
	@echo "  gate_sim_regress_conv2d_1x1_single_wave  - Run gate-level/VCS firmware regression for conv2d_1x1_single_wave.yaml"
	@echo "  gate_sim_regress_conv2d_3x3_single_wave  - Run gate-level/VCS firmware regression for conv2d_3x3_single_wave.yaml"
	@echo "  gate_sim_regress_gemm_single_wave        - Run gate-level/VCS firmware regression for gemm_single_wave.yaml"
	@echo "  gate_regress_<name>  - Gate-level top single-wave regression for conv2d_1x1 / conv2d_3x3 / gemm"
	@echo "  synthesize_<name>    - Run script/synthesis_<name>.tcl"
	@echo "  syn_top              - Synthesize top-level HybridAcc via script/syn_hybridacc.tcl"
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
	@echo "  clean_superlint      - Remove output Jasper/Superlint logs, run dirs, and Tcl probes"
	@echo "  clean                - Remove simulation build artifacts"
	@echo "  clean_syn            - Remove synthesized files and reports"
	@echo "Variables: TEST_DATA_DIR=... VERIFY_TOL=... CLOCK_PERIOD_NS=... WAVE_DUMP=0|1 WAVE_DEPTH=... VERDI_HOME=... NOVAS_HOME=... RUN_TAG=... SYN_OUT_ROOT=... REPORT_OUT_ROOT=... SYN_BUILD_DIR=... SYN_LOG_NAME=... SYN_MAX_CORES=... SYN_MAX_MEMORY_GB=... GATE_TOP_NETLIST_DIR=... GATE_TOP_SDF_FILE=... GATE_SIM_RUN_FILTER=... RTL_FW_REGRESS_ROOT=... RTL_FW_DEBUG_PLUSARGS='+TRACE_CLUSTER_DEBUG' RTL_FW_TEST_SEED=... UV=... RISCV_TOOL_PREFIX=... JASPER=... JASPER_CSHRC=... SUPERLINT_REPORT_OUT=... SUPERLINT_HOTSPOT_OUT=..."

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
	$(VCS) $(VCS_FLAGS) +incdir+$$tb_dir $(RTL_INCDIRS) $(RTL_SRAM_SRCS) $$tb_file -o $(sim_bld_dir)/$$tb_base.simv > $(sim_log_dir)/$$tb_base.compile.log 2>&1 || \
		{ echo "[SIM] ERROR: VCS compile failed. See $(sim_log_dir)/$$tb_base.compile.log"; exit 1; }; \
	if [ ! -x $(sim_bld_dir)/$$tb_base.simv ]; then \
		echo "[SIM] ERROR: simv was not generated. See $(sim_log_dir)/$$tb_base.compile.log"; \
		exit 1; \
	fi; \
	echo "[SIM] Running $$tb_base ..."; \
	$(sim_bld_dir)/$$tb_base.simv $(SIM_COMMON_PLUSARGS) $(SIM_PLUSARGS) > $(sim_log_dir)/$$tb_base.run.log 2>&1 || \
		{ echo "[SIM] ERROR: Simulation failed. See $(sim_log_dir)/$$tb_base.run.log"; exit 1; }; \
	if grep -Eq 'Fatal:|\[FAIL|\[TB_TIMEOUT\]| FAIL \(' $(sim_log_dir)/$$tb_base.run.log; then \
		echo "[SIM] ERROR: Simulation log indicates failure. See $(sim_log_dir)/$$tb_base.run.log"; \
		tail -40 $(sim_log_dir)/$$tb_base.run.log; \
		exit 1; \
	fi; \
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

define run_rtl_fw_regress
.PHONY: rtl_regress_$(1)
rtl_regress_$(1): | $(sim_bld_dir) $(sim_log_dir)
	@set -e; \
	echo "[RTL_REGRESS] [$(1)] Compiling $(2) -> $(3)"; \
	mkdir -p "$(3)"; \
	cd $(REPO_ROOT) && $(HACC_COMPILE) "$(2)" -o "$(3)" --dump-ir; \
	cd "$(CURDIR)"; \
	echo "[RTL_REGRESS] [$(1)] Generating $(3)/firmware.mem"; \
	fw_bytes=`cd $(REPO_ROOT) && $(UV) run python "$(RTL_FW_IMAGE_TOOL)" \
		--elf "$(3)/firmware.elf" \
		--output "$(3)/firmware.mem" \
		--objcopy "$(RISCV_OBJCOPY)" \
		--objdump "$(RISCV_OBJDUMP)" \
		--isram-bytes "$(RTL_FW_ISRAM_BYTES)" \
		--dsram-base "$(RTL_FW_DSRAM_BASE)" \
		--dsram-bytes "$(RTL_FW_DSRAM_BYTES)"`; \
	echo "[RTL_REGRESS] [$(1)] Generating $(3)/dram_init.bin + golden_output.bin"; \
	cd $(REPO_ROOT) && $(UV) run python -m hybridacc_verify.gen.gen_test_dram \
		--ir "$(3)/hardware_ir.json" \
		--workload "$(2)" \
		--output-dir "$(3)" \
		--seed "$(RTL_FW_TEST_SEED)"; \
	cd "$(CURDIR)"; \
	dram_bytes=`wc -c < "$(3)/dram_init.bin" | tr -d '[:space:]'`; \
	golden_bytes=`wc -c < "$(3)/golden_output.bin" | tr -d '[:space:]'`; \
	dram_base=`sed -n 's/^dram_base=0x//p' "$(3)/golden_meta.txt"`; \
	golden_base=`sed -n 's/^dram_output_base=0x//p' "$(3)/golden_meta.txt"`; \
	if [ -z "$$$$dram_base" ] || [ -z "$$$$golden_base" ]; then \
		echo "[RTL_REGRESS] [$(1)] ERROR: failed to parse DRAM base metadata"; \
		exit 1; \
	fi; \
	actual_output="$(3)/rtl_actual_output.bin"; \
	echo "[RTL_REGRESS] [$(1)] Running tb_hybridacc_sim (FW_BYTES=$$$$fw_bytes DRAM_BYTES=$$$$dram_bytes GOLDEN_BYTES=$$$$golden_bytes)"; \
	set +e; \
	$(MAKE) -C "$(CURDIR)" sim_tb_hybridacc_sim TEST_DATA_DIR= VERIFY_TOL= CLOCK_PERIOD_NS= \
		SIM_PLUSARGS="+FW_MEM=$(3)/firmware.mem +FW_BYTES=$$$$fw_bytes +DRAM_MIRROR=$(3)/dram_init.bin +DRAM_MIRROR_BYTES=$$$$dram_bytes +DRAM_MIRROR_BASE=$$$$dram_base +GOLDEN_OUTPUT=$(3)/golden_output.bin +GOLDEN_OUTPUT_BYTES=$$$$golden_bytes +GOLDEN_OUTPUT_BASE=$$$$golden_base +ACTUAL_OUTPUT_DUMP=$$$$actual_output +SKIP_FW_TEST_SUMMARY +SKIP_GOLDEN_EXACT_CHECK +MAX_LOADER_CYCLES=$(RTL_FW_MAX_LOADER_CYCLES) +MAX_CORE_CYCLES=$(RTL_FW_MAX_CORE_CYCLES) $(RTL_FW_DEBUG_PLUSARGS)"; \
	rc=$$$$?; \
	if [ $$$$rc -eq 0 ]; then \
		echo "[RTL_REGRESS] [$(1)] Comparing rtl_actual_output.bin against golden_output.bin"; \
		cd $(REPO_ROOT) && $(UV) run python -m hybridacc_verify.check.comparator \
			--sim "$$$$actual_output" \
			--expected "$(3)/golden_output.bin" \
			--rtol "$(RTL_FW_COMPARE_RTOL)" \
			--atol "$(RTL_FW_COMPARE_ATOL)" \
			--show "$(RTL_FW_COMPARE_SHOW)" \
			> "$(CURDIR)/$(4).compare.log" 2>&1; \
		rc=$$$$?; \
		cd "$(CURDIR)"; \
		cat "$(CURDIR)/$(4).compare.log"; \
	fi; \
	set -e; \
	cp "$(CURDIR)"/$(sim_log_dir)/tb_hybridacc_sim.compile.log "$(4).compile.log"; \
	cp "$(CURDIR)"/$(sim_log_dir)/tb_hybridacc_sim.run.log "$(4).run.log"; \
	echo "[RTL_REGRESS] [$(1)] Logs: $(4).compile.log $(4).run.log $(4).compare.log"; \
	if [ $$$$rc -ne 0 ]; then \
		exit $$$$rc; \
	fi
endef

$(eval $(call run_rtl_fw_regress,conv2d_1x1_single_wave,$(RTL_REGRESS_CONV2D_1X1_SINGLE_WAVE_YAML),$(RTL_REGRESS_CONV2D_1X1_SINGLE_WAVE_OUT),$(RTL_REGRESS_CONV2D_1X1_SINGLE_WAVE_LOG_PREFIX)))
$(eval $(call run_rtl_fw_regress,conv2d_3x3_single_wave,$(RTL_REGRESS_CONV2D_3X3_SINGLE_WAVE_YAML),$(RTL_REGRESS_CONV2D_3X3_SINGLE_WAVE_OUT),$(RTL_REGRESS_CONV2D_3X3_SINGLE_WAVE_LOG_PREFIX)))
$(eval $(call run_rtl_fw_regress,gemm_single_wave,$(RTL_REGRESS_GEMM_SINGLE_WAVE_YAML),$(RTL_REGRESS_GEMM_SINGLE_WAVE_OUT),$(RTL_REGRESS_GEMM_SINGLE_WAVE_LOG_PREFIX)))

.PHONY: rtl_regress_single_wave
rtl_regress_single_wave:
	@set -e; \
	fail=0; \
	for target in \
		rtl_regress_conv2d_1x1_single_wave \
		rtl_regress_conv2d_3x3_single_wave \
		rtl_regress_gemm_single_wave; do \
		if ! $(MAKE) $$target; then \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	if [ $$fail -ne 0 ]; then \
		echo "[RTL_REGRESS] ERROR: $$fail case(s) failed. Inspect case-specific logs under $(sim_log_dir)."; \
		exit 1; \
	fi

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
	compile_status_file=$(gate_sim_log_dir)/$$tb_base.compile.status; \
	rm -f $$compile_status_file; \
	( $(VCS) $(GATE_VCS_FLAGS) +incdir+$$tb_dir $(RTL_INCDIRS) \
		$(GATE_PKG_SRCS) $$netlist $$tb_file \
		-o $(gate_sim_bld_dir)/$$tb_base.gate.simv; \
		echo $$? > $$compile_status_file ) 2>&1 \
		| awk -v re='$(GATE_SIM_COMPILE_FILTER_REGEX)' '$$0 !~ re { print }' > $(gate_sim_log_dir)/$$tb_base.compile.log; \
	compile_rc=$$(cat $$compile_status_file); \
	rm -f $$compile_status_file; \
	if [ $$compile_rc -ne 0 ]; then \
		echo "[GATE_SIM] ERROR: VCS compile failed. See $(gate_sim_log_dir)/$$tb_base.compile.log"; \
		exit $$compile_rc; \
	fi; \
	if [ ! -x $(gate_sim_bld_dir)/$$tb_base.gate.simv ]; then \
		echo "[GATE_SIM] ERROR: gate simv was not generated. See $(gate_sim_log_dir)/$$tb_base.compile.log"; \
		exit 1; \
	fi; \
	echo "[GATE_SIM] Running $$tb_base ..."; \
	status_file=$(gate_sim_log_dir)/$$tb_base.run.status; \
	rm -f $$status_file; \
	( $(gate_sim_bld_dir)/$$tb_base.gate.simv $(SIM_COMMON_PLUSARGS) $(SIM_PLUSARGS); echo $$? > $$status_file ) 2>&1 \
		| awk -v re='$(GATE_SIM_RUN_FILTER_REGEX)' '$$0 !~ re { print }' > $(gate_sim_log_dir)/$$tb_base.run.log; \
	sim_rc=$$(cat $$status_file); \
	rm -f $$status_file; \
	if [ $$sim_rc -ne 0 ]; then \
		echo "[GATE_SIM] ERROR: Simulation failed. See $(gate_sim_log_dir)/$$tb_base.run.log"; \
		exit $$sim_rc; \
	fi; \
	if grep -Eq 'Fatal:|\[FAIL|\[TB_TIMEOUT\]| FAIL \(' $(gate_sim_log_dir)/$$tb_base.run.log; then \
		echo "[GATE_SIM] ERROR: Simulation log indicates failure. See $(gate_sim_log_dir)/$$tb_base.run.log"; \
		tail -40 $(gate_sim_log_dir)/$$tb_base.run.log; \
		exit 1; \
	fi; \
	echo "[GATE_SIM] Logs: $(gate_sim_log_dir)/$$tb_base.compile.log $(gate_sim_log_dir)/$$tb_base.run.log"
endef

define run_gate_fw_regress
.PHONY: gate_regress_$(1)
gate_regress_$(1): | $(gate_sim_bld_dir) $(gate_sim_log_dir)
	@set -e; \
	echo "[GATE_REGRESS] [$(1)] Compiling $(2) -> $(3)"; \
	mkdir -p "$(3)"; \
	cd $(REPO_ROOT) && $(HACC_COMPILE) "$(2)" -o "$(3)" --dump-ir; \
	cd "$(CURDIR)"; \
	echo "[GATE_REGRESS] [$(1)] Generating $(3)/firmware.mem"; \
	fw_bytes=`cd $(REPO_ROOT) && $(UV) run python "$(RTL_FW_IMAGE_TOOL)" \
		--elf "$(3)/firmware.elf" \
		--output "$(3)/firmware.mem" \
		--objcopy "$(RISCV_OBJCOPY)" \
		--objdump "$(RISCV_OBJDUMP)" \
		--isram-bytes "$(RTL_FW_ISRAM_BYTES)" \
		--dsram-base "$(RTL_FW_DSRAM_BASE)" \
		--dsram-bytes "$(RTL_FW_DSRAM_BYTES)"`; \
	echo "[GATE_REGRESS] [$(1)] Generating $(3)/dram_init.bin + golden_output.bin"; \
	cd $(REPO_ROOT) && $(UV) run python -m hybridacc_verify.gen.gen_test_dram \
		--ir "$(3)/hardware_ir.json" \
		--workload "$(2)" \
		--output-dir "$(3)" \
		--seed "$(RTL_FW_TEST_SEED)"; \
	cd "$(CURDIR)"; \
	dram_bytes=`wc -c < "$(3)/dram_init.bin" | tr -d '[:space:]'`; \
	golden_bytes=`wc -c < "$(3)/golden_output.bin" | tr -d '[:space:]'`; \
	dram_base=`sed -n 's/^dram_base=0x//p' "$(3)/golden_meta.txt"`; \
	golden_base=`sed -n 's/^dram_output_base=0x//p' "$(3)/golden_meta.txt"`; \
	if [ -z "$$$$dram_base" ] || [ -z "$$$$golden_base" ]; then \
		echo "[GATE_REGRESS] [$(1)] ERROR: failed to parse DRAM base metadata"; \
		exit 1; \
	fi; \
	actual_output="$(3)/gate_actual_output.bin"; \
	echo "[GATE_REGRESS] [$(1)] Running tb_hybridacc_sim gate-level"; \
	set +e; \
	$(MAKE) -C "$(CURDIR)" gate_sim_tb_hybridacc_sim MOD_NAME=HybridAcc GATE_NETLIST_DIR="$(GATE_TOP_NETLIST_DIR)" \
		SIM_PLUSARGS="+SDF_FILE=$(GATE_TOP_SDF_FILE) +FW_MEM=$(3)/firmware.mem +FW_BYTES=$$$$fw_bytes +DRAM_MIRROR=$(3)/dram_init.bin +DRAM_MIRROR_BYTES=$$$$dram_bytes +DRAM_MIRROR_BASE=$$$$dram_base +GOLDEN_OUTPUT=$(3)/golden_output.bin +GOLDEN_OUTPUT_BYTES=$$$$golden_bytes +GOLDEN_OUTPUT_BASE=$$$$golden_base +ACTUAL_OUTPUT_DUMP=$$$$actual_output +SKIP_FW_TEST_SUMMARY +SKIP_GOLDEN_EXACT_CHECK +MAX_LOADER_CYCLES=$(RTL_FW_MAX_LOADER_CYCLES) +MAX_CORE_CYCLES=$(RTL_FW_MAX_CORE_CYCLES) $(RTL_FW_DEBUG_PLUSARGS)"; \
	rc=$$$$?; \
	if [ $$$$rc -eq 0 ]; then \
		echo "[GATE_REGRESS] [$(1)] Comparing gate_actual_output.bin against golden_output.bin"; \
		cd $(REPO_ROOT) && $(UV) run python -m hybridacc_verify.check.comparator \
			--sim "$$$$actual_output" \
			--expected "$(3)/golden_output.bin" \
			--rtol "$(GATE_TOP_COMPARE_RTOL)" \
			--atol "$(GATE_TOP_COMPARE_ATOL)" \
			--show "$(GATE_TOP_COMPARE_SHOW)" \
			> "$(CURDIR)/$(4).compare.log" 2>&1; \
		rc=$$$$?; \
		cd "$(CURDIR)"; \
		cat "$(CURDIR)/$(4).compare.log"; \
	fi; \
	set -e; \
	cp "$(CURDIR)"/$(gate_sim_log_dir)/tb_hybridacc_sim.compile.log "$(4).compile.log"; \
	cp "$(CURDIR)"/$(gate_sim_log_dir)/tb_hybridacc_sim.run.log "$(4).run.log"; \
	echo "[GATE_REGRESS] [$(1)] Logs: $(4).compile.log $(4).run.log $(4).compare.log"; \
	if [ $$$$rc -ne 0 ]; then \
		exit $$$$rc; \
	fi
endef

$(eval $(call run_gate_fw_regress,conv2d_1x1_single_wave,$(RTL_REGRESS_CONV2D_1X1_SINGLE_WAVE_YAML),$(RTL_REGRESS_CONV2D_1X1_SINGLE_WAVE_OUT),$(gate_sim_log_dir)/tb_hybridacc_sim_gate_conv2d_1x1_single_wave))
$(eval $(call run_gate_fw_regress,conv2d_3x3_single_wave,$(RTL_REGRESS_CONV2D_3X3_SINGLE_WAVE_YAML),$(RTL_REGRESS_CONV2D_3X3_SINGLE_WAVE_OUT),$(gate_sim_log_dir)/tb_hybridacc_sim_gate_conv2d_3x3_single_wave))
$(eval $(call run_gate_fw_regress,gemm_single_wave,$(RTL_REGRESS_GEMM_SINGLE_WAVE_YAML),$(RTL_REGRESS_GEMM_SINGLE_WAVE_OUT),$(gate_sim_log_dir)/tb_hybridacc_sim_gate_gemm_single_wave))

.PHONY: gate_sim_regress_conv2d_1x1_single_wave gate_sim_regress_conv2d_3x3_single_wave gate_sim_regress_gemm_single_wave
gate_sim_regress_conv2d_1x1_single_wave: gate_regress_conv2d_1x1_single_wave

gate_sim_regress_conv2d_3x3_single_wave: gate_regress_conv2d_3x3_single_wave

gate_sim_regress_gemm_single_wave: gate_regress_gemm_single_wave

.PHONY: gate_regress_single_wave
gate_regress_single_wave:
	@set -e; \
	fail=0; \
	for target in \
		gate_regress_conv2d_1x1_single_wave \
		gate_regress_conv2d_3x3_single_wave \
		gate_regress_gemm_single_wave; do \
		if ! $(MAKE) $$target; then \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	if [ $$fail -ne 0 ]; then \
		echo "[GATE_REGRESS] ERROR: $$fail case(s) failed. Inspect case-specific logs under $(gate_sim_log_dir)."; \
		exit 1; \
	fi

.PHONY: gate_sim_regress_single_wave
gate_sim_regress_single_wave: gate_regress_single_wave

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
	echo "[SYN] Resource cap: $(SYN_RESOURCE_LIMIT_DESC)"; \
	status_file="$$(pwd)/.dc_status"; \
	rm -f "$$status_file"; \
	( bash -lc '$(SYN_BASH_RESOURCE_LIMIT); $(SYN_DC_ENV) $(DC_SHELL) -no_home_init -f ../script/synthesis_$*.tcl'; \
	  echo $$? > "$$status_file" ) | tee syn_compile.log; \
	dc_rc=$$(cat "$$status_file"); \
	rm -f "$$status_file"; \
	if [ $$dc_rc -ne 0 ]; then \
		echo "[SYN] ERROR: synthesis_$*.tcl failed under resource cap $(SYN_RESOURCE_LIMIT_DESC)"; \
		exit $$dc_rc; \
	fi

syn_top: | $(bld_dir) $(syn_dir) $(report_dir)
	@echo "[SYN] Synthesizing top-level HybridAcc ..."
	@build_dir="$(SYN_BUILD_DIR)"; \
	mkdir -p "$$build_dir"; \
	cp "$(DC_SETUP_FILE)" "$$build_dir/.synopsys_dc.setup"; \
	cd "$$build_dir"; \
	echo "[SYN] Resource cap: $(SYN_RESOURCE_LIMIT_DESC)"; \
	status_file="$$(pwd)/.dc_status"; \
	rm -f "$$status_file"; \
	( bash -lc '$(SYN_BASH_RESOURCE_LIMIT); $(TOP_SYN_DC_ENV) $(DC_SHELL) -no_home_init -f "$(TOP_SYN_TCL)"'; \
	  echo $$? > "$$status_file" ) | tee "$(SYN_LOG_NAME)"; \
	dc_rc=$$(cat "$$status_file"); \
	rm -f "$$status_file"; \
	if [ $$dc_rc -ne 0 ]; then \
		echo "[SYN] ERROR: top-level synthesis failed under resource cap $(SYN_RESOURCE_LIMIT_DESC)"; \
		exit $$dc_rc; \
	fi
	@echo "[SYN] Done: HybridAcc  Build dir: $(SYN_BUILD_DIR)  Syn root: $(TOP_SYN_OUT_ROOT)  Report root: $(TOP_SYN_REPORT_OUT_ROOT)  Log: $(SYN_LOG_NAME)"

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
	echo "[SYN] Resource cap: $(SYN_RESOURCE_LIMIT_DESC)"; \
	status_file="$$(pwd)/.dc_status"; \
	rm -f "$$status_file"; \
	( bash -lc '$(SYN_BASH_RESOURCE_LIMIT); $(SYN_DC_ENV) $(DC_SHELL) -no_home_init -x "set MOD_NAME $*" -f ../script/synthesis_pe_units.tcl'; \
	  echo $$? > "$$status_file" ) | tee ../$(report_dir)/$*/syn_compile_$*.log; \
	dc_rc=$$(cat "$$status_file"); \
	rm -f "$$status_file"; \
	if [ $$dc_rc -ne 0 ]; then \
		echo "[SYN] ERROR: PE unit $* failed under resource cap $(SYN_RESOURCE_LIMIT_DESC)"; \
		exit $$dc_rc; \
	fi
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
	echo "[SYN] Resource cap: $(SYN_RESOURCE_LIMIT_DESC)"; \
	status_file="$$(pwd)/.dc_status"; \
	rm -f "$$status_file"; \
	( bash -lc '$(SYN_BASH_RESOURCE_LIMIT); $(SYN_DC_ENV) $(DC_SHELL) -no_home_init -x "set MOD_NAME $*" -f ../script/synthesis_noc_units.tcl'; \
	  echo $$? > "$$status_file" ) | tee ../$(report_dir)/$*/syn_compile_$*.log; \
	dc_rc=$$(cat "$$status_file"); \
	rm -f "$$status_file"; \
	if [ $$dc_rc -ne 0 ]; then \
		echo "[SYN] ERROR: NoC unit $* failed under resource cap $(SYN_RESOURCE_LIMIT_DESC)"; \
		exit $$dc_rc; \
	fi
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
	echo "[SYN] Resource cap: $(SYN_RESOURCE_LIMIT_DESC)"; \
	status_file="$$(pwd)/.dc_status"; \
	rm -f "$$status_file"; \
	( bash -lc '$(SYN_BASH_RESOURCE_LIMIT); $(SYN_DC_ENV) $(DC_SHELL) -no_home_init -x "set MOD_NAME $*" -f ../script/synthesis_cluster_units.tcl'; \
	  echo $$? > "$$status_file" ) | tee ../$(report_dir)/$*/syn_compile_$*.log; \
	dc_rc=$$(cat "$$status_file"); \
	rm -f "$$status_file"; \
	if [ $$dc_rc -ne 0 ]; then \
		echo "[SYN] ERROR: Cluster unit $* failed under resource cap $(SYN_RESOURCE_LIMIT_DESC)"; \
		exit $$dc_rc; \
	fi
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

superlint_report:
	@mkdir -p "$(REPO_ROOT)/output"
	@echo "[SUPERLINT] Running whole-design report query ..."
	@tcsh -ic 'source "$(JASPER_CSHRC)" >& /dev/null; setenv HACC_JG_PROJECT_ROOT "$(CURDIR)"; cd "$(REPO_ROOT)"; "$(JASPER)" $(JASPER_FLAGS) "$(SUPERLINT_REPORT_QUERY_TCL)" |& tee "$(SUPERLINT_REPORT_OUT)"'
	@echo "[SUPERLINT] Report written to $(SUPERLINT_REPORT_OUT)"

superlint_hotspot:
	@mkdir -p "$(REPO_ROOT)/output"
	@echo "[SUPERLINT] Running error-hotspot query ..."
	@tcsh -ic 'source "$(JASPER_CSHRC)" >& /dev/null; setenv HACC_JG_PROJECT_ROOT "$(CURDIR)"; cd "$(REPO_ROOT)"; "$(JASPER)" $(JASPER_FLAGS) "$(SUPERLINT_HOTSPOT_QUERY_TCL)" |& tee "$(SUPERLINT_HOTSPOT_OUT)"'
	@echo "[SUPERLINT] Hotspot written to $(SUPERLINT_HOTSPOT_OUT)"

superlint: superlint_report superlint_hotspot
	@echo "[SUPERLINT] Completed report + hotspot queries"

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

clean_superlint:
	@echo "[SUPERLINT] Removing output Jasper/Superlint artifacts ..."
	@find "$(REPO_ROOT)/output" -mindepth 1 -maxdepth 1 \
		\( -name 'jasper*' -o \( -name '*superlint*' ! -name 'superlint-report.md' \) \) \
		-exec rm -rf {} +
	@rm -rf "$(REPO_ROOT)/jgproject"
	@echo "[SUPERLINT] Output cleanup complete"

# ---------------------------------------------------------------------------
# sim_noc_sim — Full NoC system-level simulation using ESL test data
#   make sim_noc_sim TEST_DATA_DIR=output/noc-sim/conv_k3c4
#   make sim_noc_sim TEST_DATA_DIR=output/noc-sim/gemm VERIFY_TOL=0.05
#   make sim_noc_sim_all                      → run all cases under output/noc-sim/
# ---------------------------------------------------------------------------
sim_noc_sim: | $(sim_bld_dir) $(sim_log_dir)
	@echo "[NOC_SIM] Compiling tb_noc_sim.sv ..."; \
	$(VCS) $(VCS_FLAGS) +incdir+tb/NoC +incdir+tb $(RTL_SRAM_SRCS) \
		tb/NoC/tb_noc_sim.sv \
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
	if grep -Eq 'Fatal:|\[FAIL|\[TB_TIMEOUT\]| FAIL \(' $(sim_log_dir)/tb_noc_sim.run.log; then \
		echo "[NOC_SIM] ERROR: Simulation log indicates failure. See $(sim_log_dir)/tb_noc_sim.run.log"; \
		tail -40 $(sim_log_dir)/tb_noc_sim.run.log; \
		exit 1; \
	fi; \
	echo "[NOC_SIM] Logs: $(sim_log_dir)/tb_noc_sim.compile.log $(sim_log_dir)/tb_noc_sim.run.log"; \
	tail -30 $(sim_log_dir)/tb_noc_sim.run.log

sim_noc_sim_all: | $(sim_bld_dir) $(sim_log_dir)
	@echo "[NOC_SIM] Compiling tb_noc_sim.sv ..."; \
	$(VCS) $(VCS_FLAGS) +incdir+tb/NoC +incdir+tb \
		tb/NoC/tb_noc_sim.sv \
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

.PHONY: help sim_all sim_pe sim_noc sim_cluster sim_noc_fast sim_noc_sim sim_noc_sim_all syn_top synall syn_pe_all syn_noc_all syn_cluster_all syn_report sim_report gate_sim_report gate_sim_pe gate_sim_noc gate_regress_single_wave gate_regress_conv2d_1x1_single_wave gate_regress_conv2d_3x3_single_wave gate_regress_gemm_single_wave gate_sim_regress_single_wave gate_sim_regress_conv2d_1x1_single_wave gate_sim_regress_conv2d_3x3_single_wave gate_sim_regress_gemm_single_wave superlint superlint_report superlint_hotspot postsim_all clean clean_syn clean_superlint rtl_regress_single_wave rtl_regress_conv2d_1x1_single_wave rtl_regress_conv2d_3x3_single_wave rtl_regress_gemm_single_wave
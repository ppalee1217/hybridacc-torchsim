syn_dir := ./syn
bld_dir := ./build
report_dir := ./report
sim_dir := ./sim
sim_bld_dir := $(sim_dir)/build
sim_log_dir := $(sim_dir)/log
gate_sim_bld_dir := $(sim_dir)/gate_build
gate_sim_log_dir := $(sim_dir)/gate_log
REPO_ROOT := $(abspath $(RTL_ROOT)/../..)

SCRIPT_ROOT := $(RTL_ROOT)/script
SYN_SCRIPT_ROOT := $(SCRIPT_ROOT)/tcl/synthesis
PY_SCRIPT_ROOT := $(SCRIPT_ROOT)/python/reporting
PY_UTIL_SCRIPT_ROOT := $(SCRIPT_ROOT)/python/utils
SDC_ROOT := $(SCRIPT_ROOT)/sdc
SUPERLINT_QUERY_ROOT := $(SCRIPT_ROOT)/tcl/superlint
ANALYSIS_TCL_ROOT := $(SCRIPT_ROOT)/tcl/analysis
PRIMETIME_TCL_ROOT := $(ANALYSIS_TCL_ROOT)/primetime
PRIMEPOWER_TCL_ROOT := $(ANALYSIS_TCL_ROOT)/primepower

SYN_COMMON_TCL := $(abspath $(SYN_SCRIPT_ROOT)/syn_common.tcl)
TOP_SYN_TCL := $(abspath $(SYN_SCRIPT_ROOT)/syn_hybridacc.tcl)
SYN_PE_TCL := $(abspath $(SYN_SCRIPT_ROOT)/synthesis_pe_units.tcl)
SYN_NOC_TCL := $(abspath $(SYN_SCRIPT_ROOT)/synthesis_noc_units.tcl)
SYN_CLUSTER_TCL := $(abspath $(SYN_SCRIPT_ROOT)/synthesis_cluster_units.tcl)
DC_SETUP_FILE := $(abspath $(SCRIPT_ROOT)/synopsys_dc.setup)
SIM_REPORT_PY := $(abspath $(PY_SCRIPT_ROOT)/sim_report.py)
SYN_REPORT_PY := $(abspath $(PY_SCRIPT_ROOT)/syn_report.py)
SIGNOFF_REPORT_PY := $(abspath $(PY_SCRIPT_ROOT)/analyze_signoff_reports.py)
SUPERLINT_REPORT_QUERY_TCL := $(abspath $(SUPERLINT_QUERY_ROOT)/jasper_report_query.tcl)
SUPERLINT_HOTSPOT_QUERY_TCL := $(abspath $(SUPERLINT_QUERY_ROOT)/jasper_error_hotspot_query.tcl)
PRIMETIME_MAIN_TCL := $(abspath $(PRIMETIME_TCL_ROOT)/run_pt_hybridacc.tcl)
PRIMETIME_CONSTRAINT_TCL := $(abspath $(PRIMETIME_TCL_ROOT)/pt_constraints_hybridacc.tcl)
PRIMEPOWER_MAIN_TCL := $(abspath $(PRIMEPOWER_TCL_ROOT)/run_pwr_hybridacc.tcl)

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
ifdef GATE_TOP_LISNETLIST_DIR
GATE_TOP_NETLIST_DIR := $(GATE_TOP_LISNETLIST_DIR)
endif
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
RTL_FW_IMAGE_MODULE := hybridacc_tools.flat_fw_mem
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
SUPERLINT_REPORT_OUT ?= $(REPO_ROOT)/output/jasper_report_current.out
SUPERLINT_HOTSPOT_OUT ?= $(REPO_ROOT)/output/jasper_error_hotspot_current.out
PRESIM_FAST_TARGETS := sim_tb_corecontroller_smoke sim_tb_hybridacc_smoke sim_tb_hybridacc_sim sim_tb_processelement sim_tb_pe_sim sim_tb_networkonchip
SUPERLINT_FAST_TARGET_TAGS := INS_NR_INPR FLP_NR_FNIN OTP_NR_ASYA INS_IS_FEED NET_NO_LOAD PRT_NO_PRMS SEQ_NR_BLKA CAS_NR_DEFX
SUPERLINT_FAST_BATCH_DIR ?= $(REPO_ROOT)/output/superlint_strict_full_20260518/tag_batches_20260519
SUPERLINT_FAST_DUMP_TCL ?= $(SUPERLINT_FAST_BATCH_DIR)/tag_warning_dump.tcl
SUPERLINT_FAST_WAIVER_TCL ?= $(SUPERLINT_FAST_BATCH_DIR)/cumulative_07_INS_FLP_OTP_FEED_PRT_SEQ_CAS_waiver.tcl
SUPERLINT_FAST_REPORT_ROOT ?= $(REPO_ROOT)/output/superlint_fast_current
SUPERLINT_FAST_RUN_NAME ?= iter_fast_signoff
SUPERLINT_FAST_ITER_DIR ?= $(SUPERLINT_FAST_REPORT_ROOT)/$(SUPERLINT_FAST_RUN_NAME)
SUPERLINT_FAST_RUN_DIR ?= $(SUPERLINT_FAST_ITER_DIR)/warning_dump_run
SUPERLINT_FAST_LOG ?= $(SUPERLINT_FAST_ITER_DIR)/superlint.log
HACC_CC_EXAMPLE_ROOT := $(REPO_ROOT)/design/hybridacc-cc/example
RTL_REGRESS_CONV2D_1X1_SINGLE_WAVE_YAML := $(HACC_CC_EXAMPLE_ROOT)/conv1x1/conv2d_1x1_single_wave.yaml
RTL_REGRESS_CONV2D_3X3_SINGLE_WAVE_YAML := $(HACC_CC_EXAMPLE_ROOT)/conv3x3/conv2d_3x3_single_wave.yaml
RTL_REGRESS_CONV2D_3X3_FOUR_WAVE_YAML := $(HACC_CC_EXAMPLE_ROOT)/conv3x3/conv2d_3x3_four_wave.yaml
RTL_REGRESS_GEMM_SINGLE_WAVE_YAML := $(HACC_CC_EXAMPLE_ROOT)/gemm/gemm_single_wave.yaml
RTL_REGRESS_CONV2D_1X1_SINGLE_WAVE_OUT := $(RTL_FW_REGRESS_ROOT)/conv2d_1x1_single_wave
RTL_REGRESS_CONV2D_3X3_SINGLE_WAVE_OUT := $(RTL_FW_REGRESS_ROOT)/conv2d_3x3_single_wave
RTL_REGRESS_CONV2D_3X3_FOUR_WAVE_OUT := $(RTL_FW_REGRESS_ROOT)/conv2d_3x3_four_wave
RTL_REGRESS_GEMM_SINGLE_WAVE_OUT := $(RTL_FW_REGRESS_ROOT)/gemm_single_wave
RTL_REGRESS_CONV2D_1X1_SINGLE_WAVE_LOG_PREFIX := $(sim_log_dir)/tb_hybridacc_sim_conv2d_1x1_single_wave
RTL_REGRESS_CONV2D_3X3_SINGLE_WAVE_LOG_PREFIX := $(sim_log_dir)/tb_hybridacc_sim_conv2d_3x3_single_wave
RTL_REGRESS_CONV2D_3X3_FOUR_WAVE_LOG_PREFIX := $(sim_log_dir)/tb_hybridacc_sim_conv2d_3x3_four_wave
RTL_REGRESS_GEMM_SINGLE_WAVE_LOG_PREFIX := $(sim_log_dir)/tb_hybridacc_sim_gemm_single_wave
# M15: pure multi-K-wave gemm (M=48/N=32/K=192 -> wave_k=2) fidelity reference
RTL_REGRESS_GEMM_MULTI_WAVE_YAML := $(HACC_CC_EXAMPLE_ROOT)/gemm/gemm_m48_k192_n32.yaml
RTL_REGRESS_GEMM_MULTI_WAVE_OUT := $(RTL_FW_REGRESS_ROOT)/gemm_multi_wave
RTL_REGRESS_GEMM_MULTI_WAVE_LOG_PREFIX := $(sim_log_dir)/tb_hybridacc_sim_gemm_multi_wave

TB_ALL := $(shell find tb -type f -name 'tb_*.sv' ! -path 'tb/Cluster/tb_sram.sv')
TB_PE := $(shell find tb/PE -type f -name 'tb_*.sv' 2>/dev/null)
TB_NOC := $(shell find tb/NoC -type f -name 'tb_*.sv' 2>/dev/null)
TB_CLUSTER := $(shell find tb/Cluster -type f -name 'tb_*.sv' ! -name 'tb_sram.sv' 2>/dev/null)
SYN_TCL := $(shell find $(SYN_SCRIPT_ROOT) -maxdepth 1 -type f -name 'synthesis_*.tcl')

DC_SHELL ?= dc_shell
PT_SHELL ?= pt_shell
PWR_SHELL ?= pwr_shell
PRIMEPOWER_SHELL ?= $(PT_SHELL)
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
PRIMETIME_RUN_TAG ?= $(TOP_SYN_CLK_TAG)
PRIMETIME_CLOCK_PERIOD_NS ?= $(TOP_SYN_CLOCK_PERIOD_NS)
PRIMETIME_TOP_NAME ?= HybridAcc
PRIMETIME_BUILD_ROOT ?= $(bld_dir)/primetime
PRIMETIME_RUN_DIR ?= $(PRIMETIME_BUILD_ROOT)/$(PRIMETIME_RUN_TAG)
PRIMETIME_REPORT_DIR ?= $(report_dir)/primetime/$(PRIMETIME_RUN_TAG)
PRIMETIME_NETLIST ?= $(TOP_SYN_OUT_ROOT)/$(PRIMETIME_TOP_NAME)/$(PRIMETIME_TOP_NAME)_syn.v
PRIMETIME_SPEF_FILE ?=
PRIMETIME_GUI ?= 0
PRIMEPOWER_RUN_TAG ?= $(TOP_SYN_CLK_TAG)
PRIMEPOWER_CLOCK_PERIOD_NS ?= $(TOP_SYN_CLOCK_PERIOD_NS)
PRIMEPOWER_TOP_NAME ?= HybridAcc
PRIMEPOWER_ACTIVITY_TAG ?= tb_hybridacc_sim
PRIMEPOWER_BUILD_ROOT ?= $(bld_dir)/primepower/$(PRIMEPOWER_RUN_TAG)/$(PRIMEPOWER_ACTIVITY_TAG)
PRIMEPOWER_REPORT_DIR ?= $(report_dir)/primepower/$(PRIMEPOWER_RUN_TAG)/$(PRIMEPOWER_ACTIVITY_TAG)
PRIMEPOWER_ACTIVITY_DIR ?= $(PRIMEPOWER_BUILD_ROOT)/activity
PRIMEPOWER_NETLIST ?= $(TOP_SYN_OUT_ROOT)/$(PRIMEPOWER_TOP_NAME)/$(PRIMEPOWER_TOP_NAME)_syn.v
PRIMEPOWER_SPEF_FILE ?=
PRIMEPOWER_SCOPE ?= tb_hybridacc_sim/dut
PRIMEPOWER_FSDB ?= $(RTL_ROOT)/tb_hybridacc_sim.fsdb
PRIMEPOWER_VCD ?= $(PRIMEPOWER_ACTIVITY_DIR)/$(PRIMEPOWER_ACTIVITY_TAG).vcd
PRIMEPOWER_ACTIVITY_FILE ?= $(PRIMEPOWER_VCD)
PRIMEPOWER_ACTIVITY_FORMAT ?= vcd
PRIMEPOWER_HIERARCHY_LEVELS ?= 4
PRIMEPOWER_GUI ?= 0
FSDB2VCD ?= $(if $(wildcard $(VERDI_HOME)/bin/fsdb2vcd),$(VERDI_HOME)/bin/fsdb2vcd,fsdb2vcd)
FSDB2VCD_FLAGS ?=
SYN_DC_ENV := CLOCK_PERIOD_NS="$(CLOCK_PERIOD_NS)" RUN_TAG="$(RUN_TAG)" SYN_OUT_ROOT="$(SYN_OUT_ROOT)" REPORT_OUT_ROOT="$(REPORT_OUT_ROOT)" SYN_MAX_CORES="$(SYN_MAX_CORES)"
TOP_SYN_DC_ENV := CLOCK_PERIOD_NS="$(TOP_SYN_CLOCK_PERIOD_NS)" RUN_TAG="$(TOP_SYN_RUN_TAG)" SYN_OUT_ROOT="$(TOP_SYN_OUT_ROOT)" REPORT_OUT_ROOT="$(TOP_SYN_REPORT_OUT_ROOT)" SYN_MAX_CORES="$(SYN_MAX_CORES)" TOP_SYN_MAX_CORES="$(SYN_MAX_CORES)"
SNPSLMD_LICENSE_FILE ?= 26585@lstn
export SNPSLMD_LICENSE_FILE

PE_UNITS := DataMemory Decoder InstructionMemory LoopController PsumRegFile \
	TransformRegFile VADDU VMULU LDMA SDMA \
	IF_ID_Stage EXE_M_Stage EXE_A_Stage PErouter ProcessElement

NOC_UNITS := MBUS NoCRouter NetworkOnChip

CLUSTER_UNITS := AddressGenerateUnit ScratchpadMemory HybridDataDeliverUnit ComputeCluster

# Mapping: PE module name -> gate-level netlist path (relative to project root)
GATE_NETLIST_DIR := $(syn_dir)

.PHONY: help clean

help::
	@echo "Available targets:"
	@echo "  make <target> [VAR=VALUE ...]"
	@echo "Common:"
	@echo "  clean                - Remove simulation build artifacts"
	@echo "Variables: TEST_DATA_DIR=... VERIFY_TOL=... CLOCK_PERIOD_NS=... WAVE_DUMP=0|1 WAVE_DEPTH=... VERDI_HOME=... NOVAS_HOME=... RUN_TAG=... SYN_OUT_ROOT=... REPORT_OUT_ROOT=... SYN_BUILD_DIR=... SYN_LOG_NAME=... SYN_MAX_CORES=... SYN_MAX_MEMORY_GB=... GATE_TOP_NETLIST_DIR=... GATE_TOP_SDF_FILE=... GATE_SIM_RUN_FILTER=... RTL_FW_REGRESS_ROOT=... RTL_FW_DEBUG_PLUSARGS='+TRACE_CLUSTER_DEBUG' RTL_FW_TEST_SEED=... UV=... RISCV_TOOL_PREFIX=... JASPER=... JASPER_CSHRC=... SUPERLINT_REPORT_OUT=... SUPERLINT_HOTSPOT_OUT=... SUPERLINT_FAST_REPORT_ROOT=... SUPERLINT_FAST_RUN_NAME=... SUPERLINT_FAST_WAIVER_TCL=..."

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

clean:
	rm -rf csrc simv simv.daidir ucli.key DVEfiles *.vpd nWaveLog/ *.fsdb novas* *.lib++ sdfAnnotateInfo $(bld_dir) $(sim_dir)
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

.PHONY: sim_all sim_pe sim_noc sim_cluster presim_fast sim_noc_sim sim_noc_sim_all \
	rtl_regress_single_wave rtl_regress_conv2d_1x1_single_wave rtl_regress_conv2d_3x3_single_wave rtl_regress_conv2d_3x3_four_wave rtl_regress_conv2d_3x3_multi_wave rtl_regress_gemm_single_wave rtl_regress_gemm_multi_wave \
	compile_tb_hybridacc_sim sim_report postsim_all

help::
	@echo "Pre-sim:"
	@echo "  sim_<tb|module>      - Pre-sim: compile and run a testbench (RTL)"
	@echo "  sim_all              - Pre-sim: run all testbenches under tb/"
	@echo "  sim_pe               - Pre-sim: run all testbenches under tb/PE/"
	@echo "  sim_noc              - Pre-sim: run all testbenches under tb/NoC/"
	@echo "  sim_noc_sim          - Pre-sim: full NoC system sim with ESL test data"
	@echo "  sim_noc_sim_all      - Pre-sim: run all NoC test cases under output/noc-sim/"
	@echo "  sim_cluster          - Pre-sim: run all testbenches under tb/Cluster/"
	@echo "  presim_fast          - Pre-sim: run the 6-target signoff smoke bundle"
	@echo "  rtl_regress_single_wave             - Compile 3 hybridacc-cc single-wave YAMLs and run RTL/VCS firmware regression"
	@echo "  rtl_regress_conv2d_1x1_single_wave  - Run RTL/VCS firmware regression for conv2d_1x1_single_wave.yaml"
	@echo "  rtl_regress_conv2d_3x3_single_wave  - Run RTL/VCS firmware regression for conv2d_3x3_single_wave.yaml"
	@echo "  rtl_regress_conv2d_3x3_four_wave    - Run RTL/VCS firmware regression for conv2d_3x3_four_wave.yaml (4 temporal waves)"
	@echo "  rtl_regress_conv2d_3x3_multi_wave   - Alias of rtl_regress_conv2d_3x3_four_wave"
	@echo "  rtl_regress_gemm_single_wave        - Run RTL/VCS firmware regression for gemm_single_wave.yaml"
	@echo "  rtl_regress_gemm_multi_wave         - Run RTL/VCS firmware regression for gemm_m48_k192_n32.yaml (pure multi-K, wave_k=2)"
	@echo "  compile_tb_hybridacc_sim            - Pre-sim: compile tb_hybridacc_sim.simv (cached; skips if .sv sources unchanged)"
	@echo "  sim_report           - Generate a pre-sim report from sim/log/"

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

presim_fast: | $(sim_bld_dir) $(sim_log_dir)
	@set -e; \
	echo "[PRESIM_FAST] Running signoff pre-sim bundle ..."; \
	for target in $(PRESIM_FAST_TARGETS); do \
		$(MAKE) $$target TEST_DATA_DIR="$(TEST_DATA_DIR)" VERIFY_TOL="$(VERIFY_TOL)" CLOCK_PERIOD_NS="$(CLOCK_PERIOD_NS)" WAVE_DUMP="$(WAVE_DUMP)" WAVE_DEPTH="$(WAVE_DEPTH)" SIM_PLUSARGS="$(SIM_PLUSARGS)"; \
	done; \
	echo "[PRESIM_FAST] Completed: $(PRESIM_FAST_TARGETS)"

# --- Cached compile target for tb_hybridacc_sim ---
# Recompiles only when .sv sources change.
# NOTE: If VCS_FLAGS change (e.g. WAVE_DUMP=1/0 toggled between invocations),
#       remove $(sim_bld_dir)/tb_hybridacc_sim.simv manually to force recompile.
_tb_hybridacc_sim_sv_deps := tb/tb_hybridacc_sim.sv \
	$(shell find src -name '*.sv' 2>/dev/null)

$(sim_bld_dir)/tb_hybridacc_sim.simv: $(_tb_hybridacc_sim_sv_deps) | $(sim_bld_dir) $(sim_log_dir)
	@echo "[SIM] Compiling tb_hybridacc_sim.sv ..."
	$(VCS) $(VCS_FLAGS) +incdir+tb $(RTL_INCDIRS) $(RTL_SRAM_SRCS) tb/tb_hybridacc_sim.sv \
		-o $@ > $(sim_log_dir)/tb_hybridacc_sim.compile.log 2>&1 || \
		{ echo "[SIM] ERROR: VCS compile failed. See $(sim_log_dir)/tb_hybridacc_sim.compile.log"; exit 1; }

.PHONY: compile_tb_hybridacc_sim
compile_tb_hybridacc_sim: $(sim_bld_dir)/tb_hybridacc_sim.simv

define run_rtl_fw_regress
.PHONY: rtl_regress_$(1)
rtl_regress_$(1): | $(sim_bld_dir) $(sim_log_dir)
	@set -e; \
	echo "[RTL_REGRESS] [$(1)] Compiling $(2) -> $(3)"; \
	mkdir -p "$(3)"; \
	cd $(REPO_ROOT) && $(HACC_COMPILE) "$(2)" -o "$(3)" --dump-ir; \
	cd "$(CURDIR)"; \
	echo "[RTL_REGRESS] [$(1)] Generating $(3)/firmware.mem"; \
	fw_bytes=`cd $(REPO_ROOT) && $(UV) run python -m $(RTL_FW_IMAGE_MODULE) \
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
	$(MAKE) -C "$(CURDIR)" $(sim_bld_dir)/tb_hybridacc_sim.simv; \
	set +e; \
	$(sim_bld_dir)/tb_hybridacc_sim.simv \
		$(if $(WAVE_DUMP_ENABLED),+WAVE_DUMP +WAVE_DEPTH=$(WAVE_DEPTH),) \
		+FW_MEM=$(3)/firmware.mem +FW_BYTES=$$$$fw_bytes \
		+DRAM_MIRROR=$(3)/dram_init.bin +DRAM_MIRROR_BYTES=$$$$dram_bytes +DRAM_MIRROR_BASE=$$$$dram_base \
		+GOLDEN_OUTPUT=$(3)/golden_output.bin +GOLDEN_OUTPUT_BYTES=$$$$golden_bytes +GOLDEN_OUTPUT_BASE=$$$$golden_base \
		+ACTUAL_OUTPUT_DUMP=$$$$actual_output +SKIP_FW_TEST_SUMMARY +SKIP_GOLDEN_EXACT_CHECK \
		+MAX_LOADER_CYCLES=$(RTL_FW_MAX_LOADER_CYCLES) +MAX_CORE_CYCLES=$(RTL_FW_MAX_CORE_CYCLES) \
		$(RTL_FW_DEBUG_PLUSARGS) \
		> $(sim_log_dir)/tb_hybridacc_sim.run.log 2>&1; \
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
$(eval $(call run_rtl_fw_regress,conv2d_3x3_four_wave,$(RTL_REGRESS_CONV2D_3X3_FOUR_WAVE_YAML),$(RTL_REGRESS_CONV2D_3X3_FOUR_WAVE_OUT),$(RTL_REGRESS_CONV2D_3X3_FOUR_WAVE_LOG_PREFIX)))
$(eval $(call run_rtl_fw_regress,gemm_single_wave,$(RTL_REGRESS_GEMM_SINGLE_WAVE_YAML),$(RTL_REGRESS_GEMM_SINGLE_WAVE_OUT),$(RTL_REGRESS_GEMM_SINGLE_WAVE_LOG_PREFIX)))
$(eval $(call run_rtl_fw_regress,gemm_multi_wave,$(RTL_REGRESS_GEMM_MULTI_WAVE_YAML),$(RTL_REGRESS_GEMM_MULTI_WAVE_OUT),$(RTL_REGRESS_GEMM_MULTI_WAVE_LOG_PREFIX)))

rtl_regress_conv2d_3x3_multi_wave: rtl_regress_conv2d_3x3_four_wave

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

sim_report:
	@$(UV) run python "$(SIM_REPORT_PY)" --log-dir $(sim_log_dir) --mode pre-sim --output $(report_dir)/pre_sim_report_$(shell date +%Y_%m_%d_%H_%M_%S).md

postsim_%:
	$(summarize_tb)

postsim_all:
	@for log_file in $(sim_log_dir)/*.run.log; do \
		[ -f $$log_file ] || continue; \
		echo "[POSTSIM] $$log_file"; \
		grep -E 'PASS|FAIL|Verification Results|Outputs collected|Output timeout|tb_.* PASS' $$log_file || true; \
	done

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
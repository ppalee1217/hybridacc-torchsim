.PHONY: gate_sim_pe gate_sim_noc gate_regress_single_wave gate_regress_conv2d_1x1_single_wave gate_regress_conv2d_3x3_single_wave gate_regress_gemm_single_wave gate_sim_regress_single_wave gate_sim_regress_conv2d_1x1_single_wave gate_sim_regress_conv2d_3x3_single_wave gate_sim_regress_gemm_single_wave gate_sim_regressess_single_wave gate_sim_regressess_conv2d_1x1_single_wave gate_sim_regressess_conv2d_3x3_single_wave gate_sim_regressess_gemm_single_wave gate_sim_report

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
GATE_NOC_MAP_tb_noc_unit        := NoCRouter
GATE_NOC_MAP_tb_noc_sim         := NetworkOnChip

# All NoC testbenches that have gate-level counterparts
GATE_NOC_TBS := tb_mbus tb_nocrouter tb_noc_unit tb_noc_sim

help::
	@echo "Post-sim:"
	@echo "  gate_sim_<tb|module> - Post-sim: compile and run a testbench (gate-level + SDF)"
	@echo "  gate_sim_pe          - Post-sim: run all PE testbenches (gate-level + SDF)"
	@echo "  gate_sim_noc         - Post-sim: run all NoC testbenches (gate-level + SDF)"
	@echo "  gate_sim_regress_single_wave             - Compile 3 hybridacc-cc single-wave YAMLs and run gate-level/VCS firmware regression"
	@echo "  gate_sim_regress_conv2d_1x1_single_wave  - Run gate-level/VCS firmware regression for conv2d_1x1_single_wave.yaml"
	@echo "  gate_sim_regress_conv2d_3x3_single_wave  - Run gate-level/VCS firmware regression for conv2d_3x3_single_wave.yaml"
	@echo "  gate_sim_regress_gemm_single_wave        - Run gate-level/VCS firmware regression for gemm_single_wave.yaml"
	@echo "  gate_regress_<name>  - Gate-level top single-wave regression for conv2d_1x1 / conv2d_3x3 / gemm"
	@echo "  gate_sim_report      - Generate a post-sim report from sim/gate_log/"

define run_gate_tb
	@tb_file=$$(find tb -type f \( -name 'tb_$*.sv' -o -name '$*.sv' \) | head -n 1); \
	if [ -z "$$tb_file" ]; then \
		echo "No testbench found for '$*'"; \
		exit 1; \
	fi; \
	tb_base=$$(basename $$tb_file .sv); \
	tb_dir=$$(dirname $$tb_file); \
	mod_name=$${MOD_NAME:-$$(echo $(GATE_PE_MAP_$$(basename $$tb_file .sv))$(GATE_NOC_MAP_$$(basename $$tb_file .sv)))}; \
	sdf_file=$(GATE_NETLIST_DIR)/$$mod_name/$${mod_name}.sdf; \
	sdf_define=; \
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
	if [ -f "$$sdf_file" ]; then \
		sdf_define="+define+TB_SDF_FILE=\"$$sdf_file\""; \
	fi; \
	echo "[GATE_SIM] Compiling $$tb_file (gate: $$mod_name) ..."; \
	compile_status_file=$(gate_sim_log_dir)/$$tb_base.compile.status; \
	rm -f $$compile_status_file; \
	( $(VCS) $(GATE_VCS_FLAGS) +incdir+$$tb_dir $(RTL_INCDIRS) \
		$$sdf_define $(GATE_PKG_SRCS) $$netlist $$tb_file \
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
	fw_bytes=`cd $(REPO_ROOT) && $(UV) run python -m $(RTL_FW_IMAGE_MODULE) \
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
	$(MAKE) -C "$(CURDIR)" gate_sim_tb_hybridacc_sim MOD_NAME=HybridAcc GATE_NETLIST_DIR="$(GATE_TOP_NETLIST_DIR)" CLOCK_PERIOD_NS="$(CLOCK_PERIOD_NS)" \
		SIM_PLUSARGS="+FW_MEM=$(3)/firmware.mem +FW_BYTES=$$$$fw_bytes +DRAM_MIRROR=$(3)/dram_init.bin +DRAM_MIRROR_BYTES=$$$$dram_bytes +DRAM_MIRROR_BASE=$$$$dram_base +GOLDEN_OUTPUT=$(3)/golden_output.bin +GOLDEN_OUTPUT_BYTES=$$$$golden_bytes +GOLDEN_OUTPUT_BASE=$$$$golden_base +ACTUAL_OUTPUT_DUMP=$$$$actual_output +SKIP_FW_TEST_SUMMARY +SKIP_GOLDEN_EXACT_CHECK +MAX_LOADER_CYCLES=$(RTL_FW_MAX_LOADER_CYCLES) +MAX_CORE_CYCLES=$(RTL_FW_MAX_CORE_CYCLES) $(RTL_FW_DEBUG_PLUSARGS)"; \
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

gate_sim_regress_conv2d_1x1_single_wave: gate_regress_conv2d_1x1_single_wave

gate_sim_regress_conv2d_3x3_single_wave: gate_regress_conv2d_3x3_single_wave

gate_sim_regress_gemm_single_wave: gate_regress_gemm_single_wave

gate_sim_regressess_conv2d_1x1_single_wave: gate_sim_regress_conv2d_1x1_single_wave

gate_sim_regressess_conv2d_3x3_single_wave: gate_sim_regress_conv2d_3x3_single_wave

gate_sim_regressess_gemm_single_wave: gate_sim_regress_gemm_single_wave

gate_sim_regressess_single_wave: gate_sim_regress_single_wave

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
		tb_noc_unit:NoCRouter \
		tb_noc_sim:NetworkOnChip \
	; do \
		tb=$${pair%%:*}; mod=$${pair##*:}; \
		$(MAKE) gate_sim_$$tb MOD_NAME=$$mod; \
	done

gate_sim_report:
	@$(UV) run python "$(SIM_REPORT_PY)" --log-dir $(gate_sim_log_dir) --mode post-sim --output $(report_dir)/post_sim_report_$(shell date +%Y_%m_%d_%H_%M_%S).md
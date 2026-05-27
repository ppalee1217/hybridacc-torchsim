.PHONY: primepower_fsdb_to_vcd primepower_run primepower_gui primepower_analyze primepower_full clean_primepower

help::
	@echo "PrimePower:"
	@echo "  primepower_fsdb_to_vcd - Convert FSDB activity into the staged VCD under build/primepower"
	@echo "  primepower_run         - Run PrimePower into build/primepower and report/primepower"
	@echo "  primepower_gui         - Run PrimePower with GUI mode enabled"
	@echo "  primepower_analyze     - Parse PrimePower reports and emit HTML/PDF comparison against synthesis"
	@echo "  primepower_full        - Convert FSDB if needed, run PrimePower, then generate HTML/PDF analysis"
	@echo "  clean_primepower       - Remove PrimePower run and report directories for the selected case"

primepower_fsdb_to_vcd:
	@mkdir -p "$(PRIMEPOWER_ACTIVITY_DIR)"
	@if [ ! -f "$(PRIMEPOWER_FSDB)" ]; then \
		echo "[PRIMEPOWER] ERROR: FSDB not found: $(PRIMEPOWER_FSDB)"; \
		exit 1; \
	fi
	@echo "[PRIMEPOWER] Converting $(PRIMEPOWER_FSDB) -> $(PRIMEPOWER_VCD)"
	@"$(FSDB2VCD)" $(FSDB2VCD_FLAGS) "$(PRIMEPOWER_FSDB)" -o "$(PRIMEPOWER_VCD)"

primepower_run:
	@mkdir -p "$(PRIMEPOWER_BUILD_ROOT)" "$(PRIMEPOWER_REPORT_DIR)" "$(PRIMEPOWER_ACTIVITY_DIR)"
	@set -e; \
	activity_file="$(PRIMEPOWER_ACTIVITY_FILE)"; \
	if [ ! -f "$$activity_file" ]; then \
		if [ "$(PRIMEPOWER_ACTIVITY_FORMAT)" = "vcd" ] && [ "$$activity_file" = "$(PRIMEPOWER_VCD)" ]; then \
			if [ ! -f "$(PRIMEPOWER_FSDB)" ]; then \
				echo "[PRIMEPOWER] ERROR: FSDB not found: $(PRIMEPOWER_FSDB)"; \
				exit 1; \
			fi; \
			echo "[PRIMEPOWER] Converting $(PRIMEPOWER_FSDB) -> $(PRIMEPOWER_VCD)"; \
			"$(FSDB2VCD)" $(FSDB2VCD_FLAGS) "$(PRIMEPOWER_FSDB)" -o "$(PRIMEPOWER_VCD)"; \
		else \
			echo "[PRIMEPOWER] ERROR: Activity file not found: $$activity_file"; \
			exit 1; \
		fi; \
	fi; \
	gui_arg=""; \
	if [ "$(PRIMEPOWER_GUI)" = "1" ]; then gui_arg="-gui"; fi; \
	cd "$(PRIMEPOWER_BUILD_ROOT)" && \
	RUN_TAG="$(PRIMEPOWER_RUN_TAG)" \
	CLOCK_PERIOD_NS="$(PRIMEPOWER_CLOCK_PERIOD_NS)" \
	TOP_NAME="$(PRIMEPOWER_TOP_NAME)" \
	NETLIST_FILE="$(abspath $(PRIMEPOWER_NETLIST))" \
	REPORT_DIR="$(abspath $(PRIMEPOWER_REPORT_DIR))" \
	SPEF_FILE="$(PRIMEPOWER_SPEF_FILE)" \
	ACTIVITY_SCOPE="$(PRIMEPOWER_SCOPE)" \
	ACTIVITY_FILE="$(abspath $(PRIMEPOWER_ACTIVITY_FILE))" \
	ACTIVITY_FORMAT="$(PRIMEPOWER_ACTIVITY_FORMAT)" \
	HIERARCHY_LEVELS="$(PRIMEPOWER_HIERARCHY_LEVELS)" \
	GUI_MODE="$(PRIMEPOWER_GUI)" \
	$(PRIMEPOWER_SHELL) $$gui_arg -file "$(PRIMEPOWER_MAIN_TCL)" | tee "$(abspath $(PRIMEPOWER_REPORT_DIR))/pwr_shell.log"

primepower_gui: PRIMEPOWER_GUI=1
primepower_gui: primepower_run

primepower_analyze:
	@mkdir -p "$(PRIMEPOWER_REPORT_DIR)/analysis"
	@$(UV) run python "$(SIGNOFF_REPORT_PY)" \
		--mode primepower \
		--report-dir "$(PRIMEPOWER_REPORT_DIR)" \
		--synthesis-report-dir "$(report_dir)/$(PRIMEPOWER_RUN_TAG)/HybridAcc" \
		--output-dir "$(PRIMEPOWER_REPORT_DIR)/analysis"

primepower_full: primepower_run primepower_analyze

clean_primepower:
	rm -rf "$(PRIMEPOWER_BUILD_ROOT)" "$(PRIMEPOWER_REPORT_DIR)"
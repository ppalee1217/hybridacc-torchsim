.PHONY: primetime_run primetime_gui primetime_analyze primetime_full clean_primetime

help::
	@echo "PrimeTime:"
	@echo "  primetime_run        - Run standalone PrimeTime STA into build/primetime and report/primetime"
	@echo "  primetime_gui        - Run PrimeTime with GUI mode enabled"
	@echo "  primetime_analyze    - Parse PrimeTime reports and emit HTML/PDF comparison against synthesis"
	@echo "  primetime_full       - Run PrimeTime then generate HTML/PDF analysis"
	@echo "  clean_primetime      - Remove PrimeTime run and report directories for the selected run tag"

primetime_run:
	@mkdir -p "$(PRIMETIME_RUN_DIR)" "$(PRIMETIME_REPORT_DIR)"
	@set -e; \
	gui_arg=""; \
	if [ "$(PRIMETIME_GUI)" = "1" ]; then gui_arg="-gui"; fi; \
	cd "$(PRIMETIME_RUN_DIR)" && \
	RUN_TAG="$(PRIMETIME_RUN_TAG)" \
	CLOCK_PERIOD_NS="$(PRIMETIME_CLOCK_PERIOD_NS)" \
	TOP_NAME="$(PRIMETIME_TOP_NAME)" \
	NETLIST_FILE="$(abspath $(PRIMETIME_NETLIST))" \
	REPORT_DIR="$(abspath $(PRIMETIME_REPORT_DIR))" \
	SPEF_FILE="$(PRIMETIME_SPEF_FILE)" \
	GUI_MODE="$(PRIMETIME_GUI)" \
	$(PT_SHELL) $$gui_arg -file "$(PRIMETIME_MAIN_TCL)" | tee "$(abspath $(PRIMETIME_REPORT_DIR))/pt_shell.log"

primetime_gui: PRIMETIME_GUI=1
primetime_gui: primetime_run

primetime_analyze:
	@mkdir -p "$(PRIMETIME_REPORT_DIR)/analysis"
	@$(UV) run python "$(SIGNOFF_REPORT_PY)" \
		--mode primetime \
		--report-dir "$(PRIMETIME_REPORT_DIR)" \
		--synthesis-report-dir "$(report_dir)/$(PRIMETIME_RUN_TAG)/HybridAcc" \
		--output-dir "$(PRIMETIME_REPORT_DIR)/analysis"

primetime_full: primetime_run primetime_analyze

clean_primetime:
	rm -rf "$(PRIMETIME_RUN_DIR)" "$(PRIMETIME_REPORT_DIR)"
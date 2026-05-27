.PHONY: syn_top synall syn_pe_all syn_noc_all syn_cluster_all syn_report clean_syn

help::
	@echo "Synthesis:"
	@echo "  synthesize_<name>    - Run a synthesis_<name>.tcl flow"
	@echo "  syn_top              - Synthesize top-level HybridAcc"
	@echo "  synall               - Run all synthesis_*.tcl scripts under script/"
	@echo "  syn_pe_<module>      - Synthesize a PE unit, e.g. syn_pe_VADDU"
	@echo "  syn_pe_all           - Synthesize all PE units"
	@echo "  syn_noc_<module>     - Synthesize a NoC unit, e.g. syn_noc_MBUS"
	@echo "  syn_noc_all          - Synthesize all NoC units"
	@echo "  syn_cluster_<module> - Synthesize a Cluster unit"
	@echo "  syn_cluster_all      - Synthesize all Cluster units"
	@echo "  syn_report           - Generate a synthesis report"
	@echo "  clean_syn            - Remove synthesized files and reports"

syn_report:
	@$(UV) run python "$(SYN_REPORT_PY)" --report-dir $(report_dir) --output $(report_dir)/pe_synthesis_report_$(shell date +%Y_%m_%d_%H_%M_%S).md

synthesize_%: | $(bld_dir) $(syn_dir) $(report_dir)
	@echo "Synthesizing $*..."
	@test -f $(SYN_SCRIPT_ROOT)/synthesis_$*.tcl || (echo "Missing $(SYN_SCRIPT_ROOT)/synthesis_$*.tcl" && exit 1)
	mkdir -p $(syn_dir)/$*
	mkdir -p $(report_dir)/$*
	cp $(DC_SETUP_FILE) $(bld_dir)/.synopsys_dc.setup
	cd $(bld_dir); \
	echo "[SYN] Resource cap: $(SYN_RESOURCE_LIMIT_DESC)"; \
	status_file="$$(pwd)/.dc_status"; \
	rm -f "$$status_file"; \
	( bash -lc '$(SYN_BASH_RESOURCE_LIMIT); $(SYN_DC_ENV) $(DC_SHELL) -no_home_init -f "$(SYN_SCRIPT_ROOT)/synthesis_$*.tcl"'; \
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

syn_pe_%: | $(bld_dir) $(syn_dir) $(report_dir)
	@echo "[SYN] Synthesizing PE unit: $* ..."
	mkdir -p $(syn_dir)/$*
	mkdir -p $(report_dir)/$*
	cp $(DC_SETUP_FILE) $(bld_dir)/.synopsys_dc.setup
	cd $(bld_dir); \
	echo "[SYN] Resource cap: $(SYN_RESOURCE_LIMIT_DESC)"; \
	status_file="$$(pwd)/.dc_status"; \
	rm -f "$$status_file"; \
	( bash -lc '$(SYN_BASH_RESOURCE_LIMIT); $(SYN_DC_ENV) $(DC_SHELL) -no_home_init -x "set MOD_NAME $*" -f "$(SYN_PE_TCL)"'; \
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

syn_noc_%: | $(bld_dir) $(syn_dir) $(report_dir)
	@echo "[SYN] Synthesizing NoC unit: $* ..."
	mkdir -p $(syn_dir)/$*
	mkdir -p $(report_dir)/$*
	cp $(DC_SETUP_FILE) $(bld_dir)/.synopsys_dc.setup
	cd $(bld_dir); \
	echo "[SYN] Resource cap: $(SYN_RESOURCE_LIMIT_DESC)"; \
	status_file="$$(pwd)/.dc_status"; \
	rm -f "$$status_file"; \
	( bash -lc '$(SYN_BASH_RESOURCE_LIMIT); $(SYN_DC_ENV) $(DC_SHELL) -no_home_init -x "set MOD_NAME $*" -f "$(SYN_NOC_TCL)"'; \
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

syn_cluster_%: | $(bld_dir) $(syn_dir) $(report_dir)
	@echo "[SYN] Synthesizing Cluster unit: $* ..."
	mkdir -p $(syn_dir)/$*
	mkdir -p $(report_dir)/$*
	cp $(DC_SETUP_FILE) $(bld_dir)/.synopsys_dc.setup
	cd $(bld_dir); \
	echo "[SYN] Resource cap: $(SYN_RESOURCE_LIMIT_DESC)"; \
	status_file="$$(pwd)/.dc_status"; \
	rm -f "$$status_file"; \
	( bash -lc '$(SYN_BASH_RESOURCE_LIMIT); $(SYN_DC_ENV) $(DC_SHELL) -no_home_init -x "set MOD_NAME $*" -f "$(SYN_CLUSTER_TCL)"'; \
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

clean_syn:
	rm -rf $(syn_dir) $(report_dir)
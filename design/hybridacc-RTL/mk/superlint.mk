.PHONY: superlint superlint_report superlint_hotspot superlint_fast clean_superlint

help::
	@echo "Superlint:"
	@echo "  superlint_report     - Run Jasper Superlint whole-design report query"
	@echo "  superlint_hotspot    - Run Jasper Superlint error-hotspot query"
	@echo "  superlint            - Run both Jasper Superlint report and hotspot queries"
	@echo "  superlint_fast       - Run final strict tag-dump Superlint with cumulative signoff waiver"
	@echo "  clean_superlint      - Remove output Jasper/Superlint logs, run dirs, and Tcl probes"

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

clean_superlint:
	@echo "[SUPERLINT] Removing output Jasper/Superlint artifacts ..."
	@find "$(REPO_ROOT)/output" -mindepth 1 -maxdepth 1 \
		\( -name 'jasper*' -o \( -name '*superlint*' ! -name 'superlint-report.md' \) \) \
		-exec rm -rf {} +
	@rm -rf "$(REPO_ROOT)/jgproject"
	@echo "[SUPERLINT] Output cleanup complete"

# Detailed strict batch flow remains to be populated in the follow-up flow patch.
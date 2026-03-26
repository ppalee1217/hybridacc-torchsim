MAKEFILE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
PROJECT_ROOT ?= $(abspath $(MAKEFILE_DIR)/../../..)
DEFAULT_ASM := $(PROJECT_ROOT)/design/hybridacc-pe-isa/tools/bin/ha-asm
ASM ?= $(shell if [ -x "$(DEFAULT_ASM)" ]; then printf '%s' "$(DEFAULT_ASM)"; elif command -v ha-asm >/dev/null 2>&1; then command -v ha-asm; else printf '%s' "$(DEFAULT_ASM)"; fi)
TEMPLATE_DIR ?= template
BIN_OUT_DIR ?= bin
JSON_OUT_DIR ?= json

ASM_SRCS := $(wildcard $(TEMPLATE_DIR)/*.asm)
ASM_NAMES := $(patsubst $(TEMPLATE_DIR)/%.asm,%,$(ASM_SRCS))
BIN_OUTS := $(addprefix $(BIN_OUT_DIR)/,$(addsuffix .bin,$(ASM_NAMES)))
JSON_OUTS := $(addprefix $(JSON_OUT_DIR)/,$(addsuffix .json,$(ASM_NAMES)))

.PHONY: all bins jsons clean list

all: bins jsons

bins: $(BIN_OUTS)

jsons: $(JSON_OUTS) $(BIN_OUTS)

list:
	@printf '%s\n' $(ASM_SRCS)

$(BIN_OUT_DIR) $(JSON_OUT_DIR):
	@mkdir -p $@

$(BIN_OUT_DIR)/%.bin $(JSON_OUT_DIR)/%.json: $(TEMPLATE_DIR)/%.asm | $(BIN_OUT_DIR) $(JSON_OUT_DIR)
	@if [ ! -x "$(ASM)" ]; then \
		echo "Error: assembler not found at '$(ASM)' and 'ha-asm' is not available in PATH."; \
		exit 1; \
	fi
	@$(ASM) $< -o $(BIN_OUT_DIR)/$*.bin --json $(JSON_OUT_DIR)/$*.json
	@echo "Assembled $< -> $(BIN_OUT_DIR)/$*.bin and $(JSON_OUT_DIR)/$*.json"

clean:
	@rm -f $(BIN_OUTS) $(JSON_OUTS)
	@echo "Cleaned generated .bin and .json files"
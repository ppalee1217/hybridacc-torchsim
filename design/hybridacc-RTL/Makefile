RTL_ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

.DEFAULT_GOAL := help

include $(RTL_ROOT)/mk/common.mk
include $(RTL_ROOT)/mk/pre-sim.mk
include $(RTL_ROOT)/mk/post-sim.mk
include $(RTL_ROOT)/mk/synthesis.mk
include $(RTL_ROOT)/mk/superlint.mk
include $(RTL_ROOT)/mk/primetime.mk
include $(RTL_ROOT)/mk/primepower.mk

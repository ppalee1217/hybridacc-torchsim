# Master Makefile — builds all firmware tests

TESTS := test_alu test_branch test_jump test_loadstore test_mul \
         test_csr test_stack test_trap test_plic test_fabric \
		 test_hazard test_compound test_cluster_ctrl test_dma test_wfi_timer

.PHONY: all clean $(TESTS)

all: $(TESTS)

$(TESTS):
	$(MAKE) -C $@

clean:
	@for d in $(TESTS); do $(MAKE) -C $$d clean; done

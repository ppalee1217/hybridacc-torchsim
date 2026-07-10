# ISA v3 GEMM PE program.
#
# Native multi-K contract:
# - one PE START executes exactly one firmware wave and reaches HALT;
# - firmware patches all three wave-local loop counts to one for multi-K;
# - CLEAR.P clears only the wave-local VP accumulation;
# - VPSUMR emits PLO = PLI + VP, so firmware can carry the completed partial
#   sum through alternating PLI/PLO SPM groups before the next PE START.

.template
gemm_template(KERNEL_DMA_STORE_LEN=64, KERNEL_DMA_LOAD_LEN=256, INPUT_DIM=32, OUTPUT_DIM_MINUS_ONE=7, PSUM_COUNT=24, NUM_OF_KERNEL_PREFETCH_SETS=1, NUM_OF_KERNEL_LOAD_LOOP=1, NUM_OF_KERNEL_REUSE_LOOP=1, K_TILE_DIM=32):
    SYS.CTRL (CLEAR.P)

setup:
    SDMA.LOOP $(NUM_OF_KERNEL_PREFETCH_SETS)
    SDMA.ADDR 0
    SDMA.LEN $(KERNEL_DMA_STORE_LEN)
    SDMA.SD 4

    LDMA.ADDR 0
    LDMA.LEN $(KERNEL_DMA_LOAD_LEN)
    LDMA.LHB 1

    SYS.CTRL (SDMA.ACT)

load_ab_tile:
    LOOPIN $(NUM_OF_KERNEL_LOAD_LOOP)
    SYS.SYNC (SWAPDM)

    LOOPIN $(NUM_OF_KERNEL_REUSE_LOOP)
    SYS.CTRL (CLEAR.P, RST.PID, RST.TID, LDMA.ACT)

loop_k_dim:
    LOOPIN $(K_TILE_DIM)

load_a_b:
    VTSTORE vt0
    VTSTORE vt1
    VTSTORE vt2

compute_outer:
    LOOPIN $(OUTPUT_DIM_MINUS_ONE)
    VMULR 1, 1
    VMULR 1, 1
    VMULRN 1, VTRST
    LOOPEND

    VMULR 1, 1
    VMULR 1, 1
    VMULRN PRST, VTRST
    LOOPEND

accumulate_c:
    LOOPIN $(PSUM_COUNT)
    VPSUMR 1
    LOOPEND

    LOOPEND
    LOOPEND

    HALT

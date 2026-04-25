## 1D Convolution with kernel size 7 template assembly code

##############################################################################
# SYMBOL TABLE - Template Parameters
##############################################################################
# This template requires the following symbols to be defined at compile time:
#
# Symbol Name           | Type    | Description                          | Example Value
# ----------------------|---------|--------------------------------------|---------------
# KERNEL_DMA_STORE_LEN  | int     | Kernel DMA transfer length           | 64
#                       |         | (num_kernels * vectors_per_kernel)   | (4out * 2vector * 32dim)
#
# KERNEL_DMA_LOAD_LEN   | int     | Kernel DMA transfer length           | 256
#
# INPUT_DIM             | int     | Input dimension size                 | 32
#
# OUTPUT_DIM_MINUS_ONE  | int     | Output dimension size minus one      | 7
#
# PSUM_COUNT            | int     | Number of partial sums to compute    | 24
#
# NUM_OF_KERNEL_PREFETCH_SETS | int | Number of kernel payload prefetch sets | 2
#                            |     | (typically K-tile sets per load loop) |
#
# NUM_OF_KERNEL_LOAD_LOOP | int   | Number of kernel payload load loops    | 2
#
# NUM_OF_KERNEL_REUSE_LOOP | int  | Number of compute reuse loops per load | 2

##############################################################################
# Usage Example:
#   Assembler should replace $(SYMBOL_NAME) with actual values before assembly
#   e.g., $(KERNEL_DMA_LEN) -> 64
##############################################################################

.template
gemm_template(KERNEL_DMA_STORE_LEN=64, KERNEL_DMA_LOAD_LEN=256, INPUT_DIM=32, OUTPUT_DIM_MINUS_ONE=7, PSUM_COUNT=24, NUM_OF_KERNEL_PREFETCH_SETS=32, NUM_OF_KERNEL_LOAD_LOOP=2, NUM_OF_KERNEL_REUSE_LOOP=2, K_TILE_DIM=32):
    # Initialize vector registers for partial sums
    SYS.CTRL (CLEAR.P)

setup:
    # 1) C tile store prefetch
    SDMA.LOOP $(NUM_OF_KERNEL_PREFETCH_SETS)  # loop for $(NUM_OF_KERNEL_PREFETCH_SETS) kernel payload sets
    SDMA.ADDR 0
    SDMA.LEN $(KERNEL_DMA_STORE_LEN)  # STORE C-tile (example: 4out * 2vector * 32dim)
    SDMA.SD 4 # start DMA store operation

    # 2) A/B tile load prefetch
    LDMA.ADDR 0
    LDMA.LEN $(KERNEL_DMA_LOAD_LEN) # LOAD A/B tile data (example: 32dim * 8input)
    LDMA.LHB 1 # start DMA load operation (fp16, broadcasted)

    SYS.CTRL (SDMA.ACT)
load_ab_tile:
    # Kernel payload load loop: each iteration consumes one SWAPDM-ready payload
    LOOPIN $(NUM_OF_KERNEL_LOAD_LOOP)  # kernel payload load loops (example: 2)
    SYS.SYNC (SWAPDM) # Wait for A/B tile to be ready

    LOOPIN $(NUM_OF_KERNEL_REUSE_LOOP)  # compute reuse loops per payload (example: 2)
    SYS.CTRL (CLEAR.P, RST.PID, RST.TID, LDMA.ACT) # Start loading next A/B slice

    loop_k_dim:
        LOOPIN $(K_TILE_DIM)  # K dimension within tile

        load_a_b:
            VTSTORE vt0
            VTSTORE vt1
            VTSTORE vt2

        compute_outer:
            LOOPIN $(OUTPUT_DIM_MINUS_ONE)  # PE-local output columns within tile
                    VMULR 1, 1
                VMULR 1, 1
                VMULRN 1, VTRST # reset vector register id
            LOOPEND # compute_outer
            VMULR 1, 1
            VMULR 1, 1
            VMULRN PRST, VTRST # reset vector register id
        LOOPEND # loop_k_dim

    accumulate_c:
        # Partial sum for C tile
        LOOPIN $(PSUM_COUNT) # number of partial sums in tile
            VPSUMR 1
        LOOPEND # accumulate_c

    LOOPEND  # End M-tiles loop
    LOOPEND

    HALT
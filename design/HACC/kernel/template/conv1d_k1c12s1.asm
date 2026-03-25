## 1D Convolution with kernel size 1 template assembly code

##############################################################################
# SYMBOL TABLE - Template Parameters
##############################################################################
# This template requires the following symbols to be defined at compile time:
#
# Symbol Name                   | Type    | Description                          | Example Value
# ------------------------------|---------|--------------------------------------|---------------
# KERNEL_DMA_LEN                | int     | Kernel DMA transfer length           | 48
#                               |         | (num_kernels * vectors_per_kernel)   | (16 * 3)
#
# OUTPUT_WINDOW_CNT_MINUS_ONE   | int     | Number of sliding windows minus one  | 799
#                               |         | Total input elements to process      |
#
# KERNEL_COUNT                  | int     | Number of convolution kernels        | 16
#                               |         |                                      |
# KERNEL_LOOP            | int     | Number of kernel sets to process     | 2
#                               |         | (for double buffering)               |
##############################################################################
# Usage Example:
#   Assembler should replace $(SYMBOL_NAME) with actual values before assembly
#   e.g., $(KERNEL_DMA_LEN) -> 48
##############################################################################
.template
conv1d_k1c12s1_template(KERNEL_DMA_LEN=48, OUTPUT_WINDOW_CNT_MINUS_ONE=799, KERNEL_COUNT=16, KERNEL_LOOP_INNER=2, KERNEL_LOOP_OUTER=1):
    # Initialize vector registers for partial sums
    SYS.CTRL (CLEAR.P)
extra_loop:
    LOOPIN $(KERNEL_LOOP_OUTER)  # Loop for $(KERNEL_LOOP_OUT

setup:
    SDMA.ADDR 0
    SDMA.LEN $(KERNEL_DMA_LEN)  # STORE $(KERNEL_DMA_LEN) steps of kernel data ($(KERNEL_COUNT) kernels * 3 vector each)
    SDMA.LOOP $(KERNEL_LOOP_INNER)  # loop for $(KERNEL_LOOP_INNER) kernel sets
    SDMA.SD 4  # start DMA store operation

    LDMA.ADDR 0
    LDMA.LEN $(KERNEL_DMA_LEN)
    LDMA.LD 4  # LOAD $(KERNEL_DMA_LEN) steps of input data ($(KERNEL_COUNT) kernels * 4 elements each)

    SYS.CTRL (SDMA.ACT)

compute_loop:
    LOOPIN $(KERNEL_LOOP_INNER)  # Loop for $(KERNEL_LOOP_INNER) kernel sets
    SYS.SYNC (SWAPDM)

    VTSTORE vt0
    VTSTORE vt1
    VTSTORE vt2

loop_window:
    LOOPIN $(OUTPUT_WINDOW_CNT_MINUS_ONE)  # Loop for $(OUTPUT_WINDOW_CNT_MINUS_ONE) input elements
    SYS.CTRL (RST.PID, RST.TID, LDMA.ACT, CLEAR.P)
    LOOPIN $(KERNEL_COUNT)  # Loop for $(KERNEL_COUNT) kernels
    VMACRN 0, 1
    VMACRN 0, 1
    VMACRN 1, VTRST # reset vector register id
    LOOPEND
    VPSUM_VTSTORE vp0, vt0
    VPSUM_VTSTORE vp1, vt1
    VPSUM_VTSTORE vp2, vt2
    VPSUM vp3
    LOOPEND
last:
    SYS.CTRL (RST.PID, RST.TID, LDMA.ACT, CLEAR.P)
    LOOPIN $(KERNEL_COUNT)  # Loop for $(KERNEL_COUNT) kernels
    VMACRN 0, 1
    VMACRN 0, 1
    VMACRN 1, VTRST # reset vector register id
    LOOPEND
    VPSUM vp0
    VPSUM vp1
    VPSUM vp2
    VPSUM vp3

    LOOPEND  # End global loop

    LOOPEND # end of outer kernel set loop

    HALT

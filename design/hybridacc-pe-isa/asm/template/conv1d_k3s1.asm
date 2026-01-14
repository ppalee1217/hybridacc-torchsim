## 1D Convolution with kernel size 3 template assembly code

##############################################################################
# SYMBOL TABLE - Template Parameters
##############################################################################
# This template requires the following symbols to be defined at compile time:
#
# Symbol Name           | Type    | Description                          | Example Value
# ----------------------|---------|--------------------------------------|---------------
# KERNEL_DMA_LEN        | int     | Kernel DMA transfer length           | 48
#                       |         | (num_kernels * vectors_per_kernel)   | (16 * 3)
#
# OUTPUT_WINDOW_CNT     | int     | Number of sliding windows            | 798
#                       |         | Total input elements to process      |
#
# KERNEL_COUNT          | int     | Number of convolution kernels        | 16
#                       |         |                                      |
##############################################################################
# Usage Example:
#   Assembler should replace $(SYMBOL_NAME) with actual values before assembly
#   e.g., $(KERNEL_DMA_LEN) -> 48
##############################################################################

.template
conv1d_k3s1_template(KERNEL_DMA_LEN=48, OUTPUT_WINDOW_CNT=798, KERNEL_COUNT=16):
    # Initialize vector registers for partial sums
    CLEAR.P

load_kernel:
    SDMA.ADDR 0
    SDMA.LEN $(KERNEL_DMA_LEN)  # Load kernel weights: 16 kernels × 3 vectors
    SDMA.LOOP 1  # loop for 1 kernel set
    SDMA.SD 4  # start DMA store operation

loop_tile:
    LOOPIN 1  # Tile loop for 1 tile
    SWAPDM

preload_input:
    TSTORE t0
    TSTORE t3
    TSTORE t6
    TSTORE t9
    TSTORE t1
    TSTORE t4
    TSTORE t7
    TSTORE t10

loop_window:
    LOOPIN $(OUTPUT_WINDOW_CNT)  # Sliding window loop: process N input windows

load_input:
    TSTORE t2
    TSTORE t5
    TSTORE t8
    TSTORE t11

    LDMA.ADDR 0
    LDMA.LEN $(KERNEL_DMA_LEN)  # Load input data for current window
    LDMA.LD 4  # LOAD 48 steps of input data (3 vector * 4 elements each)
    SETRID.PT 0, 0

loop_kernel:
    LOOPIN $(KERNEL_COUNT)  # Process each kernel (compute convolution)
    VMACRN 0, 1
    VMACRN 0, 1
    VMACRN 1, VTRST # reset vector register id
    LOOPEND

calculate_psum:
    VPSUM vp0
    VPSUM vp1
    VPSUM vp2
    VPSUM vp3
    CLEAR.P # Clear the partial sum register

    TSHIFT K3
    LOOPEND

    LOOPEND
    HALT  # End of program

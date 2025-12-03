## 1D Convolution with kernel size 5 template assembly code

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
# OUTPUT_WINDOW_CNT     | int     | Number of sliding windows            | 796
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
conv1d_k5s1_template(KERNEL_DMA_LEN=48, OUTPUT_WINDOW_CNT=796, KERNEL_COUNT=16):
    # Initialize vector registers for partial sums
    CLEAR.P

load_kernel:
    DMA.ADDR 0
    DMA.LEN $(KERNEL_DMA_LEN)  # STORE $(KERNEL_DMA_LEN) steps of kernel data (16 kernels * 3 vector each)
    DMA.SD 4  # start DMA store operation

preload_input:
    TSTORE t0
    TSTORE t6
    TSTORE t1
    TSTORE t7
    TSTORE t2
    TSTORE t8
    TSTORE t3
    TSTORE t9

loop_window:
    LOOPIN 796  # Loop for 800 input elements

load_input:
    TSTORE t4
    TSTORE t10

    DMA.ADDR 0
    DMA.LEN $(KERNEL_DMA_LEN)
    DMA.LD 4  # LOAD $(KERNEL_DMA_LEN) steps of input data (3 vector * 4 elements each)
    SETRID.PT 0, 0

loop_kernel:
    LOOPIN $(KERNEL_COUNT)  # Loop for $(KERNEL_COUNT) kernels
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

    TSHIFT K5
    LOOPEND

    HALT  # End of program

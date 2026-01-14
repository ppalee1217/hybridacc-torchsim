## 1D Convolution with kernel size 7 template assembly code

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
# OUTPUT_WINDOW_CNT     | int     | Number of sliding windows            | 397
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
conv1d_k7s1_template(KERNEL_DMA_LEN=48, OUTPUT_WINDOW_CNT=397, KERNEL_COUNT=16):
    # Initialize vector registers for partial sums
    CLEAR.P

load_kernel:
    SDMA.ADDR 0
    SDMA.LEN $(KERNEL_DMA_LEN)  # STORE $(KERNEL_DMA_LEN) steps of kernel data (16 kernels * 3 vector each)
    SDMA.LOOP 1  # loop for 1 kernel set
    SDMA.SD 4  # start DMA store operation

loop_tile:
    LOOPIN 1  # Tile loop for 1 tile
    SWAPDM

preload_input:
    TSTORE t0
    TSTORE t1
    TSTORE t2
    TSTORE t3
    TSTORE t4
    TSTORE t5

loop_window:
    LOOPIN $(OUTPUT_WINDOW_CNT)  # Loop for 800 input elements

load_input:
    TSTORE t6

    LDMA.ADDR 0
    LDMA.LEN $(KERNEL_DMA_LEN)  # Use the defined symbol for kernel DMA length
    LDMA.LD 4  # LOAD $(KERNEL_DMA_LEN) steps of input data (3 vector * 4 elements each)
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

    TSHIFT K7
    TSTORE t6
    TSHIFT K7
    LOOPEND

    LOOPEND
    HALT  # End of program

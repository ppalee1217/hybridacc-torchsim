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
# OUTPUT_DIM            | int     | Output dimension size                | 8
#
# PSUM_COUNT            | int     | Number of partial sums to compute    | 24

##############################################################################
# Usage Example:
#   Assembler should replace $(SYMBOL_NAME) with actual values before assembly
#   e.g., $(KERNEL_DMA_LEN) -> 64
##############################################################################

.template
gemv_template(KERNEL_DMA_STORE_LEN=64, KERNEL_DMA_LOAD_LEN=256, INPUT_DIM=32, OUTPUT_DIM=8, PSUM_COUNT=24):
    # Initialize vector registers for partial sums
    CLEAR.P

load_kernel:
    SDMA.ADDR 0
    SDMA.LEN $(KERNEL_DMA_STORE_LEN)  # STORE $(KERNEL_DMA_LEN) steps of kernel data (4out * 2vector * 32dim)
    SDMA.LOOP 1  # loop for 1 kernel set
    SDMA.SD 4 # start DMA store operation

loop_tile:
    LOOPIN 1  # Tile loop for 1 tile
    SWAPDM

    LDMA.ADDR 0
    LDMA.LEN $(KERNEL_DMA_LOAD_LEN) # LOAD $(KERNEL_DMA_LOAD_LEN) steps of input data (INPUT_DIM * 8input)
    LDMA.LHB 1 # start DMA load operation (fp16, broadcasted to 4out)

loop_in_dim:
    LOOPIN $(INPUT_DIM)  # Loop for $(INPUT_DIM) input dimensions

load_input:
    TSTORE t0
    TSTORE t3
    TSTORE t6
    TSTORE t9
    TSTORE t1
    TSTORE t4
    TSTORE t7
    TSTORE t10
    TSTORE t2
    TSTORE t5
    TSTORE t8
    TSTORE t11

    SETRID.PT 0, 0
compute_gemv:
    LOOPIN $(OUTPUT_DIM) # Loop for $(OUTPUT_DIM) output dimensions
    VMULR 1, 1
    VMULR 1, 1
    VMULRN 1, VTRST # reset vector register id
    LOOPEND # compute_gemv

    SETRID.P 0

    LOOPEND # loop_in_dim

psum:
    # Calculate the partial sum for each output dimension
    LOOPIN $(PSUM_COUNT) # Loop for $(PSUM_COUNT) partial sums
    VPSUMR 1
    LOOPEND # psum
    CLEAR.P # Clear the partial sum register

    LOOPEND
    HALT  # End of program

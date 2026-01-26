gemv:

    SDMA.LOOP 4
    SDMA.ADDR 0
    SDMA.LEN 64  # STORE 64 steps of kernel data (4out * 2vector * 32dim)
    SDMA.SD 4 # start DMA store operation

    LOOPIN 4  # Global loop for 4x data volume

load_kernel:
    SWAPDM

    LDMA.ADDR 0
    LDMA.LEN 256 # LOAD 256 steps of input data (32dim * 8input)
    LDMA.LHB 1 # start DMA load operation (fp16, broadcasted to 4out)

loop_in_dim:
    LOOPIN 32  # Loop for 32 input dimensions

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
    LOOPIN 8 # Loop for 8 output dimensions
    VMULR 1, 1
    VMULR 1, 1
    VMULRN 1, VTRST # reset vector register id
    LOOPEND # compute_gemv

    SETRID.P 0

    LOOPEND # loop_in_dim

psum:
    # Calculate the partial sum for each output dimension
    LOOPIN 24 # Loop for 24 partial sums
    VPSUMR 1
    LOOPEND # psum
    CLEAR.P # Clear the partial sum register


    LOOPEND  # End global loop

    HALT

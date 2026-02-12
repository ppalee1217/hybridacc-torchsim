gemv:

    SDMA.LOOP 1
    SDMA.ADDR 0
    SDMA.LEN 64  # STORE 64 steps of kernel data (4out * 2vector * 32dim)
    SDMA.SD 4 # start DMA store operation

    LDMA.ADDR 0
    LDMA.LEN 256 # LOAD 256 steps of input data (32dim * 8input)
    LDMA.LHB 1 # start DMA load operation (fp16, broadcasted to 4out)

    SYS.CTRL (SDMA.ACT)

    # LOOPIN 4  # Global loop for 4x data volume

load_kernel:
    SYS.SYNC (SWAPDM) # Wait for kernel data to be ready

loop_in_dim:
    LOOPIN 32  # Loop for 32 input dimensions

load_input:
    VTSTORE vt0
    VTSTORE vt1
    VTSTORE vt2

    SYS.CTRL (RST.PID, RST.TID, LDMA.ACT) # Start loading input data for the current dimension
compute_gemv:
    LOOPIN 8 # Loop for 8 output dimensions
    VMULR 1, 1
    VMULR 1, 1
    VMULRN 1, VTRST # reset vector register id
    LOOPEND # compute_gemv

    LOOPEND # loop_in_dim

psum:
    # Calculate the partial sum for each output dimension
    SYS.CTRL (RST.PID)
    LOOPIN 24 # Loop for 24 partial sums
    VPSUMR 1
    LOOPEND # psum
    SYS.CTRL (CLEAR.P)
    #LOOPEND  # End global loop

    HALT

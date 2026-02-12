# ISA v3
gemv:

setup:
    SDMA.ADDR 0
    SDMA.LEN 48  # STORE 48 steps of kernel data (16 kernels * 3 vector each)
    SDMA.LOOP 1  # loop for 1 kernel set
    SDMA.SD 4  # start DMA store operation

    LDMA.ADDR 0
    LDMA.LEN 256 # LOAD 256 steps of input data (32dim * 8input)
    LDMA.LHB 1 # start DMA load operation (fp16, broadcasted to 4out)

    SYS.CTRL (SDMA.ACT)

compute:
    LOOPIN 1 # processing pass
    SYS.SYNC (SWAPDM)  # wait for previous SDMA operation to complete

    SYS.CTRL (RST.PID, RST.TID, LDMA.ACT, CLEAR.P)

loop_in_dim:
    LOOPIN 32  # Loop for 32 input dimensions

load_input:
    VTSTORE vt0
    VTSTORE vt1
    VTSTORE vt2

    SYS.CTRL (RST.PID, RST.TID)
compute_gemv:
    LOOPIN 8 # Loop for 8 output dimensions
    VMULR 1, 1
    VMULR 1, 1
    VMULRN 1, VTRST # reset vector register id
    LOOPEND # compute_gemv

    SYS.CTRL (RST.PID)

    LOOPEND # loop_in_dim

psum:
    # Calculate the partial sum for each output dimension
    LOOPIN 24 # Loop for 24 partial sums
    VPSUMR 1
    LOOPEND # psum

    LOOPEND
    HALT  # End of program

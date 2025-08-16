# Illegal Instruction
# DMA.ADDR, start_addr
# DMA.LEN, len
# DMA.LB, stride
# DMA.LH, stride
# DMA.LW, stride
# DMA.LD, stride
# DMA.LBB, stride
# DMA.LHB, stride
# DMA.LWB, stride
# DMA.SD, len
# TSTORE, trd
# TSHIFT, kernel_size
# J, imm
# LOOPIN, loop_count
# LOOPBREAK
# LOOPEND

# NOP
# VMAC, prd, vtrs
# VMACN, prd, vtrs
# VMACR, pstride, vtstride
# VMACRN, pstride, vtstride
# VMUL, vprd, vtrs
# VMULN, vprd, vtrs
# VMULR, vpstride, vtstride
# VMULRN, vpstride, vtstride
# VPSUM, vprs
# VPSUMR, vprs
# SETRID.PT, pid, vtid
# SETRID.P, pid
# SETRID.T, vtid
# CLEAR.T
# CLEAR.P
# HALT


gemv:

load_kernel:
    DMA.ADDR 0
    DMA.LEN 64  # STORE 64 steps of kernel data (4out * 2vector * 32dim)
    DMA.SD 4 # start DMA store operation

    DMA.ADDR 0
    DMA.LEN 256 # LOAD 256 steps of input data (32dim * 8input)
    DMA.LHB 2 # start DMA load operation (fp16, broadcasted to 4out)

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

    LOOPEND # loop_in_dim

    SETRID.P 0
psum:
    # Calculate the partial sum for each output dimension
    LOOPIN 24 # Loop for 24 partial sums
    VPSUMR 1
    LOOPEND # psum

    HALT  # End of program

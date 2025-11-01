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


conv1d_3x3:

load_kernel:
    DMA.ADDR 0
    DMA.LEN $(CHANNEL_OUT*3)  # STORE 48 steps of kernel data (16 kernels * 3 vector each)
    DMA.SD 4  # start DMA store operation

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
    LOOPIN $(WIDTH-PAD*2)  # Loop for 800 input elements

load_input:
    TSTORE t2
    TSTORE t5
    TSTORE t8
    TSTORE t11

    DMA.ADDR 0
    DMA.LEN $(CHANNEL_OUT*3)  # STORE 48 steps of kernel data (16 kernels * 3 vector each)
    DMA.LD 4  # LOAD 48 steps of input data (3 vector * 4 elements each)
    SETRID.PT 0, 0

loop_kernel:
    LOOPIN $(CHANNEL_OUT)  # Loop for 16 kernels
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

    HALT  # End of program

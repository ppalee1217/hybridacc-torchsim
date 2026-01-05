conv2d_5x5:

load_kernel:
    DMA.ADDR 0
    DMA.LEN 48  # STORE 48 steps of kernel data
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
    LOOPIN 196  # Loop for 196 input elements

load_input:
    TSTORE t4
    TSTORE t10

    DMA.ADDR 0
    DMA.LEN 48
    DMA.LD 4  # LOAD 48 steps of input data
    SETRID.PT 0, 0

loop_kernel:
    LOOPIN 16  # Loop for 16 kernels
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
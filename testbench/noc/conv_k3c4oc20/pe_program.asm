conv2d_3x3:

load_kernel:
    DMA.ADDR 0
    DMA.LEN 60  # STORE 60 steps of kernel data
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
    LOOPIN 198  # Loop for 198 input elements

load_input:
    TSTORE t2
    TSTORE t5
    TSTORE t8
    TSTORE t11

    DMA.ADDR 0
    DMA.LEN 60
    DMA.LD 4  # LOAD 60 steps of input data
    SETRID.PT 0, 0

loop_kernel:
    LOOPIN 20  # Loop for 20 kernels
    VMACRN 0, 1
    VMACRN 0, 1
    VMACRN 1, VTRST # reset vector register id
    LOOPEND

calculate_psum:
    VPSUM vp0
    VPSUM vp1
    VPSUM vp2
    VPSUM vp3
    VPSUM vp4

    CLEAR.P # Clear the partial sum register

    TSHIFT K3
    LOOPEND

    HALT  # End of program
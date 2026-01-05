conv1d_7x7:

load_kernel:
    DMA.ADDR 0
    DMA.LEN 48  # STORE 48 steps of kernel data (16 kernels * 3 vector each)
    DMA.SD 4  # start DMA store operation

preload_input:
    TSTORE t0
    TSTORE t1
    TSTORE t2
    TSTORE t3
    TSTORE t4
    TSTORE t5

loop_window:
    LOOPIN 397  # Loop for 800 input elements

load_input:
    TSTORE t6

    DMA.ADDR 0
    DMA.LEN 48
    DMA.LD 4  # LOAD 48 steps of input data (3 vector * 4 elements each)
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

    TSHIFT K7
    TSTORE t6
    TSHIFT K7
    LOOPEND

    HALT  # End of program

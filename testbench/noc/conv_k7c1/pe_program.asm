conv2d_7x7:

    SDMA.LOOP 4
    SDMA.ADDR 0
    SDMA.LEN 48  # STORE 48 steps of kernel data
    SDMA.SD 4  # start DMA store operation

    LOOPIN 4  # Global loop for 4x data volume

load_kernel:
    SWAPDM

preload_input:
    TSTORE t0
    TSTORE t1
    TSTORE t2
    TSTORE t3
    TSTORE t4
    TSTORE t5

loop_window:
    LOOPIN 194  # Loop for 194 input elements

load_input:
    TSTORE t6

    LDMA.ADDR 0
    LDMA.LEN 48
    LDMA.LD 4  # LOAD 48 steps of input data
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
    LOOPEND


    LOOPEND  # End global loop

    HALT

conv2d_3x3:

    SDMA.LOOP 4
    SDMA.ADDR 0
    SDMA.LEN 120  # STORE 120 steps of kernel data
    SDMA.SD 4  # start DMA store operation

    LOOPIN 4  # Global loop for 4x data volume

load_kernel:
    SWAPDM

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

    LDMA.ADDR 0
    LDMA.LEN 120
    LDMA.LD 4  # LOAD 120 steps of input data
    SETRID.PT 0, 0

loop_kernel:
    LOOPIN 40  # Loop for 40 kernels
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
    VPSUM vp5
    VPSUM vp6
    VPSUM vp7
    VPSUM vp8
    VPSUM vp9

    CLEAR.P # Clear the partial sum register

    TSHIFT K3
    LOOPEND


    LOOPEND  # End global loop

    HALT

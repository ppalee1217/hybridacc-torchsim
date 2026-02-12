conv2d_3x3:

    SDMA.LOOP 4
    SDMA.ADDR 0
    SDMA.LEN 48  # STORE 48 steps of kernel data
    SDMA.SD 4  # start DMA store operation

    LDMA.ADDR 0
    LDMA.LEN 48
    LDMA.LD 4  # LOAD 48 steps of input data

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
    VTSTORE vt2 # t2,t5,t8,t11
    RESET PC,TC,LDMA,PR

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

    TSHIFT K3
    LOOPEND


    LOOPEND  # End global loop

    HALT

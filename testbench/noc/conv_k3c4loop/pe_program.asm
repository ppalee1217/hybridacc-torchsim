conv2d_3x3:

    SDMA.LOOP 4
    SDMA.ADDR 0
    SDMA.LEN 48  # STORE 48 steps of kernel data
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
    SETRID.P 0
    LOOPIN 4  # Loop for 4 partial sum calculations
    VPSUMR 1
    LOOPEND
    CLEAR.P # Clear the partial sum register

    TSHIFT K3
    LOOPEND


    LOOPEND  # End global loop

    HALT

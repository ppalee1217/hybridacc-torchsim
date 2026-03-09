conv2d_3x3:

setup:
    SDMA.ADDR 0
    SDMA.LEN 48  # STORE 48 steps of kernel data (16 kernels * 3 vector each)
    SDMA.LOOP 1  # loop for 1 kernel set
    SDMA.SD 4  # start DMA store operation

    LDMA.ADDR 0
    LDMA.LEN 48
    LDMA.LD 4  # LOAD 48 steps of input data (3 vector * 4 elements each)

    SYS.CTRL (SDMA.ACT)

compute_loop:
    LOOPIN 1  # Loop for 1 kernel set
preload_input:
    VTSTORE vt0
    VTSTORE vt1
    VTSTORE vt2
    SYS.SYNC (SWAPDM)

loop_window:
    LOOPIN 197  # Loop for 197 input elements
    SYS.CTRL (RST.PID, RST.TID, LDMA.ACT, CLEAR.P)
    LOOPIN 16  # Loop for 16 kernels
    VMACRN 0, 1
    VMACRN 0, 1
    VMACRN 1, VTRST # reset vector register id
    LOOPEND
    VPSUM_TSHIFT vp0, K3
    VPSUM_VTSTORE vp1, vt2
    VPSUM vp2
    VPSUM vp3
    LOOPEND

last:
    SYS.CTRL (RST.PID, RST.TID, LDMA.ACT, CLEAR.P)
    LOOPIN 16  # Loop for 16 kernels
    VMACRN 0, 1
    VMACRN 0, 1
    VMACRN 1, VTRST # reset vector register id
    LOOPEND
    VPSUM vp0
    VPSUM vp1
    VPSUM vp2
    VPSUM vp3

    LOOPEND

    HALT  # End of program

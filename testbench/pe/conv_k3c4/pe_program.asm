conv1d_3x3:

setup:
    SDMA.ADDR 0
    SDMA.LEN 48  # STORE 48 steps of kernel data (16 kernels * 3 vector each)
    SDMA.LOOP 1  # loop for 1 kernel set
    SDMA.SD 4  # start DMA store operation

    LDMA.ADDR 0
    LDMA.LEN 48
    LDMA.LD 4  # LOAD 48 steps of input data (3 vector * 4 elements each)

    SYS.CTRL (SDMA.ACT)

compute:
    SYS.SYNC (SWAPDM)
preload_input:
    VTSTORE vt0
    VTSTORE vt1
    VTSTORE vt2

loop_window:
    LOOPIN 797  # Loop for 800 input elements
    SYS.CTRL (RST.PID, RST.TID, LDMA.ACT, CLEAR.P)
loop_kernel:
    LOOPIN 16  # Loop for 16 kernels
    VMACRN 0, 1
    VMACRN 0, 1
    VMACRN 1, VTRST # reset vector register id
    LOOPEND
calculate_psum:
    VPSUM_TSHIFT vp0, K3
    VPSUM_VTSTORE vp1, vt2
    VPSUM vp2
    VPSUM vp3
    LOOPEND

finalize:
    SYS.CTRL (RST.PID, RST.TID, LDMA.ACT, CLEAR.P)
loop_kernel_2:
    LOOPIN 16  # Loop for 16 kernels
    VMACRN 0, 1
    VMACRN 0, 1
    VMACRN 1, VTRST # reset vector register id
    LOOPEND
calculate_psum_2:
    VPSUM vp0
    VPSUM vp1
    VPSUM vp2
    VPSUM vp3

    HALT  # End of program

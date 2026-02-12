# ISA v3
conv1d_7x7:

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
    LOOPIN 1 # processing pass
    SYS.SYNC (SWAPDM)

preload_input:
    TSTORE t0
    TSTORE t1
    TSTORE t2
    TSTORE t3
    TSTORE t4


loop_window:
    LOOPIN 397  # Loop for 800 input elements

load_input:
    TSTORE t5
    TSTORE t6

    SYS.CTRL (RST.PID, RST.TID, LDMA.ACT, CLEAR.P)

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

    TSHIFT K7
    TSHIFT K7
    LOOPEND

    LOOPEND
    HALT  # End of program

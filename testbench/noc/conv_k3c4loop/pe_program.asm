# ISA v3 (ported from the pre-v3 program the way upstream 1636e7e ported testbench/pe/conv_k5c2;
# this fixture's own property, the partial sums drained by a VPSUMR loop, is kept)
conv2d_3x3:

setup:
    SDMA.ADDR 0
    SDMA.LEN 48  # STORE 48 steps of kernel data (16 kernels * 3 vector each)
    SDMA.LOOP 4  # loop for 4 kernel sets
    SDMA.SD 4  # start DMA store operation

    LDMA.ADDR 0
    LDMA.LEN 48
    LDMA.LD 4  # LOAD 48 steps of input data

    SYS.CTRL (SDMA.ACT)

compute:
    LOOPIN 4  # Global loop for 4x data volume
    SYS.SYNC (SWAPDM)

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

    SYS.CTRL (RST.PID, RST.TID, LDMA.ACT, CLEAR.P)

loop_kernel:
    LOOPIN 16  # Loop for 16 kernels
    VMACRN 0, 1
    VMACRN 0, 1
    VMACRN 1, VTRST # reset vector register id
    LOOPEND

calculate_psum:
    SYS.CTRL (RST.PID)
    LOOPIN 4  # Loop for 4 partial sum calculations
    VPSUMR 1
    LOOPEND

    TSHIFT K3
    LOOPEND

    LOOPEND  # End global loop
    HALT

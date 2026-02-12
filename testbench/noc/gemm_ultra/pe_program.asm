gemm_tiled:

    # 1) C tile store prefetch
    SDMA.LOOP 4
    SDMA.ADDR 0
    SDMA.LEN 64  # STORE C-tile (example: 4out * 2vector * 32dim)
    SDMA.SD 4 # start DMA store operation

    # 2) A/B tile load prefetch
    LDMA.ADDR 0
    LDMA.LEN 256 # LOAD A/B tile data (example: 32dim * 8input)
    LDMA.LHB 1 # start DMA load operation (fp16, broadcasted)

    SYS.CTRL (SDMA.ACT)
load_ab_tile:
    # Global tile loop: iterate over M tiles for a fixed (N,K) tile
    LOOPIN 4  # N-tiles (example: 2)
    SYS.SYNC (SWAPDM) # Wait for A/B tile to be ready

    LOOPIN 4  # M-tiles (example: 2)
    SYS.CTRL (CLEAR.P, RST.PID, RST.TID, LDMA.ACT) # Start loading next A/B slice

    loop_k_dim:
        LOOPIN 32  # K dimension within tile

        load_a_b:
            VTSTORE vt0
            VTSTORE vt1
            VTSTORE vt2

        compute_outer:
            LOOPIN 7 # N dimension within tile
                VMULR 1, 1
                VMULR 1, 1
                VMULRN 1, VTRST # reset vector register id
            LOOPEND # compute_outer
            VMULR 1, 1
            VMULR 1, 1
            VMULRN PRST, VTRST # reset vector register id
        LOOPEND # loop_k_dim

    accumulate_c:
        # Partial sum for C tile
        LOOPIN 23 # number of partial sums in tile
            VPSUMR 1
        LOOPEND # accumulate_c
        VPSUM vp23

    LOOPEND  # End M-tiles loop
    LOOPEND

    HALT

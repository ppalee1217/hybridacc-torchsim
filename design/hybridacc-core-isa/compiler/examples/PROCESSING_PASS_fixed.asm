.section .data
# Constants
WEIGHT_BASE_ADDR:          .word 0
INPUT_BASE_ADDR:          .word 65536
OUTPUT_BASE_ADDR:          .word 131072
TEMP_BUFFER_ADDR:          .word 196608
NOC_PS_OFFSET:          .word 0
NOC_PD_OFFSET:          .word 64
NOC_PI_OFFSET:          .word 128
NOC_PO_OFFSET:          .word 192
CORE_ROWS:          .word 4
CORE_COLS:          .word 16
KERNEL_SIZE:          .word 3
STRIDE:          .word 1
PADDING:          .word 1
CONV2D_PASS_CO:          .word 64
CONV2D_PASS_CI_K3:          .word 4
CONV2D_PASS_HO:          .word 16
CONV2D_PASS_HI:          .word 18
CONV2D_PASS_WO:          .word 200
CONV2D_PASS_WI:          .word 202

.section .text
.global _start

_start:
    # Load constants into registers
    LUI x1, 0x00000      # WEIGHT_BASE_ADDR high
    LUI x2, 0x00001      # INPUT_BASE_ADDR high
    LUI x3, 0x00002      # OUTPUT_BASE_ADDR high

    # Statement 1
    # Variable declaration: WEIGHT_BASE_ADDR
    LI x4, 0
    MV x5, x4
    # Registers: 1/27 used, 1 in pool
    # Statement 2
    # Variable declaration: INPUT_BASE_ADDR
    LUI x4, 16
    MV x6, x4
    # Registers: 2/27 used, 1 in pool
    # Statement 3
    # Variable declaration: OUTPUT_BASE_ADDR
    LUI x4, 32
    MV x7, x4
    # Registers: 3/27 used, 1 in pool
    # Statement 4
    # Variable declaration: TEMP_BUFFER_ADDR
    LUI x4, 48
    MV x8, x4
    # Registers: 4/27 used, 1 in pool
    # Statement 5
    # Variable declaration: NOC_PS_OFFSET
    LI x4, 0
    MV x9, x4
    # Registers: 5/27 used, 1 in pool
    # Statement 6
    # Variable declaration: NOC_PD_OFFSET
    LI x4, 64
    MV x10, x4
    # Registers: 6/27 used, 1 in pool
    # Statement 7
    # Variable declaration: NOC_PI_OFFSET
    LI x4, 128
    MV x11, x4
    # Registers: 7/27 used, 1 in pool
    # Statement 8
    # Variable declaration: NOC_PO_OFFSET
    LI x4, 192
    MV x12, x4
    # Registers: 8/27 used, 1 in pool
    # Statement 9
    # Variable declaration: CORE_ROWS
    LI x4, 4
    MV x13, x4
    # Registers: 9/27 used, 1 in pool
    # Statement 10
    # Variable declaration: CORE_COLS
    LI x4, 16
    MV x14, x4
    # Registers: 10/27 used, 1 in pool
    # Statement 11
    # Variable declaration: KERNEL_SIZE
    LI x4, 3
    MV x15, x4
    # Registers: 11/27 used, 1 in pool
    # Statement 12
    # Variable declaration: STRIDE
    LI x4, 1
    MV x16, x4
    # Registers: 12/27 used, 1 in pool
    # Statement 13
    # Variable declaration: PADDING
    LI x4, 1
    MV x17, x4
    # Registers: 13/27 used, 1 in pool
    # Statement 14
    # Variable declaration: CONV2D_PASS_CO
    LI x4, 64
    MV x18, x4
    # Registers: 14/27 used, 1 in pool
    # Statement 15
    # Variable declaration: CONV2D_PASS_CI_K3
    LI x4, 4
    MV x19, x4
    # Registers: 15/27 used, 1 in pool
    # Statement 16
    # Variable declaration: CONV2D_PASS_HO
    MV x4, x14
    # Registers: 16/27 used, 1 in pool
    # Statement 17
    # Variable declaration: CONV2D_PASS_HI
    LI x4, 1
    SUB x20, x4, x4
    MUL x21, x20, x16
    ADD x20, x21, x15
    MV x21, x20
    # Registers: 17/27 used, 2 in pool
    # Statement 18
    # Variable declaration: CONV2D_PASS_WO
    LI x21, 200
    MV x20, x21
    # Registers: 18/27 used, 1 in pool
    # Statement 19
    # Variable declaration: CONV2D_PASS_WI
    LI x20, 1
    SUB x22, x20, x20
    MUL x23, x22, x16
    ADD x22, x23, x15
    MV x23, x22
    # Registers: 19/27 used, 2 in pool
    # Statement 20
    # For loop: co
    LI x23, 0
    MV x22, x23
loop_start1:
    SLT x22, x22, x18
    BEQ x22, x0, loop_end2
    # For loop: kh
    LI x24, 0
    MV x25, x24
loop_start4:
    SLT x26, x25, x15
    BEQ x26, x0, loop_end5
    # For loop: kw
    LI x27, 0
    MV x28, x27
loop_start7:
    SLT x29, x28, x15
    BEQ x29, x0, loop_end8
    # For loop: ci
    LI x30, 0
    MV x5, x30
loop_start10:
    SLT x24, x5, x19
    BEQ x24, x0, loop_end11
    # Variable declaration: dm_addr
    LI x24, 0
    MUL x24, x22, x19
    ADD x24, x24, x5
    MUL x24, x24, x15
    ADD x24, x24, x25
    MUL x24, x24, x15
    ADD x24, x24, x28
    ADD x24, x24, x24
    # Variable declaration: weight_data
    LDV v0, 0(x24)
    # Vector variable weight_data uses vector register v0
    # Variable declaration: noc_addr
    MUL x24, x25, x15
    ADD x26, x24, x28
    MUL x26, x26, x19
    ADD x26, x9, x26
    ADD x26, x26, x5
    STV v0, 0(x26)
loop_continue12:
    LI x26, 4
    ADD x5, x5, x26
    JMP loop_start10
loop_end11:
loop_continue9:
    LI x27, 1
    ADD x28, x28, x27
    JMP loop_start7
loop_end8:
loop_continue6:
    LI x29, 1
    ADD x25, x25, x29
    JMP loop_start4
loop_end5:
loop_continue3:
    LI x27, 1
    ADD x22, x22, x27
    JMP loop_start1
loop_end2:
    # Registers: 27/27 used, 0 in pool
    # Statement 21
    # For loop: wi
    LI x27, 0
    MV x6, x27
loop_start13:
    SLT x27, x6, x23
    BEQ x27, x0, loop_end14
    # For loop: hi
    LI x27, 0
    MV x7, x27
loop_start16:
    SLT x27, x7, x21
    BEQ x27, x0, loop_end17
    # For loop: ci
    LI x27, 0
    MV x5, x27
loop_start19:
    SLT x27, x5, x19
    BEQ x27, x0, loop_end20
    # If statement
    SLT x27, x6, x17
    SUB x27, x23, x17
    SLT x27, x6, x27
    XOR x27, x27, 1
    OR x27, x27, x27
    BEQ x27, x0, else22
    # Variable declaration: noc_addr
    ADD x27, x10, x7
    MV x26, x27
    LI x27, 0
    STV v0, 0(x26)
    JMP loop_continue21
    JMP endif23
else22:
endif23:
    # Variable declaration: dm_addr
    LUI x27, 16
    MUL x27, x5, x21
    ADD x27, x27, x7
    MUL x27, x27, x23
    ADD x27, x27, x27
    ADD x27, x27, x6
    MV x24, x27
    # Variable declaration: noc_addr
    ADD x27, x10, x7
    MV x26, x27
    # Variable declaration: data
    LDV v0, 0(x24)
    # Vector variable data uses vector register v0
    STV v0, 0(x26)
loop_continue21:
    LI x27, 1
    ADD x5, x5, x27
    JMP loop_start19
loop_end20:
loop_continue18:
    LI x27, 1
    ADD x7, x7, x27
    JMP loop_start16
loop_end17:
    # For loop: ho
    LI x27, 0
    MV x8, x27
loop_start24:
    SLT x27, x8, x4
    BEQ x27, x0, loop_end25
    # For loop: co
    LI x27, 0
    MV x22, x27
loop_start27:
    SLT x27, x22, x18
    BEQ x27, x0, loop_end28
    # Variable declaration: dm_addr
    LUI x27, 32
    MUL x27, x22, x4
    ADD x27, x27, x8
    MUL x27, x27, x20
    ADD x27, x27, x27
    ADD x27, x27, x6
    MV x24, x27
    # Variable declaration: noc_addr
    ADD x27, x11, x8
    MV x26, x27
    # Variable declaration: partial_sum
    LDV v0, 0(x24)
    # Vector variable partial_sum uses vector register v0
    STV v0, 0(x26)
loop_continue29:
    LI x27, 4
    ADD x22, x22, x27
    JMP loop_start27
loop_end28:
loop_continue26:
    LI x27, 1
    ADD x8, x8, x27
    JMP loop_start24
loop_end25:
    # For loop: ho
    LI x27, 0
    MV x8, x27
loop_start30:
    SLT x27, x8, x4
    BEQ x27, x0, loop_end31
    # For loop: co
    LI x27, 0
    MV x22, x27
loop_start33:
    SLT x27, x22, x18
    BEQ x27, x0, loop_end34
    # Variable declaration: noc_addr
    ADD x27, x12, x8
    MV x26, x27
    # Variable declaration: output_data
    LDV v0, 0(x26)
    # Vector variable output_data uses vector register v0
    # Variable declaration: dm_addr
    LUI x27, 32
    MUL x27, x22, x4
    ADD x27, x27, x8
    MUL x27, x27, x20
    ADD x27, x27, x27
    ADD x27, x27, x6
    MV x24, x27
    STV v0, 0(x24)
loop_continue35:
    LI x27, 4
    ADD x22, x22, x27
    JMP loop_start33
loop_end34:
loop_continue32:
    LI x27, 1
    ADD x8, x8, x27
    JMP loop_start30
loop_end31:
loop_continue15:
    LI x27, 1
    ADD x6, x6, x27
    JMP loop_start13
loop_end14:
    # Registers: 26/27 used, 1 in pool

program_end:
    NOP
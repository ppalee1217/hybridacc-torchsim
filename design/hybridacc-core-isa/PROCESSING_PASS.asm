# PROCESSING_PASS.asm - Compiled from PROCESSING_PASS using core-ISA
# RV32I Assembly with vector extensions

.section .data
# Constants
WEIGHT_BASE_ADDR:   .word 0x00000000
INPUT_BASE_ADDR:    .word 0x00010000
OUTPUT_BASE_ADDR:   .word 0x00020000
TEMP_BUFFER_ADDR:   .word 0x00030000

NOC_PS_OFFSET:      .word 0x00
NOC_PD_OFFSET:      .word 0x40
NOC_PI_OFFSET:      .word 0x80
NOC_PO_OFFSET:      .word 0xc0

CORE_ROWS:          .word 4
CORE_COLS:          .word 16
KERNEL_SIZE:        .word 3
STRIDE:             .word 1
PADDING:            .word 1
CONV2D_PASS_CO:     .word 64
CONV2D_PASS_CI_K3:  .word 4
CONV2D_PASS_HO:     .word 16    # CORE_COLS
CONV2D_PASS_HI:     .word 18    # (CONV2D_PASS_HO - 1) * STRIDE + KERNEL_SIZE
CONV2D_PASS_WO:     .word 200
CONV2D_PASS_WI:     .word 202   # (CONV2D_PASS_WO - 1) * STRIDE + KERNEL_SIZE

.section .text
.global _start

_start:
    # Load constants into registers
    LUI x1, 0x00000      # WEIGHT_BASE_ADDR high
    LUI x2, 0x00001      # INPUT_BASE_ADDR high
    LUI x3, 0x00002      # OUTPUT_BASE_ADDR high

    LI x4, 0x00          # NOC_PS_OFFSET
    LI x5, 0x40          # NOC_PD_OFFSET
    LI x6, 0x80          # NOC_PI_OFFSET
    LI x7, 0xc0          # NOC_PO_OFFSET

    LI x8, 3             # KERNEL_SIZE
    LI x9, 1             # STRIDE
    LI x10, 1            # PADDING
    LI x11, 64           # CONV2D_PASS_CO
    LI x12, 4            # CONV2D_PASS_CI_K3
    LI x13, 16           # CONV2D_PASS_HO
    LI x14, 18           # CONV2D_PASS_HI
    LI x15, 200          # CONV2D_PASS_WO
    LI x16, 202          # CONV2D_PASS_WI

# Load weights into PEs
load_weights:
    LI x17, 0            # co = 0

weight_co_loop:
    LI x18, 0            # kh = 0

weight_kh_loop:
    LI x19, 0            # kw = 0

weight_kw_loop:
    LI x20, 0            # ci = 0

weight_ci_loop:
    # Calculate dm_addr = WEIGHT_BASE_ADDR + (((co * CONV2D_PASS_CI_K3 + ci) * KERNEL_SIZE + kh) * KERNEL_SIZE + kw)
    MUL x21, x17, x12    # co * CONV2D_PASS_CI_K3
    ADD x21, x21, x20    # + ci
    MUL x21, x21, x8     # * KERNEL_SIZE
    ADD x21, x21, x18    # + kh
    MUL x21, x21, x8     # * KERNEL_SIZE
    ADD x21, x21, x19    # + kw
    ADD x21, x21, x1     # + WEIGHT_BASE_ADDR

    # Load vector weight_data
    LDV v0, 0(x21)

    # Calculate noc_addr = NOC_PS_OFFSET + (kh * KERNEL_SIZE + kw) * CONV2D_PASS_CI_K3 + ci
    MUL x22, x18, x8     # kh * KERNEL_SIZE
    ADD x22, x22, x19    # + kw
    MUL x22, x22, x12    # * CONV2D_PASS_CI_K3
    ADD x22, x22, x20    # + ci
    ADD x22, x22, x4     # + NOC_PS_OFFSET

    # Store vector to NOC
    STV v0, 0(x22)

    ADDI x20, x20, 4     # ci += 4
    SLTU x23, x20, x12   # ci < CONV2D_PASS_CI_K3
    BEQ x23, x0, weight_ci_end
    JMP weight_ci_loop

weight_ci_end:
    ADDI x19, x19, 1     # kw++
    SLT x23, x19, x8     # kw < KERNEL_SIZE
    BEQ x23, x0, weight_kw_end
    JMP weight_kw_loop

weight_kw_end:
    ADDI x18, x18, 1     # kh++
    SLT x23, x18, x8     # kh < KERNEL_SIZE
    BEQ x23, x0, weight_kh_end
    JMP weight_kh_loop

weight_kh_end:
    ADDI x17, x17, 1     # co++
    SLT x23, x17, x11    # co < CONV2D_PASS_CO
    BEQ x23, x0, main_processing
    JMP weight_co_loop

# Main processing loop
main_processing:
    LI x17, 0            # wi = 0

main_wi_loop:
    # Send input activations
    LI x18, 0            # hi = 0

input_hi_loop:
    LI x19, 0            # ci = 0

input_ci_loop:
    # Check padding: if wi < PADDING or wi >= CONV2D_PASS_WI - PADDING
    SLT x20, x17, x10    # wi < PADDING
    SUB x21, x16, x10    # CONV2D_PASS_WI - PADDING
    SLTU x22, x17, x21   # wi < (CONV2D_PASS_WI - PADDING)
    XOR x22, x22, 1      # wi >= (CONV2D_PASS_WI - PADDING)
    OR x20, x20, x22     # padding condition

    BEQ x20, x0, no_padding

    # Send zero for padding
    ADD x23, x5, x18     # NOC_PD_OFFSET + hi
    LI x24, 0
    STV x24, 0(x23)      # Store zero vector
    JMP input_ci_continue

no_padding:
    # Calculate dm_addr = INPUT_BASE_ADDR + (ci * CONV2D_PASS_HI + hi) * CONV2D_PASS_WI + wi
    MUL x21, x19, x14    # ci * CONV2D_PASS_HI
    ADD x21, x21, x18    # + hi
    MUL x21, x21, x15    # * CONV2D_PASS_WO
    ADD x21, x21, x17    # + wi
    ADD x21, x21, x2     # + INPUT_BASE_ADDR

    # Load vector data
    LDV v0, 0(x21)

    # Calculate noc_addr = NOC_PD_OFFSET + hi
    ADD x22, x5, x18     # NOC_PD_OFFSET + hi

    # Store vector to NOC
    STV v0, 0(x22)

input_ci_continue:
    ADDI x19, x19, 1     # ci++
    SLT x23, x19, x12    # ci < CONV2D_PASS_CI_K3
    BEQ x23, x0, input_ci_end
    JMP input_ci_loop

input_ci_end:
    ADDI x18, x18, 1     # hi++
    SLT x23, x18, x14    # hi < CONV2D_PASS_HI
    BEQ x23, x0, partial_sums_send
    JMP input_hi_loop

partial_sums_send:
    # Send input partial sums
    LI x18, 0            # ho = 0

partial_send_ho_loop:
    LI x19, 0            # co = 0

partial_send_co_loop:
    # Calculate dm_addr = OUTPUT_BASE_ADDR + (co * CONV2D_PASS_HO + ho) * CONV2D_PASS_WO + wi
    MUL x20, x19, x13    # co * CONV2D_PASS_HO
    ADD x20, x20, x18    # + ho
    MUL x20, x20, x15    # * CONV2D_PASS_WO
    ADD x20, x20, x17    # + wi
    ADD x20, x20, x3     # + OUTPUT_BASE_ADDR

    # Load partial sum
    LDV v0, 0(x20)

    # Calculate noc_addr = NOC_PI_OFFSET + ho
    ADD x21, x6, x18     # NOC_PI_OFFSET + ho

    # Store partial sum to NOC
    STV v0, 0(x21)

    ADDI x19, x19, 4     # co += 4
    SLT x23, x19, x11    # co < CONV2D_PASS_CO
    BEQ x23, x0, partial_send_co_end
    JMP partial_send_co_loop

partial_send_co_end:
    ADDI x18, x18, 1     # ho++
    SLT x23, x18, x13    # ho < CONV2D_PASS_HO
    BEQ x23, x0, partial_sums_receive
    JMP partial_send_ho_loop

partial_sums_receive:
    # Receive output partial sums
    LI x18, 0            # ho = 0

partial_receive_ho_loop:
    LI x19, 0            # co = 0

partial_receive_co_loop:
    # Calculate noc_addr = NOC_PO_OFFSET + ho
    ADD x20, x7, x18     # NOC_PO_OFFSET + ho

    # Load output data from NOC
    LDV v0, 0(x20)

    # Calculate dm_addr = OUTPUT_BASE_ADDR + (co * CONV2D_PASS_HO + ho) * CONV2D_PASS_WO + wi
    MUL x21, x19, x13    # co * CONV2D_PASS_HO
    ADD x21, x21, x18    # + ho
    MUL x21, x21, x15    # * CONV2D_PASS_WO
    ADD x21, x21, x17    # + wi
    ADD x21, x21, x3     # + OUTPUT_BASE_ADDR

    # Store output data
    STV v0, 0(x21)

    ADDI x19, x19, 4     # co += 4
    SLT x23, x19, x11    # co < CONV2D_PASS_CO
    BEQ x23, x0, partial_receive_co_end
    JMP partial_receive_co_loop

partial_receive_co_end:
    ADDI x18, x18, 1     # ho++
    SLT x23, x18, x13    # ho < CONV2D_PASS_HO
    BEQ x23, x0, main_wi_continue
    JMP partial_receive_ho_loop

main_wi_continue:
    ADDI x17, x17, 1     # wi++
    SLT x23, x17, x16    # wi < CONV2D_PASS_WI
    BEQ x23, x0, program_end
    JMP main_wi_loop

program_end:
    NOP
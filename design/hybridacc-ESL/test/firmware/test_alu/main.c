/**
 * @file main.c
 * @brief Test RV32I ALU instructions (OP and OP-IMM).
 *
 * Covers: ADD, SUB, AND, OR, XOR, SLL, SRL, SRA, SLT, SLTU
 *         ADDI, ANDI, ORI, XORI, SLLI, SRLI, SRAI, SLTI, SLTIU
 *         LUI, AUIPC
 */

#include "hacc_test.h"

void main(void)
{
    TEST_INIT();
    uint32_t r;

    /* ---- Register-Immediate (OP-IMM) ---- */

    /* T001: ADDI */
    r = 0;
    __asm__ volatile ("li %0, 100\n addi %0, %0, 23" : "=r"(r));
    TEST_EQ(1, r, 123);

    /* T002: ADDI negative */
    r = 0;
    __asm__ volatile ("li %0, 100\n addi %0, %0, -50" : "=r"(r));
    TEST_EQ(2, r, 50);

    /* T003: ADDI overflow (wraps) */
    r = 0;
    __asm__ volatile ("li %0, -1\n addi %0, %0, 1" : "=r"(r));
    TEST_EQ(3, r, 0);

    /* T004: ANDI */
    r = 0;
    __asm__ volatile ("li %0, 0xFF\n andi %0, %0, 0x0F" : "=r"(r));
    TEST_EQ(4, r, 0x0F);

    /* T005: ORI */
    r = 0;
    __asm__ volatile ("li %0, 0xF0\n ori %0, %0, 0x0F" : "=r"(r));
    TEST_EQ(5, r, 0xFF);

    /* T006: XORI */
    r = 0;
    __asm__ volatile ("li %0, 0xFF\n xori %0, %0, 0xFF" : "=r"(r));
    TEST_EQ(6, r, 0);

    /* T007: SLLI */
    r = 0;
    __asm__ volatile ("li %0, 1\n slli %0, %0, 4" : "=r"(r));
    TEST_EQ(7, r, 16);

    /* T008: SRLI */
    r = 0;
    __asm__ volatile ("li %0, 256\n srli %0, %0, 4" : "=r"(r));
    TEST_EQ(8, r, 16);

    /* T009: SRAI (arithmetic, preserves sign) */
    r = 0;
    __asm__ volatile ("li %0, -16\n srai %0, %0, 2" : "=r"(r));
    TEST_EQ(9, r, (uint32_t)(-4));

    /* T010: SLTI (signed less than immediate) */
    r = 99;
    __asm__ volatile ("li %0, -5\n slti %0, %0, 0" : "=r"(r));
    TEST_EQ(10, r, 1);  /* -5 < 0 → 1 */

    /* T011: SLTI (not less than) */
    r = 99;
    __asm__ volatile ("li %0, 5\n slti %0, %0, 0" : "=r"(r));
    TEST_EQ(11, r, 0);  /* 5 < 0 → 0 */

    /* T012: SLTIU (unsigned less than immediate) */
    r = 99;
    __asm__ volatile ("li %0, 3\n sltiu %0, %0, 5" : "=r"(r));
    TEST_EQ(12, r, 1);

    /* ---- Register-Register (OP) ---- */

    /* T013: ADD */
    {
        uint32_t a = 100, b = 200;
        __asm__ volatile ("add %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(13, r, 300);
    }

    /* T014: SUB */
    {
        uint32_t a = 500, b = 300;
        __asm__ volatile ("sub %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(14, r, 200);
    }

    /* T015: SUB underflow */
    {
        uint32_t a = 0, b = 1;
        __asm__ volatile ("sub %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(15, r, 0xFFFFFFFF);
    }

    /* T016: AND */
    {
        uint32_t a = 0xF0F0F0F0, b = 0xFF00FF00;
        __asm__ volatile ("and %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(16, r, 0xF000F000);
    }

    /* T017: OR */
    {
        uint32_t a = 0x0F0F0000, b = 0x00F0F0F0;
        __asm__ volatile ("or %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(17, r, 0x0FFFF0F0);
    }

    /* T018: XOR */
    {
        uint32_t a = 0xAAAAAAAA, b = 0x55555555;
        __asm__ volatile ("xor %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(18, r, 0xFFFFFFFF);
    }

    /* T019: SLL */
    {
        uint32_t a = 0x01, b = 8;
        __asm__ volatile ("sll %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(19, r, 0x100);
    }

    /* T020: SRL */
    {
        uint32_t a = 0x10000, b = 8;
        __asm__ volatile ("srl %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(20, r, 0x100);
    }

    /* T021: SRA */
    {
        uint32_t a = (uint32_t)(-128);
        uint32_t b = 4;
        __asm__ volatile ("sra %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(21, r, (uint32_t)(-8));
    }

    /* T022: SLT (signed) */
    {
        uint32_t a = (uint32_t)(-1);  /* -1 */
        uint32_t b = 1;
        __asm__ volatile ("slt %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(22, r, 1);  /* -1 < 1 → 1 */
    }

    /* T023: SLTU (unsigned) */
    {
        uint32_t a = 1, b = 0xFFFFFFFF;
        __asm__ volatile ("sltu %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(23, r, 1);  /* 1 < 0xFFFFFFFF → 1 */
    }

    /* ---- LUI ---- */

    /* T024: LUI loads upper 20 bits */
    __asm__ volatile ("lui %0, 0xABCDE" : "=r"(r));
    TEST_EQ(24, r, 0xABCDE000);

    /* T025: LUI + ADDI = arbitrary 32-bit constant */
    __asm__ volatile ("lui %0, 0x12345\n addi %0, %0, 0x678" : "=r"(r));
    TEST_EQ(25, r, 0x12345678);

    /* ---- AUIPC ---- */

    /* T026: AUIPC adds upper 20-bit immediate to PC */
    {
        uint32_t pc_val, auipc_val;
        __asm__ volatile (
            "auipc %0, 0\n"
            "auipc %1, 1\n"
            : "=r"(pc_val), "=r"(auipc_val)
        );
        /* auipc_val = (pc_of_second_auipc) + (1 << 12)
         * pc_of_second_auipc = pc_val + 4
         * So: auipc_val = pc_val + 4 + 0x1000
         */
        TEST_EQ(26, auipc_val, pc_val + 4 + 0x1000);
    }

    /* ---- XORI -1 = bitwise NOT ---- */
    /* T027: XORI with -1 as pseudo-NOT */
    r = 0;
    __asm__ volatile ("li %0, 0x12345678\n xori %0, %0, -1" : "=r"(r));
    TEST_EQ(27, r, 0xEDCBA987);

    /* ---- Zero register ---- */
    /* T028: Writing to x0 should have no effect */
    __asm__ volatile ("add x0, x0, x0\n mv %0, x0" : "=r"(r));
    TEST_EQ(28, r, 0);

    TEST_FINISH();
}

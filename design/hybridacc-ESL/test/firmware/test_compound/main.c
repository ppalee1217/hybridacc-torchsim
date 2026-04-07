/**
 * @file main.c
 * @brief Compound integration test — exercises all ISA features together.
 *
 * Covers: array operations, struct manipulation, memcpy/memset patterns,
 *         sorting algorithm, bitfield operations, pointer arithmetic,
 *         mixed ALU+MUL+LOAD+STORE+BRANCH sequences.
 */

#include "hacc_test.h"

#define WORK32 ((volatile uint32_t *)(TEST_WORK_BASE))

/* Bubble sort */
static void __attribute__((noinline)) bubble_sort(int *arr, int n) {
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - 1 - i; j++) {
            if (arr[j] > arr[j + 1]) {
                int tmp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = tmp;
            }
        }
    }
}

/* Simple memcpy */
static void __attribute__((noinline)) my_memcpy(void *dst, const void *src, unsigned n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (unsigned i = 0; i < n; i++) d[i] = s[i];
}

/* Simple memset */
static void __attribute__((noinline)) my_memset(void *dst, int c, unsigned n) {
    unsigned char *d = (unsigned char *)dst;
    for (unsigned i = 0; i < n; i++) d[i] = (unsigned char)c;
}

/* CRC32 (simple byte-at-a-time, exercises MUL+XOR+SHIFT) */
static uint32_t __attribute__((noinline)) crc32_simple(const uint8_t *data, unsigned len) {
    uint32_t crc = 0xFFFFFFFF;
    for (unsigned i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
            else crc >>= 1;
        }
    }
    return ~crc;
}

/* Matrix multiply 4x4 (exercises multiply+accumulate) */
static void __attribute__((noinline)) mat_mul_4x4(
    const int *a, const int *b, int *c)
{
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            int sum = 0;
            for (int k = 0; k < 4; k++) {
                sum += a[i * 4 + k] * b[k * 4 + j];
            }
            c[i * 4 + j] = sum;
        }
    }
}

void main(void)
{
    TEST_INIT();

    /* ---- Array and sorting ---- */

    /* T001: Bubble sort */
    {
        int arr[] = {5, 3, 8, 1, 9, 2, 7, 4, 6, 0};
        bubble_sort(arr, 10);
        int sorted = 1;
        for (int i = 0; i < 9; i++) {
            if (arr[i] > arr[i + 1]) sorted = 0;
        }
        TEST_ASSERT(1, sorted);
        TEST_EQ(2, arr[0], 0);
        TEST_EQ(3, arr[9], 9);
    }

    /* ---- Memory operations ---- */

    /* T004: memcpy */
    {
        uint32_t src[4] = {0xAAAA, 0xBBBB, 0xCCCC, 0xDDDD};
        uint32_t dst[4] = {0};
        my_memcpy(dst, src, 16);
        TEST_EQ(4, dst[0], 0xAAAA);
        TEST_EQ(5, dst[1], 0xBBBB);
        TEST_EQ(6, dst[2], 0xCCCC);
        TEST_EQ(7, dst[3], 0xDDDD);
    }

    /* T008: memset */
    {
        uint8_t buf[16];
        my_memset(buf, 0xAB, 16);
        TEST_EQ(8, buf[0], 0xAB);
        TEST_EQ(9, buf[15], 0xAB);
        /* Verify as word */
        uint32_t *wp = (uint32_t *)buf;
        TEST_EQ(10, wp[0], 0xABABABAB);
    }

    /* ---- CRC32 ---- */

    /* T011: Known CRC32 value */
    {
        const uint8_t data[] = "123456789";
        uint32_t crc = crc32_simple(data, 9);
        TEST_EQ(11, crc, 0xCBF43926);
    }

    /* T012: CRC32 of empty data */
    {
        uint8_t dummy = 0;
        uint32_t crc = crc32_simple(&dummy, 0);
        TEST_EQ(12, crc, 0);  /* ~0xFFFFFFFF with 0 iterations */
    }

    /* ---- Matrix multiply ---- */

    /* T013: Identity × arbitrary = arbitrary */
    {
        int identity[16] = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1
        };
        int mat[16] = {
            1, 2, 3, 4,
            5, 6, 7, 8,
            9, 10, 11, 12,
            13, 14, 15, 16
        };
        int result[16];
        mat_mul_4x4(identity, mat, result);
        int ok = 1;
        for (int i = 0; i < 16; i++) {
            if (result[i] != mat[i]) ok = 0;
        }
        TEST_ASSERT(13, ok);
    }

    /* T014: 2x scaling matrix */
    {
        int scale[16] = {
            2, 0, 0, 0,
            0, 2, 0, 0,
            0, 0, 2, 0,
            0, 0, 0, 2
        };
        int vec[16] = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1
        };
        int result[16];
        mat_mul_4x4(scale, vec, result);
        TEST_EQ(14, result[0], 2);
        TEST_EQ(15, result[5], 2);
        TEST_EQ(16, result[10], 2);
        TEST_EQ(17, result[15], 2);
    }

    /* ---- Bitfield operations ---- */

    /* T018: Extract bits */
    {
        uint32_t val = 0xDEADBEEF;
        uint32_t byte0 = val & 0xFF;
        uint32_t byte1 = (val >> 8) & 0xFF;
        uint32_t byte2 = (val >> 16) & 0xFF;
        uint32_t byte3 = (val >> 24) & 0xFF;
        TEST_EQ(18, byte0, 0xEF);
        TEST_EQ(19, byte1, 0xBE);
        TEST_EQ(20, byte2, 0xAD);
        TEST_EQ(21, byte3, 0xDE);
    }

    /* ---- Pointer arithmetic ---- */

    /* T022: Array via pointer */
    {
        int arr[4] = {10, 20, 30, 40};
        int *p = arr;
        int sum = 0;
        for (int i = 0; i < 4; i++) {
            sum += *p++;
        }
        TEST_EQ(22, sum, 100);
    }

    /* ---- Mixed ALU + MUL + Mem ---- */

    /* T023: Dot product */
    {
        int a[] = {1, 2, 3, 4};
        int b[] = {5, 6, 7, 8};
        int dot = 0;
        for (int i = 0; i < 4; i++) {
            dot += a[i] * b[i];
        }
        /* 5 + 12 + 21 + 32 = 70 */
        TEST_EQ(23, dot, 70);
    }

    /* T024: Sum of squares */
    {
        int sum_sq = 0;
        for (int i = 1; i <= 10; i++) {
            sum_sq += i * i;
        }
        TEST_EQ(24, sum_sq, 385);
    }

    /* T025: GCD (Euclidean algorithm) */
    {
        int a = 252, b = 105;
        while (b != 0) {
            int t = b;
            b = a - (a / b) * b;  /* a % b using sub+mul (no div) */

            /* Manual modulo since we don't have DIV: using repeated subtraction */
            int rem = a;
            while (rem >= t) rem -= t;
            b = rem;
            a = t;
        }
        TEST_EQ(25, a, 21);
    }

    TEST_FINISH();
}

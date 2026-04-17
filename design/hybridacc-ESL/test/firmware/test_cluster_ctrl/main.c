#include "hacc_test.h"

void trap_handler(uint32_t mcause, uint32_t mepc, uint32_t mtval)
{
    (void)mcause;
    (void)mepc;
    (void)mtval;
}

void main(void)
{
    const uint32_t cluster0 = cluster_addr(0, 0);
    TEST_INIT();

    mmio_write(LOCAL_CLUSTER_MASK_LO, 0x1u);
    mmio_write(LOCAL_CLUSTER_MASK_HI, 0x0u);

    mmio_write(cluster0 + CLUSTER_MODE_OFF, CLUSTER_MODE_LAYER_MANAGED);
    TEST_EQ(1, mmio_read(cluster0 + CLUSTER_MODE_OFF), CLUSTER_MODE_LAYER_MANAGED);

    mmio_write(cluster0 + CLUSTER_CTRL_OFF, CLUSTER_CTRL_SOFT_RESET);
    {
        uint32_t st = mmio_read(cluster0 + CLUSTER_STATUS_OFF);
        TEST_ASSERT(2, (st & CLUSTER_STATUS_IDLE) != 0u);
        TEST_ASSERT(3, (st & CLUSTER_STATUS_QUIESCED) != 0u);
        TEST_ASSERT(4, (st & CLUSTER_STATUS_DONE) != 0u);
    }

    mmio_write(cluster0 + CLUSTER_CTRL_OFF, CLUSTER_CTRL_START);
    {
        uint32_t st = mmio_read(cluster0 + CLUSTER_STATUS_OFF);
        TEST_ASSERT(5, (st & CLUSTER_STATUS_BUSY) != 0u);
        TEST_ASSERT(6, (st & CLUSTER_STATUS_IDLE) == 0u);
    }

    mmio_write(cluster0 + CLUSTER_CTRL_OFF, CLUSTER_CTRL_STOP);
    {
        uint32_t st = mmio_read(cluster0 + CLUSTER_STATUS_OFF);
        TEST_ASSERT(7, (st & CLUSTER_STATUS_IDLE) != 0u);
        TEST_ASSERT(8, (st & CLUSTER_STATUS_QUIESCED) != 0u);
        TEST_ASSERT(9, (st & CLUSTER_STATUS_DONE) != 0u);
        TEST_EQ(10, mmio_read(cluster0 + CLUSTER_ERROR_CODE_OFF), 0u);
        TEST_EQ(11, mmio_read(cluster0 + CLUSTER_SUBSTATE_OFF), 0u);
    }

    TEST_FINISH();
}
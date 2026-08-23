/*
 * QTest for the Ingchips ING916 board bring-up
 *
 * Verifies that the board boots without bus faults and that each
 * peripheral's IdRev register returns the reset value from the SVD.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

static void test_ing916_idrev(void)
{
    QTestState *qts = qtest_init("-M ing916 -nographic");

    g_assert_cmphex(qtest_readl(qts, 0x40001000), ==, 0x03002004u);
    g_assert_cmphex(qtest_readl(qts, 0x40002000), ==, 0x03031003u);
    g_assert_cmphex(qtest_readl(qts, 0x40015000), ==, 0x02031002u);
    g_assert_cmphex(qtest_readl(qts, 0x40101000), ==, 0x03011006u);
    g_assert_cmphex(qtest_readl(qts, 0x40130000), ==, 0x0000000au);
    g_assert_cmphex(qtest_readl(qts, 0x40180040), ==, 0x4f54294au);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/ing916/idrev", test_ing916_idrev);

    return g_test_run();
}

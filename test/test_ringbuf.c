#include "unity.h"
#include "dali_ringbuf.h"

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------*/
#define CAPACITY 8u  /* must be power of 2 */

static uint32_t     s_buf[CAPACITY];
static DaliRingBuf  s_rb;

void setUp(void)
{
    dali_rb_init(&s_rb, s_buf, CAPACITY);
}

void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------------*/

void test_empty_on_init(void)
{
    TEST_ASSERT_TRUE(dali_rb_empty(&s_rb));
}

void test_push_pop_single(void)
{
    TEST_ASSERT_EQUAL(DALI_OK, dali_rb_push_from_isr(&s_rb, 0xDEADBEEFu));
    TEST_ASSERT_FALSE(dali_rb_empty(&s_rb));

    uint32_t val = 0;
    TEST_ASSERT_EQUAL(DALI_OK, dali_rb_pop(&s_rb, &val));
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFu, val);
    TEST_ASSERT_TRUE(dali_rb_empty(&s_rb));
}

void test_fifo_ordering(void)
{
    for (uint32_t i = 0u; i < CAPACITY; i++) {
        TEST_ASSERT_EQUAL(DALI_OK, dali_rb_push_from_isr(&s_rb, i));
    }
    for (uint32_t i = 0u; i < CAPACITY; i++) {
        uint32_t val = 0;
        TEST_ASSERT_EQUAL(DALI_OK, dali_rb_pop(&s_rb, &val));
        TEST_ASSERT_EQUAL_UINT32(i, val);
    }
}

void test_overflow_at_capacity(void)
{
    /* Fill to capacity */
    for (uint32_t i = 0u; i < CAPACITY; i++) {
        TEST_ASSERT_EQUAL(DALI_OK, dali_rb_push_from_isr(&s_rb, i));
    }
    /* Next push must overflow */
    TEST_ASSERT_EQUAL(DALI_ERR_OVERFLOW, dali_rb_push_from_isr(&s_rb, 0xFFu));
}

void test_empty_pop_returns_timeout(void)
{
    uint32_t val = 0;
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT, dali_rb_pop(&s_rb, &val));
}

void test_clear_resets_buffer(void)
{
    dali_rb_push_from_isr(&s_rb, 1u);
    dali_rb_push_from_isr(&s_rb, 2u);
    dali_rb_clear(&s_rb);
    TEST_ASSERT_TRUE(dali_rb_empty(&s_rb));

    /* Can push again after clear */
    TEST_ASSERT_EQUAL(DALI_OK, dali_rb_push_from_isr(&s_rb, 42u));
}

void test_wrap_around(void)
{
    /* Push half, pop half, then fill again — exercises index wrap */
    for (uint32_t i = 0u; i < CAPACITY / 2u; i++) {
        dali_rb_push_from_isr(&s_rb, i);
    }
    uint32_t dummy;
    for (uint32_t i = 0u; i < CAPACITY / 2u; i++) {
        dali_rb_pop(&s_rb, &dummy);
    }
    for (uint32_t i = 0u; i < CAPACITY; i++) {
        TEST_ASSERT_EQUAL(DALI_OK, dali_rb_push_from_isr(&s_rb, i + 100u));
    }
    for (uint32_t i = 0u; i < CAPACITY; i++) {
        uint32_t val = 0;
        TEST_ASSERT_EQUAL(DALI_OK, dali_rb_pop(&s_rb, &val));
        TEST_ASSERT_EQUAL_UINT32(i + 100u, val);
    }
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------*/
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty_on_init);
    RUN_TEST(test_push_pop_single);
    RUN_TEST(test_fifo_ordering);
    RUN_TEST(test_overflow_at_capacity);
    RUN_TEST(test_empty_pop_returns_timeout);
    RUN_TEST(test_clear_resets_buffer);
    RUN_TEST(test_wrap_around);
    return UNITY_END();
}

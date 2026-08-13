#include "unity.h"
#include "../components/dali/dali_refresh_cursor.h"

void setUp(void) {}
void tearDown(void) {}

void test_cursor_is_initially_inactive(void) {
  DaliRefreshCursor cursor{};
  uint8_t index = 0xA5u;

  TEST_ASSERT_FALSE(dali_refresh_cursor_current(&cursor, 3u, &index));
  TEST_ASSERT_FALSE(dali_refresh_cursor_is_active(&cursor));
  TEST_ASSERT_EQUAL_HEX8(0xA5u, index);
}

void test_request_visits_each_entry_once(void) {
  DaliRefreshCursor cursor{};
  uint8_t index = 0xFFu;

  dali_refresh_cursor_request(&cursor);
  for (uint8_t expected = 0u; expected < 3u; expected++) {
    TEST_ASSERT_TRUE(dali_refresh_cursor_current(&cursor, 3u, &index));
    TEST_ASSERT_EQUAL_UINT8(expected, index);
    dali_refresh_cursor_complete(&cursor, 3u, DALI_REFRESH_ADVANCE);
  }
  TEST_ASSERT_FALSE(dali_refresh_cursor_current(&cursor, 3u, &index));
  TEST_ASSERT_FALSE(dali_refresh_cursor_is_active(&cursor));
}

void test_deferred_entry_keeps_same_index(void) {
  DaliRefreshCursor cursor{};
  uint8_t index = 0xFFu;

  dali_refresh_cursor_request(&cursor);
  TEST_ASSERT_TRUE(dali_refresh_cursor_current(&cursor, 3u, &index));
  TEST_ASSERT_EQUAL_UINT8(0u, index);

  /* Queue-full/scan pause: do not advance, so the next pump retries index 0. */
  TEST_ASSERT_TRUE(dali_refresh_cursor_current(&cursor, 3u, &index));
  TEST_ASSERT_EQUAL_UINT8(0u, index);

  dali_refresh_cursor_complete(&cursor, 3u, DALI_REFRESH_RETRY);
  TEST_ASSERT_TRUE(dali_refresh_cursor_current(&cursor, 3u, &index));
  TEST_ASSERT_EQUAL_UINT8(0u, index);

  dali_refresh_cursor_complete(&cursor, 3u, DALI_REFRESH_ADVANCE);
  TEST_ASSERT_TRUE(dali_refresh_cursor_current(&cursor, 3u, &index));
  TEST_ASSERT_EQUAL_UINT8(1u, index);
}

void test_advance_can_skip_ineligible_entries(void) {
  DaliRefreshCursor cursor{};
  uint8_t index = 0xFFu;

  dali_refresh_cursor_request(&cursor);
  TEST_ASSERT_TRUE(dali_refresh_cursor_current(&cursor, 4u, &index));
  TEST_ASSERT_EQUAL_UINT8(0u, index);
  dali_refresh_cursor_complete(&cursor, 4u, DALI_REFRESH_ADVANCE);
  dali_refresh_cursor_complete(&cursor, 4u, DALI_REFRESH_ADVANCE);
  TEST_ASSERT_TRUE(dali_refresh_cursor_current(&cursor, 4u, &index));
  TEST_ASSERT_EQUAL_UINT8(2u, index);
}

void test_overlapping_requests_coalesce_into_one_rerun(void) {
  DaliRefreshCursor cursor{};
  uint8_t index = 0xFFu;

  dali_refresh_cursor_request(&cursor);
  TEST_ASSERT_TRUE(dali_refresh_cursor_current(&cursor, 2u, &index));
  TEST_ASSERT_EQUAL_UINT8(0u, index);
  dali_refresh_cursor_complete(&cursor, 2u, DALI_REFRESH_ADVANCE);

  dali_refresh_cursor_request(&cursor);
  dali_refresh_cursor_request(&cursor);
  TEST_ASSERT_TRUE(dali_refresh_cursor_has_pending_request(&cursor));

  TEST_ASSERT_TRUE(dali_refresh_cursor_current(&cursor, 2u, &index));
  TEST_ASSERT_EQUAL_UINT8(1u, index);
  dali_refresh_cursor_complete(&cursor, 2u, DALI_REFRESH_ADVANCE);
  TEST_ASSERT_TRUE(dali_refresh_cursor_current(&cursor, 2u, &index));
  TEST_ASSERT_EQUAL_UINT8(0u, index);
  TEST_ASSERT_FALSE(dali_refresh_cursor_has_pending_request(&cursor));
  dali_refresh_cursor_complete(&cursor, 2u, DALI_REFRESH_ADVANCE);
  TEST_ASSERT_TRUE(dali_refresh_cursor_current(&cursor, 2u, &index));
  TEST_ASSERT_EQUAL_UINT8(1u, index);
  dali_refresh_cursor_complete(&cursor, 2u, DALI_REFRESH_ADVANCE);
  TEST_ASSERT_FALSE(dali_refresh_cursor_current(&cursor, 2u, &index));
}

void test_request_after_completion_starts_new_pass(void) {
  DaliRefreshCursor cursor{};
  uint8_t index = 0xFFu;

  dali_refresh_cursor_request(&cursor);
  TEST_ASSERT_TRUE(dali_refresh_cursor_current(&cursor, 1u, &index));
  dali_refresh_cursor_complete(&cursor, 1u, DALI_REFRESH_ADVANCE);
  TEST_ASSERT_FALSE(dali_refresh_cursor_is_active(&cursor));

  dali_refresh_cursor_request(&cursor);
  TEST_ASSERT_TRUE(dali_refresh_cursor_current(&cursor, 1u, &index));
  TEST_ASSERT_EQUAL_UINT8(0u, index);
}

void test_empty_registry_finishes_request(void) {
  DaliRefreshCursor cursor{};
  uint8_t index = 0xA5u;

  dali_refresh_cursor_request(&cursor);
  TEST_ASSERT_FALSE(dali_refresh_cursor_current(&cursor, 0u, &index));
  TEST_ASSERT_FALSE(dali_refresh_cursor_is_active(&cursor));
  TEST_ASSERT_EQUAL_HEX8(0xA5u, index);
}

void test_mixed_retries_and_skips_do_not_drop_or_duplicate_entries(void) {
  DaliRefreshCursor cursor{};
  const bool eligible[6] = {true, false, true, true, false, true};
  uint8_t attempts[6] = {};
  uint8_t accepted[6] = {};
  uint8_t index = 0xFFu;

  dali_refresh_cursor_request(&cursor);
  while (dali_refresh_cursor_current(&cursor, 6u, &index)) {
    if (!eligible[index]) {
      dali_refresh_cursor_complete(&cursor, 6u, DALI_REFRESH_ADVANCE);
      continue;
    }

    attempts[index]++;
    bool first_backpressure =
        (index == 2u || index == 5u) && attempts[index] == 1u;
    if (first_backpressure) {
      dali_refresh_cursor_complete(&cursor, 6u, DALI_REFRESH_RETRY);
    } else {
      accepted[index]++;
      dali_refresh_cursor_complete(&cursor, 6u, DALI_REFRESH_ADVANCE);
    }
  }

  TEST_ASSERT_EQUAL_UINT8(1u, attempts[0]);
  TEST_ASSERT_EQUAL_UINT8(0u, attempts[1]);
  TEST_ASSERT_EQUAL_UINT8(2u, attempts[2]);
  TEST_ASSERT_EQUAL_UINT8(1u, attempts[3]);
  TEST_ASSERT_EQUAL_UINT8(0u, attempts[4]);
  TEST_ASSERT_EQUAL_UINT8(2u, attempts[5]);
  TEST_ASSERT_EQUAL_UINT8(1u, accepted[0]);
  TEST_ASSERT_EQUAL_UINT8(0u, accepted[1]);
  TEST_ASSERT_EQUAL_UINT8(1u, accepted[2]);
  TEST_ASSERT_EQUAL_UINT8(1u, accepted[3]);
  TEST_ASSERT_EQUAL_UINT8(0u, accepted[4]);
  TEST_ASSERT_EQUAL_UINT8(1u, accepted[5]);
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_cursor_is_initially_inactive);
  RUN_TEST(test_request_visits_each_entry_once);
  RUN_TEST(test_deferred_entry_keeps_same_index);
  RUN_TEST(test_advance_can_skip_ineligible_entries);
  RUN_TEST(test_overlapping_requests_coalesce_into_one_rerun);
  RUN_TEST(test_request_after_completion_starts_new_pass);
  RUN_TEST(test_empty_registry_finishes_request);
  RUN_TEST(test_mixed_retries_and_skips_do_not_drop_or_duplicate_entries);

  return UNITY_END();
}

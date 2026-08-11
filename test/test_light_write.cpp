#include "unity.h"
#include "../components/dali/dali_light_write.h"

void setUp(void) {}
void tearDown(void) {}

/* ── Basic transmission decisions ────────────────────────────────────────── */

void test_no_request_produces_no_action(void) {
  DaliLightWrite w{};
  uint8_t level = 0xA5u;

  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_IDLE, dali_light_write_next(&w, &level));
  TEST_ASSERT_FALSE(dali_light_write_has_pending(&w));
  TEST_ASSERT_EQUAL_UINT8(0xA5u, level);
}

void test_request_on_sends_level(void) {
  DaliLightWrite w{};
  uint8_t level = 0u;

  dali_light_write_request(&w, true, 128u);
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SEND_LEVEL, dali_light_write_next(&w, &level));
  TEST_ASSERT_EQUAL_UINT8(128u, level);
}

void test_request_off_sends_off(void) {
  DaliLightWrite w{};

  dali_light_write_request(&w, false, 200u);
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SEND_OFF, dali_light_write_next(&w, nullptr));
}

/* ── Deduplication is keyed on confirmed state only ──────────────────────── */

void test_enqueue_alone_does_not_suppress_the_same_command(void) {
  DaliLightWrite w{};
  uint8_t level = 0u;

  dali_light_write_request(&w, true, 100u);
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SEND_LEVEL, dali_light_write_next(&w, &level));
  dali_light_write_sent(&w, true);

  /* Still in flight: the repeat waits rather than being suppressed against a
   * command that has not been transmitted yet. */
  dali_light_write_request(&w, true, 100u);
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_IDLE, dali_light_write_next(&w, &level));
  TEST_ASSERT_TRUE(dali_light_write_has_pending(&w));
}

void test_confirmed_state_suppresses_the_same_command(void) {
  DaliLightWrite w{};
  uint8_t level = 0u;

  dali_light_write_request(&w, true, 100u);
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SEND_LEVEL, dali_light_write_next(&w, &level));
  dali_light_write_sent(&w, true);
  TEST_ASSERT_FALSE(dali_light_write_confirm(&w, true));

  dali_light_write_request(&w, true, 100u);
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SUPPRESS, dali_light_write_next(&w, &level));
  TEST_ASSERT_FALSE(dali_light_write_has_pending(&w));
}

void test_confirmed_off_suppresses_off_regardless_of_level(void) {
  DaliLightWrite w{};

  dali_light_write_request(&w, false, 0u);
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SEND_OFF, dali_light_write_next(&w, nullptr));
  dali_light_write_sent(&w, true);
  dali_light_write_confirm(&w, true);

  /* Level is irrelevant when off; a second OFF is still redundant. */
  dali_light_write_request(&w, false, 254u);
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SUPPRESS, dali_light_write_next(&w, nullptr));
}

void test_confirmed_state_does_not_suppress_a_different_level(void) {
  DaliLightWrite w{};
  uint8_t level = 0u;

  dali_light_write_request(&w, true, 100u);
  dali_light_write_next(&w, &level);
  dali_light_write_sent(&w, true);
  dali_light_write_confirm(&w, true);

  dali_light_write_request(&w, true, 101u);
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SEND_LEVEL, dali_light_write_next(&w, &level));
  TEST_ASSERT_EQUAL_UINT8(101u, level);
}

/* ── Enqueue rejection is transient back-pressure ────────────────────────── */

void test_rejected_enqueue_retains_the_desired_state(void) {
  DaliLightWrite w{};
  uint8_t level = 0u;

  dali_light_write_request(&w, true, 77u);
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SEND_LEVEL, dali_light_write_next(&w, &level));
  dali_light_write_sent(&w, false);

  TEST_ASSERT_TRUE(dali_light_write_has_pending(&w));
  TEST_ASSERT_FALSE(dali_light_write_in_flight(&w));

  /* Retried unchanged on the next pump. */
  level = 0u;
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SEND_LEVEL, dali_light_write_next(&w, &level));
  TEST_ASSERT_EQUAL_UINT8(77u, level);
  dali_light_write_sent(&w, true);
  TEST_ASSERT_TRUE(dali_light_write_in_flight(&w));
}

void test_rejected_enqueue_never_commits_the_cache(void) {
  DaliLightWrite w{};
  uint8_t level = 0u;

  dali_light_write_request(&w, true, 77u);
  dali_light_write_next(&w, &level);
  dali_light_write_sent(&w, false);

  /* A rejected command reached no device, so nothing may be suppressed. */
  dali_light_write_request(&w, true, 77u);
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SEND_LEVEL, dali_light_write_next(&w, &level));
}

/* ── Transmission failure invalidates the cache ──────────────────────────── */

void test_failed_transmission_invalidates_cache_and_rearms(void) {
  DaliLightWrite w{};
  uint8_t level = 0u;

  dali_light_write_request(&w, true, 200u);
  dali_light_write_next(&w, &level);
  dali_light_write_sent(&w, true);

  TEST_ASSERT_TRUE(dali_light_write_confirm(&w, false));
  TEST_ASSERT_FALSE(dali_light_write_in_flight(&w));
  TEST_ASSERT_TRUE(dali_light_write_has_pending(&w));

  level = 0u;
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SEND_LEVEL, dali_light_write_next(&w, &level));
  TEST_ASSERT_EQUAL_UINT8(200u, level);
}

void test_failure_after_a_success_does_not_suppress_the_repeat(void) {
  DaliLightWrite w{};
  uint8_t level = 0u;

  /* Confirmed at 200. */
  dali_light_write_request(&w, true, 200u);
  dali_light_write_next(&w, &level);
  dali_light_write_sent(&w, true);
  dali_light_write_confirm(&w, true);

  /* Move to 50 and lose it on the bus. */
  dali_light_write_request(&w, true, 50u);
  dali_light_write_next(&w, &level);
  dali_light_write_sent(&w, true);
  dali_light_write_confirm(&w, false);

  /* The gear is still at 200, so a repeat of 200 must go out rather than be
   * suppressed against a cache the failed command never established. */
  dali_light_write_request(&w, true, 200u);
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SEND_LEVEL, dali_light_write_next(&w, &level));
  TEST_ASSERT_EQUAL_UINT8(200u, level);
}

void test_tx_retry_budget_is_bounded(void) {
  DaliLightWrite w{};
  uint8_t level = 0u;

  dali_light_write_request(&w, true, 90u);
  for (uint8_t i = 0u; i <= DALI_LIGHT_WRITE_TX_RETRIES; i++) {
    TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SEND_LEVEL, dali_light_write_next(&w, &level));
    dali_light_write_sent(&w, true);
    bool rearmed = dali_light_write_confirm(&w, false);
    TEST_ASSERT_EQUAL(i < DALI_LIGHT_WRITE_TX_RETRIES, rearmed);
  }

  /* Budget spent: no further automatic traffic, but the cache stays invalid so
   * a fresh command from the operator is not deduplicated away. */
  TEST_ASSERT_FALSE(dali_light_write_has_pending(&w));
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_IDLE, dali_light_write_next(&w, &level));

  dali_light_write_request(&w, true, 90u);
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SEND_LEVEL, dali_light_write_next(&w, &level));
}

void test_new_request_replenishes_the_tx_retry_budget(void) {
  DaliLightWrite w{};
  uint8_t level = 0u;

  dali_light_write_request(&w, true, 90u);
  for (uint8_t i = 0u; i <= DALI_LIGHT_WRITE_TX_RETRIES; i++) {
    dali_light_write_next(&w, &level);
    dali_light_write_sent(&w, true);
    dali_light_write_confirm(&w, false);
  }
  TEST_ASSERT_FALSE(dali_light_write_has_pending(&w));

  dali_light_write_request(&w, true, 91u);
  dali_light_write_next(&w, &level);
  dali_light_write_sent(&w, true);
  TEST_ASSERT_TRUE(dali_light_write_confirm(&w, false));
}

void test_newer_desired_state_supersedes_a_failed_retry(void) {
  DaliLightWrite w{};
  uint8_t level = 0u;

  dali_light_write_request(&w, true, 40u);
  dali_light_write_next(&w, &level);
  dali_light_write_sent(&w, true);

  /* Operator moves the light again while the first command is in flight. */
  dali_light_write_request(&w, true, 210u);
  dali_light_write_confirm(&w, false);

  level = 0u;
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SEND_LEVEL, dali_light_write_next(&w, &level));
  TEST_ASSERT_EQUAL_UINT8(210u, level);
}

/* ── Observed bus state ──────────────────────────────────────────────────── */

void test_observed_state_suppresses_a_matching_command(void) {
  DaliLightWrite w{};

  TEST_ASSERT_TRUE(dali_light_write_observe(&w, true, 30u));
  dali_light_write_request(&w, true, 30u);
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SUPPRESS, dali_light_write_next(&w, nullptr));
}

void test_observation_is_ignored_while_a_command_is_in_flight(void) {
  DaliLightWrite w{};
  uint8_t level = 0u;

  dali_light_write_request(&w, true, 150u);
  dali_light_write_next(&w, &level);
  dali_light_write_sent(&w, true);

  /* A readback of the pre-command level must not become the confirmed state. */
  TEST_ASSERT_FALSE(dali_light_write_observe(&w, true, 10u));
  dali_light_write_confirm(&w, true);

  dali_light_write_request(&w, true, 150u);
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SUPPRESS, dali_light_write_next(&w, &level));
}

void test_invalidate_drops_the_cache_only(void) {
  DaliLightWrite w{};
  uint8_t level = 0u;

  dali_light_write_observe(&w, true, 60u);
  dali_light_write_request(&w, true, 60u);
  dali_light_write_invalidate(&w);

  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SEND_LEVEL, dali_light_write_next(&w, &level));
  TEST_ASSERT_EQUAL_UINT8(60u, level);
}

/* ── Argument handling ───────────────────────────────────────────────────── */

void test_null_state_is_rejected_everywhere(void) {
  uint8_t level = 0u;

  dali_light_write_request(nullptr, true, 10u);
  dali_light_write_sent(nullptr, true);
  dali_light_write_invalidate(nullptr);
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_IDLE, dali_light_write_next(nullptr, &level));
  TEST_ASSERT_FALSE(dali_light_write_confirm(nullptr, true));
  TEST_ASSERT_FALSE(dali_light_write_observe(nullptr, true, 10u));
  TEST_ASSERT_FALSE(dali_light_write_has_pending(nullptr));
  TEST_ASSERT_FALSE(dali_light_write_in_flight(nullptr));
}

void test_confirm_without_an_in_flight_command_is_a_noop(void) {
  DaliLightWrite w{};

  TEST_ASSERT_FALSE(dali_light_write_confirm(&w, true));
  TEST_ASSERT_FALSE(w.known_valid);
  TEST_ASSERT_FALSE(dali_light_write_has_pending(&w));
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_no_request_produces_no_action);
  RUN_TEST(test_request_on_sends_level);
  RUN_TEST(test_request_off_sends_off);
  RUN_TEST(test_enqueue_alone_does_not_suppress_the_same_command);
  RUN_TEST(test_confirmed_state_suppresses_the_same_command);
  RUN_TEST(test_confirmed_off_suppresses_off_regardless_of_level);
  RUN_TEST(test_confirmed_state_does_not_suppress_a_different_level);
  RUN_TEST(test_rejected_enqueue_retains_the_desired_state);
  RUN_TEST(test_rejected_enqueue_never_commits_the_cache);
  RUN_TEST(test_failed_transmission_invalidates_cache_and_rearms);
  RUN_TEST(test_failure_after_a_success_does_not_suppress_the_repeat);
  RUN_TEST(test_tx_retry_budget_is_bounded);
  RUN_TEST(test_new_request_replenishes_the_tx_retry_budget);
  RUN_TEST(test_newer_desired_state_supersedes_a_failed_retry);
  RUN_TEST(test_observed_state_suppresses_a_matching_command);
  RUN_TEST(test_observation_is_ignored_while_a_command_is_in_flight);
  RUN_TEST(test_invalidate_drops_the_cache_only);
  RUN_TEST(test_null_state_is_rejected_everywhere);
  RUN_TEST(test_confirm_without_an_in_flight_command_is_a_noop);

  return UNITY_END();
}

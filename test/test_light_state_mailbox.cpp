#include "unity.h"
#include "../esphome/components/dali/light/dali_light_state_mailbox.h"

using esphome::dali::DaliLightStateMailbox;

void setUp(void) {}
void tearDown(void) {}

void test_empty_mailbox_preserves_outputs(void) {
  DaliLightStateMailbox mailbox;
  bool is_on = true;
  uint8_t level = 0xA5u;

  TEST_ASSERT_FALSE(mailbox.take(is_on, level));
  TEST_ASSERT_TRUE(is_on);
  TEST_ASSERT_EQUAL_HEX8(0xA5u, level);
}

void test_mailbox_preserves_coherent_on_state(void) {
  DaliLightStateMailbox mailbox;
  bool is_on = false;
  uint8_t level = 0u;

  mailbox.publish(true, 0x7Eu);
  TEST_ASSERT_TRUE(mailbox.take(is_on, level));
  TEST_ASSERT_TRUE(is_on);
  TEST_ASSERT_EQUAL_HEX8(0x7Eu, level);
  TEST_ASSERT_FALSE(mailbox.take(is_on, level));
}

void test_mailbox_preserves_coherent_off_state(void) {
  DaliLightStateMailbox mailbox;
  bool is_on = true;
  uint8_t level = 0u;

  mailbox.publish(false, 0xFFu);
  TEST_ASSERT_TRUE(mailbox.take(is_on, level));
  TEST_ASSERT_FALSE(is_on);
  TEST_ASSERT_EQUAL_HEX8(0xFFu, level);
}

void test_mailbox_distinguishes_off_zero_from_empty(void) {
  DaliLightStateMailbox mailbox;
  bool is_on = true;
  uint8_t level = 0xFFu;

  mailbox.publish(false, 0u);
  TEST_ASSERT_TRUE(mailbox.take(is_on, level));
  TEST_ASSERT_FALSE(is_on);
  TEST_ASSERT_EQUAL_UINT8(0u, level);
  TEST_ASSERT_FALSE(mailbox.take(is_on, level));
}

void test_new_publish_replaces_older_pending_state(void) {
  DaliLightStateMailbox mailbox;
  bool is_on = false;
  uint8_t level = 0u;

  mailbox.publish(false, 0x11u);
  mailbox.publish(true, 0xE2u);

  TEST_ASSERT_TRUE(mailbox.take(is_on, level));
  TEST_ASSERT_TRUE(is_on);
  TEST_ASSERT_EQUAL_HEX8(0xE2u, level);
  TEST_ASSERT_FALSE(mailbox.take(is_on, level));
}

void test_publish_after_take_remains_pending(void) {
  DaliLightStateMailbox mailbox;
  bool is_on = false;
  uint8_t level = 0u;

  mailbox.publish(true, 0x21u);
  TEST_ASSERT_TRUE(mailbox.take(is_on, level));
  mailbox.publish(false, 0x43u);

  TEST_ASSERT_TRUE(mailbox.take(is_on, level));
  TEST_ASSERT_FALSE(is_on);
  TEST_ASSERT_EQUAL_HEX8(0x43u, level);
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_empty_mailbox_preserves_outputs);
  RUN_TEST(test_mailbox_preserves_coherent_on_state);
  RUN_TEST(test_mailbox_preserves_coherent_off_state);
  RUN_TEST(test_mailbox_distinguishes_off_zero_from_empty);
  RUN_TEST(test_new_publish_replaces_older_pending_state);
  RUN_TEST(test_publish_after_take_remains_pending);

  return UNITY_END();
}

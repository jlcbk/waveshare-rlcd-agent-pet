#include <unity.h>

#include "poll_policy.h"

#include "app_config.h"

void setUp(void) {}
void tearDown(void) {}

// 1. Egg fast poll: fastPoll wins over everything (failStreak, busy, idle).
void test_fast_poll_always_wins(void) {
  TEST_ASSERT_EQUAL_UINT32(kFastPollMs,
                           pollIntervalMs(true, false, true, 0, 0));
  TEST_ASSERT_EQUAL_UINT32(kFastPollMs,
                           pollIntervalMs(true, false, true, 59999, 0));
  TEST_ASSERT_EQUAL_UINT32(kFastPollMs,
                           pollIntervalMs(true, false, true, 60001, 0));
  // Even with failures and a busy agent the egg window stays fast.
  TEST_ASSERT_EQUAL_UINT32(kFastPollMs,
                           pollIntervalMs(true, true, true, 0, 0));
  TEST_ASSERT_EQUAL_UINT32(kFastPollMs,
                           pollIntervalMs(true, true, true, 0, 1));
  TEST_ASSERT_EQUAL_UINT32(kFastPollMs,
                           pollIntervalMs(true, false, false, 123456, 3));
  TEST_ASSERT_EQUAL_UINT32(kFastPollMs,
                           pollIntervalMs(true, true, true, 4294967295U, 10));
}

// 2. Idle fallback: during the active hold we poll at the bridge rate; past
// it the interval falls back to the idle rate.
void test_idle_fallback_after_active_hold(void) {
  TEST_ASSERT_EQUAL_UINT32(kBridgePollMs,
                           pollIntervalMs(false, false, true, 0, 0));
  TEST_ASSERT_EQUAL_UINT32(kBridgePollMs,
                           pollIntervalMs(false, false, true, 59999, 0));
  TEST_ASSERT_EQUAL_UINT32(kIdlePollMs,
                           pollIntervalMs(false, false, true, 60001, 0));
  TEST_ASSERT_EQUAL_UINT32(kIdlePollMs,
                           pollIntervalMs(false, false, true, 600000, 0));
}

// 2b. Spec: the active hold is inclusive at exactly kActiveHoldMs, i.e. 60000
// ms after the last agent change must still poll at the bridge rate.
// KNOWN SOURCE BUG: poll_policy.cpp uses a strict "<" comparison, so 60000
// returns kIdlePollMs (10000) instead of kBridgePollMs (3000). Source is not
// modified here; this test documents the spec and stays red until fixed.
void test_active_hold_includes_60000_boundary(void) {
  TEST_ASSERT_EQUAL_UINT32(kBridgePollMs,
                           pollIntervalMs(false, false, true, kActiveHoldMs, 0));
}

// 3. Active period: a busy agent, or no baseline yet (fresh boot / baseline
// lost), keeps the bridge poll at the active rate regardless of elapsed time.
void test_busy_agent_keeps_active_rate(void) {
  TEST_ASSERT_EQUAL_UINT32(kBridgePollMs,
                           pollIntervalMs(false, true, true, 0, 0));
  TEST_ASSERT_EQUAL_UINT32(kBridgePollMs,
                           pollIntervalMs(false, true, true, 30000, 0));
  TEST_ASSERT_EQUAL_UINT32(kBridgePollMs,
                           pollIntervalMs(false, true, true, 3600000, 0));
}

void test_no_baseline_keeps_active_rate(void) {
  // Huge msSinceAgentChange simulates "never changed" (zero baseline).
  TEST_ASSERT_EQUAL_UINT32(kBridgePollMs,
                           pollIntervalMs(false, false, false, 4294967295U, 0));
  TEST_ASSERT_EQUAL_UINT32(kBridgePollMs,
                           pollIntervalMs(false, false, false, 0, 0));
  // Busy plus no baseline together still just the active rate.
  TEST_ASSERT_EQUAL_UINT32(kBridgePollMs,
                           pollIntervalMs(false, true, false, 4294967295U, 0));
}

// 4. Failure ladder: first failure retries fast; sustained failures back off
// linearly (2 s + 3 s per extra failure) and are bounded by the idle interval.
void test_first_failure_retries_fast_regardless_of_state(void) {
  TEST_ASSERT_EQUAL_UINT32(kPollRetryMs,
                           pollIntervalMs(false, false, true, 0, 1));
  TEST_ASSERT_EQUAL_UINT32(kPollRetryMs,
                           pollIntervalMs(false, false, true, 60001, 1));
  TEST_ASSERT_EQUAL_UINT32(kPollRetryMs,
                           pollIntervalMs(false, true, true, 0, 1));
  TEST_ASSERT_EQUAL_UINT32(kPollRetryMs,
                           pollIntervalMs(false, false, false, 0, 1));
}

void test_failure_backoff_ladder(void) {
  TEST_ASSERT_EQUAL_UINT32(8000,
                           pollIntervalMs(false, false, true, 60001, 2));
  TEST_ASSERT_EQUAL_UINT32(kIdlePollMs,
                           pollIntervalMs(false, false, true, 60001, 3));  // min(10000, 11000)
  TEST_ASSERT_EQUAL_UINT32(kIdlePollMs,
                           pollIntervalMs(false, false, true, 60001, 4));
  TEST_ASSERT_EQUAL_UINT32(kIdlePollMs,
                           pollIntervalMs(false, false, true, 60001, 100));
}

// 5. Failures take priority over the active/busy decision.
void test_failure_beats_active_rate(void) {
  TEST_ASSERT_EQUAL_UINT32(kPollRetryMs,
                           pollIntervalMs(false, true, true, 0, 1));
  TEST_ASSERT_EQUAL_UINT32(kPollRetryMs,
                           pollIntervalMs(false, true, true, 600000, 1));
}

// 6. millis() rollover equivalence: callers compute msSinceAgentChange with
// unsigned subtraction, so now - lastChange is correct across the 49.7-day
// wrap point of uint32_t.
void test_millis_rollover_arithmetic(void) {
  const uint32_t lastChange = 0xFFFFFFF0U;
  const uint32_t now = 0x10;
  TEST_ASSERT_EQUAL_UINT32(32, static_cast<uint32_t>(now - lastChange));
  // 32 ms since the last change -> well inside the active hold -> 3 s poll.
  TEST_ASSERT_EQUAL_UINT32(kBridgePollMs,
                           pollIntervalMs(false, false, true, 32, 0));
  // And with the rollover arithmetic inlined, same answer.
  TEST_ASSERT_EQUAL_UINT32(
      kBridgePollMs,
      pollIntervalMs(false, false, true,
                     static_cast<uint32_t>(now - lastChange), 0));
}

// 7. Pin the policy constants and the backoff formula so a constant tweak
// cannot silently change the tested behaviour.
void test_constants_and_backoff_formula(void) {
  TEST_ASSERT_EQUAL_UINT32(3000, kBridgePollMs);
  TEST_ASSERT_EQUAL_UINT32(10000, kIdlePollMs);
  TEST_ASSERT_EQUAL_UINT32(60000, kActiveHoldMs);
  TEST_ASSERT_EQUAL_UINT32(600, kFastPollMs);
  TEST_ASSERT_EQUAL_UINT32(2000, kPollRetryMs);
  // failStreak=2 -> 2000 + 2*3000 = 8000 (still below the idle cap).
  TEST_ASSERT_EQUAL_UINT32(8000, kPollRetryMs + 2 * 3000);
  // Backoff is bounded by the idle interval.
  TEST_ASSERT_EQUAL_UINT32(kIdlePollMs, kPollRetryMs + 3 * 3000 < kIdlePollMs
                                             ? kPollRetryMs + 3 * 3000
                                             : kIdlePollMs);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_fast_poll_always_wins);
  RUN_TEST(test_idle_fallback_after_active_hold);
  RUN_TEST(test_active_hold_includes_60000_boundary);
  RUN_TEST(test_busy_agent_keeps_active_rate);
  RUN_TEST(test_no_baseline_keeps_active_rate);
  RUN_TEST(test_first_failure_retries_fast_regardless_of_state);
  RUN_TEST(test_failure_backoff_ladder);
  RUN_TEST(test_failure_beats_active_rate);
  RUN_TEST(test_millis_rollover_arithmetic);
  RUN_TEST(test_constants_and_backoff_formula);
  return UNITY_END();
}

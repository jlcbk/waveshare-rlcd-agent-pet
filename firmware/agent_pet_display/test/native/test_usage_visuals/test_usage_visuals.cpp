#include <unity.h>

#include "usage_visuals.h"

void setUp(void) {}

void tearDown(void) {}

void test_parse_quota_usage_extracts_five_hour_and_week_values(void) {
  const QuotaUsage usage = parseQuotaUsage("5H 68% WK 88%");

  TEST_ASSERT_EQUAL(68, usage.fiveHourPercent);
  TEST_ASSERT_EQUAL(88, usage.weekPercent);
  TEST_ASSERT_EQUAL_STRING("", usage.fiveHourReset.c_str());
  TEST_ASSERT_EQUAL_STRING("", usage.weekReset.c_str());

  const QuotaUsage withReset = parseQuotaUsage("GEM WK 94% (4d11h) 5H 85% (3h18m) | 3P WK 100%");
  TEST_ASSERT_EQUAL(85, withReset.fiveHourPercent);
  TEST_ASSERT_EQUAL(94, withReset.weekPercent);
  TEST_ASSERT_EQUAL_STRING("3h18m", withReset.fiveHourReset.c_str());
  TEST_ASSERT_EQUAL_STRING("4d11h", withReset.weekReset.c_str());
}

void test_parse_quota_usage_handles_missing_values(void) {
  const QuotaUsage usage = parseQuotaUsage("--");

  TEST_ASSERT_EQUAL(-1, usage.fiveHourPercent);
  TEST_ASSERT_EQUAL(-1, usage.weekPercent);
}

// Home-page unit expansion: a trailing " tok" becomes " Tokens"; anything
// else passes through byte-for-byte.
void test_expand_today_token_unit_expands_trailing_tok(void) {
  TEST_ASSERT_EQUAL_STRING("49M Tokens", expandTodayTokenUnit("49M tok").c_str());
  TEST_ASSERT_EQUAL_STRING("1.2M Tokens", expandTodayTokenUnit("1.2M tok").c_str());
  TEST_ASSERT_EQUAL_STRING("812k Tokens", expandTodayTokenUnit("812k tok").c_str());
}

void test_expand_today_token_unit_leaves_other_values_untouched(void) {
  TEST_ASSERT_EQUAL_STRING("49M", expandTodayTokenUnit("49M").c_str());
  TEST_ASSERT_EQUAL_STRING("12%", expandTodayTokenUnit("12%").c_str());
  // "tok" not at the end (e.g. a rate) must not be rewritten.
  TEST_ASSERT_EQUAL_STRING("49M tok/min", expandTodayTokenUnit("49M tok/min").c_str());
  TEST_ASSERT_EQUAL_STRING("--", expandTodayTokenUnit("--").c_str());
  TEST_ASSERT_EQUAL_STRING("", expandTodayTokenUnit("").c_str());
  // Already spelled out or different casing: no double expansion.
  TEST_ASSERT_EQUAL_STRING("49M Tokens", expandTodayTokenUnit("49M Tokens").c_str());
  TEST_ASSERT_EQUAL_STRING("49M TOK", expandTodayTokenUnit("49M TOK").c_str());
}

void test_strip_today_token_unit_removes_trailing_token_unit(void) {
  TEST_ASSERT_EQUAL_STRING("2.2M", stripTodayTokenUnit("2.2M tok").c_str());
  TEST_ASSERT_EQUAL_STRING("1.9M", stripTodayTokenUnit("1.9M Tokens").c_str());
  TEST_ASSERT_EQUAL_STRING("812k", stripTodayTokenUnit("812k Token").c_str());
  TEST_ASSERT_EQUAL_STRING("49M", stripTodayTokenUnit("49M").c_str());
  TEST_ASSERT_EQUAL_STRING("49M tok/min", stripTodayTokenUnit("49M tok/min").c_str());
  TEST_ASSERT_EQUAL_STRING("--", stripTodayTokenUnit("--").c_str());
}

void test_parse_context_usage_percent_calculates_ratio(void) {
  TEST_ASSERT_EQUAL(72, parseContextUsagePercent("185k / 256k"));
  TEST_ASSERT_EQUAL(24, parseContextUsagePercent("60.8k / 256k"));
  TEST_ASSERT_EQUAL(60, parseContextUsagePercent("1.2M / 2M"));
  TEST_ASSERT_EQUAL(-1, parseContextUsagePercent("--"));
  TEST_ASSERT_EQUAL(-1, parseContextUsagePercent(""));
}

void test_quota_usage_same_ignoring_countdown_ignores_minute_ticks(void) {
  TEST_ASSERT_TRUE(quotaUsageSameIgnoringCountdown(
      "GEM WK 94% (6d14h) 5H 79% (22m) | 3P WK 100%",
      "GEM WK 94% (6d14h) 5H 79% (21m) | 3P WK 100%"));
  TEST_ASSERT_FALSE(quotaUsageSameIgnoringCountdown(
      "GEM WK 94% (6d14h) 5H 79% (22m) | 3P WK 100%",
      "GEM WK 94% (6d14h) 5H 78% (21m) | 3P WK 100%"));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_quota_usage_extracts_five_hour_and_week_values);
  RUN_TEST(test_parse_quota_usage_handles_missing_values);
  RUN_TEST(test_expand_today_token_unit_expands_trailing_tok);
  RUN_TEST(test_expand_today_token_unit_leaves_other_values_untouched);
  RUN_TEST(test_strip_today_token_unit_removes_trailing_token_unit);
  RUN_TEST(test_parse_context_usage_percent_calculates_ratio);
  RUN_TEST(test_quota_usage_same_ignoring_countdown_ignores_minute_ticks);
  return UNITY_END();
}

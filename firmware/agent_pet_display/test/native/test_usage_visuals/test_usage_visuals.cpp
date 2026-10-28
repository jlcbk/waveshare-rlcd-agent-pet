#include <unity.h>

#include "usage_visuals.h"

void setUp(void) {}

void tearDown(void) {}

void test_parse_quota_usage_extracts_five_hour_and_week_values(void) {
  const QuotaUsage usage = parseQuotaUsage("5H 68% WK 88%");

  TEST_ASSERT_EQUAL(68, usage.fiveHourPercent);
  TEST_ASSERT_EQUAL(88, usage.weekPercent);
}

void test_parse_quota_usage_handles_missing_values(void) {
  const QuotaUsage usage = parseQuotaUsage("--");

  TEST_ASSERT_EQUAL(-1, usage.fiveHourPercent);
  TEST_ASSERT_EQUAL(-1, usage.weekPercent);
}

// Home-page unit expansion: a trailing " tok" becomes " Token"; anything
// else passes through byte-for-byte.
void test_expand_today_token_unit_expands_trailing_tok(void) {
  TEST_ASSERT_EQUAL_STRING("49M Token", expandTodayTokenUnit("49M tok").c_str());
  TEST_ASSERT_EQUAL_STRING("1.2M Token", expandTodayTokenUnit("1.2M tok").c_str());
  TEST_ASSERT_EQUAL_STRING("812k Token", expandTodayTokenUnit("812k tok").c_str());
}

void test_expand_today_token_unit_leaves_other_values_untouched(void) {
  TEST_ASSERT_EQUAL_STRING("49M", expandTodayTokenUnit("49M").c_str());
  TEST_ASSERT_EQUAL_STRING("12%", expandTodayTokenUnit("12%").c_str());
  // "tok" not at the end (e.g. a rate) must not be rewritten.
  TEST_ASSERT_EQUAL_STRING("49M tok/min", expandTodayTokenUnit("49M tok/min").c_str());
  TEST_ASSERT_EQUAL_STRING("--", expandTodayTokenUnit("--").c_str());
  TEST_ASSERT_EQUAL_STRING("", expandTodayTokenUnit("").c_str());
  // Already spelled out or different casing: no double expansion.
  TEST_ASSERT_EQUAL_STRING("49M Token", expandTodayTokenUnit("49M Token").c_str());
  TEST_ASSERT_EQUAL_STRING("49M TOK", expandTodayTokenUnit("49M TOK").c_str());
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_quota_usage_extracts_five_hour_and_week_values);
  RUN_TEST(test_parse_quota_usage_handles_missing_values);
  RUN_TEST(test_expand_today_token_unit_expands_trailing_tok);
  RUN_TEST(test_expand_today_token_unit_leaves_other_values_untouched);
  return UNITY_END();
}

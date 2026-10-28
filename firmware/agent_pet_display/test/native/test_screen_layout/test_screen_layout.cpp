#include <unity.h>

#include "screen_layout.h"

void setUp(void) {}
void tearDown(void) {}

// Panel is 400px wide with a 12px page margin on each side, so every Overview
// section spans x=12..388 (w=376) now that the right-hand pet panel is gone.
void test_overview_sections_span_full_width_and_do_not_overlap(void) {
  const ScreenRect sections[] = {overviewTimeCardRect(), overviewStatusCardRect(),
                                 overviewMetricCardRect(0), overviewMetricCardRect(1),
                                 overviewMetricCardRect(2)};
  for (int i = 0; i < 5; ++i) {
    TEST_ASSERT_EQUAL(12, sections[i].x);
    TEST_ASSERT_EQUAL(376, sections[i].w);
    TEST_ASSERT_EQUAL(388, sections[i].x + sections[i].w);
    TEST_ASSERT_TRUE(sections[i].y >= 25);
    TEST_ASSERT_TRUE(sections[i].y + sections[i].h <= 272);
    if (i > 0) TEST_ASSERT_TRUE(sections[i - 1].y + sections[i - 1].h <= sections[i].y);
  }
}

// Metric band keeps its previous y/height rhythm; only the width changed.
void test_metric_cards_keep_vertical_rhythm_and_clamp_index(void) {
  const int expectedY[3] = {156, 191, 227};
  const int expectedH[3] = {33, 33, 41};
  for (int i = 0; i < 3; ++i) {
    const ScreenRect card = overviewMetricCardRect(i);
    TEST_ASSERT_EQUAL(expectedY[i], card.y);
    TEST_ASSERT_EQUAL(expectedH[i], card.h);
  }
  // Out-of-range indices clamp to the first/last card instead of reading
  // past the array.
  const ScreenRect first = overviewMetricCardRect(-1);
  const ScreenRect last = overviewMetricCardRect(3);
  TEST_ASSERT_EQUAL(overviewMetricCardRect(0).y, first.y);
  TEST_ASSERT_EQUAL(overviewMetricCardRect(2).y, last.y);
}

void test_quota_bars_are_separate_and_fit_the_quota_section(void) {
  const ScreenRect quota = overviewMetricCardRect(2);  // {12, 227, 376, 41}
  const QuotaBarLayout bars = overviewQuotaBarLayout();
  // Geometry derivation: the "QUOTA" label column keeps 70px on the left
  // (label clips at 66px), so bars start at quota.x + 70. The remaining
  // 376 - 70 - 12 = 294px splits into two 135px bars separated by a 24px
  // gap; the second bar ends at quota.x + 364, preserving the 12px inner
  // margin used by the page edges. Text ("5H n%" / "WK n%") draws above
  // each bar at helvB12 and measures well under 135px.
  TEST_ASSERT_EQUAL(quota.x + 70, bars.fiveHourBar.x);
  TEST_ASSERT_EQUAL(quota.y + 26, bars.fiveHourBar.y);
  TEST_ASSERT_EQUAL(135, bars.fiveHourBar.w);
  TEST_ASSERT_EQUAL(4, bars.fiveHourBar.h);
  TEST_ASSERT_EQUAL(quota.x + 229, bars.weekBar.x);
  TEST_ASSERT_EQUAL(quota.y + 26, bars.weekBar.y);
  TEST_ASSERT_EQUAL(135, bars.weekBar.w);
  TEST_ASSERT_EQUAL(4, bars.weekBar.h);
  // Behavioural invariants: separate, ordered, inside the quota card.
  TEST_ASSERT_TRUE(bars.fiveHourBar.x >= quota.x);
  TEST_ASSERT_TRUE(bars.fiveHourBar.x + bars.fiveHourBar.w < bars.weekBar.x);
  TEST_ASSERT_TRUE(bars.weekBar.x + bars.weekBar.w <= quota.x + quota.w);
  TEST_ASSERT_TRUE(bars.fiveHourBar.y >= quota.y);
  TEST_ASSERT_TRUE(bars.weekBar.y + bars.weekBar.h <= quota.y + quota.h);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_overview_sections_span_full_width_and_do_not_overlap);
  RUN_TEST(test_metric_cards_keep_vertical_rhythm_and_clamp_index);
  RUN_TEST(test_quota_bars_are_separate_and_fit_the_quota_section);
  return UNITY_END();
}

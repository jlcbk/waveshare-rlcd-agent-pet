#include <unity.h>

#include "screen_layout.h"

void setUp(void) {}
void tearDown(void) {}

void test_overview_sections_fit_and_do_not_overlap(void) {
  const ScreenRect sections[] = {overviewTimeCardRect(), overviewStatusCardRect(),
                                 overviewMetricCardRect(0), overviewMetricCardRect(1),
                                 overviewMetricCardRect(2)};
  for (int i = 0; i < 5; ++i) {
    TEST_ASSERT_EQUAL(12, sections[i].x);
    TEST_ASSERT_TRUE(sections[i].y >= 25);
    TEST_ASSERT_TRUE(sections[i].y + sections[i].h <= 272);
    if (i > 0) TEST_ASSERT_TRUE(sections[i - 1].y + sections[i - 1].h <= sections[i].y);
  }
  // The three metric cards form the left column next to the pet panel.
  for (int i = 0; i < 3; ++i) {
    TEST_ASSERT_EQUAL(256, sections[2 + i].w);
  }
}

void test_overview_pet_panel_sits_right_of_metrics(void) {
  const ScreenRect pet = overviewPetPanelRect();
  const ScreenRect firstMetric = overviewMetricCardRect(0);
  const ScreenRect lastMetric = overviewMetricCardRect(2);
  // Right column, clear of the 256px metric column, spanning the metric band.
  TEST_ASSERT_TRUE(pet.x >= firstMetric.x + firstMetric.w + 8);
  TEST_ASSERT_TRUE(pet.x + pet.w <= 388);
  TEST_ASSERT_TRUE(pet.y >= firstMetric.y);
  TEST_ASSERT_TRUE(pet.y + pet.h <= 272);
  TEST_ASSERT_TRUE(lastMetric.y + lastMetric.h <= 272);
  // Room for the 64x64 sprite plus a bubble strip.
  TEST_ASSERT_TRUE(pet.w >= 64 && pet.h >= 64 + 24);
}

void test_quota_bars_are_separate_and_fit_the_quota_section(void) {
  const ScreenRect quota = overviewMetricCardRect(2);
  const QuotaBarLayout bars = overviewQuotaBarLayout();
  TEST_ASSERT_TRUE(bars.fiveHourBar.x >= quota.x);
  TEST_ASSERT_TRUE(bars.fiveHourBar.x + bars.fiveHourBar.w < bars.weekBar.x);
  TEST_ASSERT_TRUE(bars.weekBar.x + bars.weekBar.w <= quota.x + quota.w);
  TEST_ASSERT_TRUE(bars.fiveHourBar.y >= quota.y);
  TEST_ASSERT_TRUE(bars.weekBar.y + bars.weekBar.h <= quota.y + quota.h);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_overview_sections_fit_and_do_not_overlap);
  RUN_TEST(test_overview_pet_panel_sits_right_of_metrics);
  RUN_TEST(test_quota_bars_are_separate_and_fit_the_quota_section);
  return UNITY_END();
}

#include <unity.h>

#include "pm_controller.h"

#include <stdint.h>

namespace {

constexpr int kFreqMhz = 80;              // idle CPU clock
constexpr int64_t kUsPerSecond = 1000000;

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// 1. Fully awake: the CPU runs at 80 cycles/us for the whole 30 s window.
// Feed one sample per simulated second; awakePercent must read 100.
void test_fully_awake_reports_100_percent(void) {
  AwakeStats stats;
  stats.begin(0, 0);
  uint32_t cycles = 0;
  for (int second = 1; second <= 30; ++second) {
    // 1 s awake at 80 MHz: cycles advance with the wall clock.
    cycles += static_cast<uint32_t>(80ULL * kUsPerSecond);
    stats.sample(cycles, static_cast<int64_t>(second) * kUsPerSecond);
  }
  TEST_ASSERT_EQUAL_INT64(30 * kUsPerSecond, stats.windowUs());
  TEST_ASSERT_EQUAL_INT64(30 * kUsPerSecond, stats.awakeUs(kFreqMhz));
  TEST_ASSERT_EQUAL_INT(100, stats.awakePercent(kFreqMhz));
}

// 2. 90% asleep: wall clock advances through the full 30 s but the cycle
// counter only moves during the awake 10% (sleep stops CCOUNT and no samples
// are taken). awakePercent must read 10 (+/- 1 tolerance).
void test_ninety_percent_asleep_reports_10_percent(void) {
  AwakeStats stats;
  stats.begin(0, 0);
  uint32_t cycles = 0;
  for (int second = 1; second <= 30; ++second) {
    // Wall clock always advances 1 s; the CPU only stays awake 100 ms of it.
    cycles += static_cast<uint32_t>(80ULL * kUsPerSecond / 10);
    stats.sample(cycles, static_cast<int64_t>(second) * kUsPerSecond);
  }
  TEST_ASSERT_EQUAL_INT64(30 * kUsPerSecond, stats.windowUs());
  TEST_ASSERT_EQUAL_INT64(3 * kUsPerSecond, stats.awakeUs(kFreqMhz));
  const int percent = stats.awakePercent(kFreqMhz);
  TEST_ASSERT_TRUE(percent >= 9 && percent <= 11);
  TEST_ASSERT_EQUAL_INT(10, percent);
}

// 3. CCOUNT wrap: the 32-bit cycle counter wraps at 2^32; the wrap-safe delta
// must keep accumulating across the wrap point.
void test_cycle_counter_wrap_is_transparent(void) {
  AwakeStats stats;
  const uint32_t start = 0xFFFFF000U;  // 4096 cycles below the wrap point
  stats.begin(start, 0);
  // First sample stays just below the wrap point.
  const uint32_t preWrapDelta = 2000;
  const uint32_t preWrap = start + preWrapDelta;  // no wrap yet
  TEST_ASSERT_TRUE(preWrap > start);
  stats.sample(preWrap, 5 * kUsPerSecond);
  // Second sample advances 1 s worth of awake cycles across the wrap
  // (unsigned arithmetic wraps exactly like the hardware counter).
  const uint32_t totalAwake = static_cast<uint32_t>(80ULL * kUsPerSecond);
  const uint32_t postWrap = preWrap + (totalAwake - preWrapDelta);
  TEST_ASSERT_TRUE(postWrap < preWrap);  // sanity: we did wrap
  stats.sample(postWrap, 10 * kUsPerSecond);

  TEST_ASSERT_EQUAL_INT64(10 * kUsPerSecond, stats.windowUs());
  TEST_ASSERT_EQUAL_INT64(totalAwake / kFreqMhz, stats.awakeUs(kFreqMhz));
  // 1 s awake in a 10 s window -> 10%.
  const int percent = stats.awakePercent(kFreqMhz);
  TEST_ASSERT_TRUE(percent >= 9 && percent <= 11);
  TEST_ASSERT_EQUAL_INT(10, percent);
}

// 4. Zero / negative boundaries: right after begin the window is empty and
// there is no meaningful percentage; a degenerate frequency reports 0 awake.
void test_empty_window_boundaries(void) {
  AwakeStats stats;
  stats.begin(12345, 1000000);
  TEST_ASSERT_EQUAL_INT64(0, stats.windowUs());
  TEST_ASSERT_EQUAL_INT(-1, stats.awakePercent(kFreqMhz));
  TEST_ASSERT_EQUAL_INT64(0, stats.awakeUs(kFreqMhz));
  // Degenerate frequency is rejected, not divided by.
  TEST_ASSERT_EQUAL_INT64(0, stats.awakeUs(0));
  TEST_ASSERT_EQUAL_INT64(0, stats.awakeUs(-80));
  // Wall clock moving backwards makes the window negative -> still -1.
  stats.sample(12345, 500000);
  TEST_ASSERT_EQUAL_INT64(-500000, stats.windowUs());
  TEST_ASSERT_EQUAL_INT(-1, stats.awakePercent(kFreqMhz));
}

// 5. resetWindow clears the accumulator and restarts the window from the
// given sample, so later readings only reflect the new window.
void test_reset_window_clears_accumulation(void) {
  AwakeStats stats;
  stats.begin(0, 0);
  // 10 s fully awake -> 100%.
  stats.sample(static_cast<uint32_t>(80ULL * kUsPerSecond * 10ULL),
               10 * kUsPerSecond);
  TEST_ASSERT_EQUAL_INT(100, stats.awakePercent(kFreqMhz));

  // Reset at the current sample: window restarts empty.
  stats.resetWindow(static_cast<uint32_t>(80ULL * kUsPerSecond * 10ULL),
                    10 * kUsPerSecond);
  TEST_ASSERT_EQUAL_INT64(0, stats.windowUs());
  TEST_ASSERT_EQUAL_INT(-1, stats.awakePercent(kFreqMhz));

  // New window: 20 s wall, only 2 s worth of awake cycles -> 10%.
  const uint32_t base = static_cast<uint32_t>(80ULL * kUsPerSecond * 10ULL);
  stats.sample(base + static_cast<uint32_t>(80ULL * kUsPerSecond * 2ULL),
               30 * kUsPerSecond);
  TEST_ASSERT_EQUAL_INT64(20 * kUsPerSecond, stats.windowUs());
  TEST_ASSERT_EQUAL_INT64(2 * kUsPerSecond, stats.awakeUs(kFreqMhz));
  TEST_ASSERT_EQUAL_INT(10, stats.awakePercent(kFreqMhz));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_fully_awake_reports_100_percent);
  RUN_TEST(test_ninety_percent_asleep_reports_10_percent);
  RUN_TEST(test_cycle_counter_wrap_is_transparent);
  RUN_TEST(test_empty_window_boundaries);
  RUN_TEST(test_reset_window_clears_accumulation);
  return UNITY_END();
}

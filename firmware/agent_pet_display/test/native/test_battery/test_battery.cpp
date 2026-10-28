#include <unity.h>

#include "battery_monitor.h"

void setUp(void) {}

void tearDown(void) {}

void test_full_voltage_maps_to_100_percent(void) {
  TEST_ASSERT_EQUAL_INT(100, mapBatteryPercent(4200));
  TEST_ASSERT_EQUAL_INT(100, mapBatteryPercent(4100));
}

// The usable-voltage display window is 3.60-4.10 V: everything at or below
// the floor is 0%, including every invalid 0/negative reading.
void test_below_window_clamps_to_zero(void) {
  TEST_ASSERT_EQUAL_INT(0, mapBatteryPercent(3599));
  TEST_ASSERT_EQUAL_INT(0, mapBatteryPercent(3600));
  TEST_ASSERT_EQUAL_INT(0, mapBatteryPercent(3300));
  TEST_ASSERT_EQUAL_INT(0, mapBatteryPercent(0));
}

// Anchor points exactly as derived from the NCR18650B median curve
// (tools/battery_curve/derive_points.py, rounded to whole percent).
void test_curve_anchor_points(void) {
  TEST_ASSERT_EQUAL_INT(15, mapBatteryPercent(3650));
  TEST_ASSERT_EQUAL_INT(24, mapBatteryPercent(3700));
  TEST_ASSERT_EQUAL_INT(32, mapBatteryPercent(3750));
  TEST_ASSERT_EQUAL_INT(42, mapBatteryPercent(3800));
  TEST_ASSERT_EQUAL_INT(53, mapBatteryPercent(3850));
  TEST_ASSERT_EQUAL_INT(64, mapBatteryPercent(3900));
  TEST_ASSERT_EQUAL_INT(73, mapBatteryPercent(3950));
  TEST_ASSERT_EQUAL_INT(82, mapBatteryPercent(4000));
  TEST_ASSERT_EQUAL_INT(90, mapBatteryPercent(4050));
}

// Midpoints exercise the integer piecewise-linear interpolation exactly as
// the firmware computes it (products first, then one integer divide).
void test_integer_interpolation_midpoints(void) {
  TEST_ASSERT_EQUAL_INT(7, mapBatteryPercent(3625));   // 0 + 25*15/50
  TEST_ASSERT_EQUAL_INT(28, mapBatteryPercent(3725));  // 24 + 25*8/50
  TEST_ASSERT_EQUAL_INT(58, mapBatteryPercent(3875));  // 53 + 25*11/50
  TEST_ASSERT_EQUAL_INT(77, mapBatteryPercent(3975));  // 73 + 25*9/50
  TEST_ASSERT_EQUAL_INT(95, mapBatteryPercent(4075));  // 90 + 25*10/50
}

void test_percent_is_monotonic_across_window(void) {
  int previous = mapBatteryPercent(3500);
  for (int mv = 3505; mv <= 4200; mv += 5) {
    const int percent = mapBatteryPercent(mv);
    TEST_ASSERT_GREATER_OR_EQUAL(0, percent);  // bounds stay sane
    TEST_ASSERT_LESS_OR_EQUAL(100, percent);
    TEST_ASSERT_GREATER_OR_EQUAL(previous, percent);
    previous = percent;
  }
  TEST_ASSERT_EQUAL_INT(100, previous);
}

// Percent is display-only: an invalid sample stays invalid (never a percent
// of 0 read as a measurement), and protection never sees this value.
void test_invalid_sample_stays_invalid(void) {
  const PowerState power = derivePowerState(0, false);
  TEST_ASSERT_FALSE(power.sampleOk);
  TEST_ASSERT_EQUAL_INT(0, power.percent);
}

void test_low_voltage_sets_low_battery(void) {
  const PowerState power = derivePowerState(3450, false);

  TEST_ASSERT_TRUE(power.lowBattery);
}

void test_charging_shows_detail(void) {
  const PowerState power = derivePowerState(3900, true);

  TEST_ASSERT_TRUE(power.charging);
  TEST_ASSERT_TRUE(shouldShowBatteryDetail(power));
}

void test_voltage_formats_to_two_decimals(void) {
  TEST_ASSERT_EQUAL_STRING("4.09V", formatBatteryVoltage(4095).c_str());
  TEST_ASSERT_EQUAL_STRING("3.60V", formatBatteryVoltage(3600).c_str());
  TEST_ASSERT_EQUAL_STRING("4.20V", formatBatteryVoltage(4200).c_str());
}

void test_voltage_format_invalid_never_shows_zero_volts(void) {
  // A failed ADC read is 0 mV: it must render as missing data, not "0.00V"
  // (which would read as a healthy-ish flat battery, not a broken sensor).
  TEST_ASSERT_EQUAL_STRING("-- V", formatBatteryVoltage(0).c_str());
  TEST_ASSERT_EQUAL_STRING("-- V", formatBatteryVoltage(-1).c_str());
}

void test_percent_clamps_below_display_window(void) {
  // 3599 mV displays as 0% under the 3.60 V window floor, yet the raw
  // millivolt value still drives the protection guard independently.
  const PowerState power = derivePowerState(3599, false);
  TEST_ASSERT_EQUAL_INT(0, power.percent);
  TEST_ASSERT_EQUAL_INT(3599, power.voltageMv);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_full_voltage_maps_to_100_percent);
  RUN_TEST(test_below_window_clamps_to_zero);
  RUN_TEST(test_curve_anchor_points);
  RUN_TEST(test_integer_interpolation_midpoints);
  RUN_TEST(test_percent_is_monotonic_across_window);
  RUN_TEST(test_invalid_sample_stays_invalid);
  RUN_TEST(test_low_voltage_sets_low_battery);
  RUN_TEST(test_charging_shows_detail);
  RUN_TEST(test_voltage_formats_to_two_decimals);
  RUN_TEST(test_voltage_format_invalid_never_shows_zero_volts);
  RUN_TEST(test_percent_clamps_below_display_window);
  return UNITY_END();
}

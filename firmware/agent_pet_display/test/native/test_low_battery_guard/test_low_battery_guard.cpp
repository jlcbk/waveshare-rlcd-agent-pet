#include <unity.h>

#include "app_config.h"
#include "battery_monitor.h"
#include "low_battery_guard.h"

void setUp(void) {}

void tearDown(void) {}

// Nine strictly-low valid samples must not trigger; the tenth must.
void test_ninth_low_sample_does_not_protect(void) {
  LowBatteryGuard guard;
  for (int i = 0; i < 9; ++i) {
    TEST_ASSERT_EQUAL(LowBatteryAction::None, guard.feed(true, 3550));
  }
  TEST_ASSERT_EQUAL_INT(9, guard.consecutiveLow());
}

void test_tenth_low_sample_enters_protection(void) {
  LowBatteryGuard guard;
  for (int i = 0; i < 9; ++i) {
    guard.feed(true, 3599);
  }
  TEST_ASSERT_EQUAL(LowBatteryAction::EnterProtection, guard.feed(true, 3599));
  TEST_ASSERT_EQUAL_INT(10, guard.consecutiveLow());
}

// Exactly 3.60 V is NOT low: the threshold is strict.
void test_exactly_3600mv_is_recovered(void) {
  LowBatteryGuard guard;
  for (int i = 0; i < 9; ++i) {
    guard.feed(true, 3550);
  }
  TEST_ASSERT_EQUAL(LowBatteryAction::None, guard.feed(true, 3600));
  TEST_ASSERT_EQUAL_INT(0, guard.consecutiveLow());
  // One recovered sample breaks the streak: protection needs 10 fresh lows.
  TEST_ASSERT_EQUAL(LowBatteryAction::None, guard.feed(true, 3550));
  TEST_ASSERT_EQUAL_INT(1, guard.consecutiveLow());
}

// An invalid sample breaks the streak instead of counting as low.
void test_invalid_sample_resets_and_never_triggers(void) {
  LowBatteryGuard guard;
  for (int i = 0; i < 9; ++i) {
    guard.feed(true, 3550);
  }
  TEST_ASSERT_EQUAL(LowBatteryAction::None, guard.feed(false, 0));
  TEST_ASSERT_EQUAL_INT(0, guard.consecutiveLow());
  for (int i = 0; i < 30; ++i) {
    TEST_ASSERT_EQUAL(LowBatteryAction::None, guard.feed(false, 0));
  }
  TEST_ASSERT_EQUAL_INT(0, guard.consecutiveLow());
}

// Valid readings above the threshold also reset the count.
void test_high_sample_resets(void) {
  LowBatteryGuard guard;
  guard.feed(true, 3590);
  guard.feed(true, 3590);
  guard.feed(true, 4000);
  TEST_ASSERT_EQUAL_INT(0, guard.consecutiveLow());
}

// Guard takes raw millivolts: a 3.599 V reading that would display as
// "3.59V"/"3.60V" still counts as low. Display formatting never feeds back.
void test_3599mv_counts_low_despite_3_60v_display(void) {
  LowBatteryGuard guard;
  TEST_ASSERT_EQUAL(LowBatteryAction::None, guard.feed(true, 3599));
  TEST_ASSERT_EQUAL_INT(1, guard.consecutiveLow());
}

// --- Recovery after a protection wake: ProtectionWakeConfirm ---
// This is the exact decision object batteryProtectionEarlyCheck() feeds.

// Two consecutive valid readings at/above 3750 mV confirm recovery.
void test_two_valid_readings_at_3750_resume(void) {
  ProtectionWakeConfirm confirm;
  TEST_ASSERT_EQUAL(ProtectionWakeAction::KeepSleeping, confirm.feed(true, 3750));
  TEST_ASSERT_EQUAL_INT(1, confirm.confirmedReadings());
  TEST_ASSERT_EQUAL(ProtectionWakeAction::ResumeNormal, confirm.feed(true, 3750));
}

void test_two_valid_readings_above_3750_resume(void) {
  ProtectionWakeConfirm confirm;
  TEST_ASSERT_EQUAL(ProtectionWakeAction::KeepSleeping, confirm.feed(true, 3900));
  TEST_ASSERT_EQUAL(ProtectionWakeAction::ResumeNormal, confirm.feed(true, 4200));
}

// A single reading can never resume, no matter how healthy.
void test_single_reading_never_resumes(void) {
  ProtectionWakeConfirm confirm;
  TEST_ASSERT_EQUAL(ProtectionWakeAction::KeepSleeping, confirm.feed(true, 4200));
  TEST_ASSERT_EQUAL_INT(1, confirm.confirmedReadings());
}

// Strictly below the recovery threshold restarts the count: 3749 mV is
// valid AND above the entry threshold (3.60 V), yet recovery refuses it.
void test_3749mv_resets_confirmation(void) {
  ProtectionWakeConfirm confirm;
  confirm.feed(true, 3750);
  TEST_ASSERT_EQUAL(ProtectionWakeAction::KeepSleeping, confirm.feed(true, 3749));
  TEST_ASSERT_EQUAL_INT(0, confirm.confirmedReadings());
  // The streak restarted: one more good reading is not enough.
  TEST_ASSERT_EQUAL(ProtectionWakeAction::KeepSleeping, confirm.feed(true, 3800));
  TEST_ASSERT_EQUAL(ProtectionWakeAction::ResumeNormal, confirm.feed(true, 3800));
}

// 3599/3600 are the ENTRY boundaries; recovery uses its own 3750 mV line.
void test_3600mv_counts_recovered_for_entry_but_not_for_recovery(void) {
  ProtectionWakeConfirm confirm;
  TEST_ASSERT_EQUAL(ProtectionWakeAction::KeepSleeping, confirm.feed(true, 3600));
  TEST_ASSERT_EQUAL_INT(0, confirm.confirmedReadings());
  TEST_ASSERT_EQUAL(ProtectionWakeAction::KeepSleeping, confirm.feed(true, 3600));
  TEST_ASSERT_EQUAL_INT(0, confirm.confirmedReadings());

  LowBatteryGuard guard;
  TEST_ASSERT_EQUAL(LowBatteryAction::None, guard.feed(true, 3600));
  TEST_ASSERT_EQUAL_INT(0, guard.consecutiveLow());
  TEST_ASSERT_EQUAL(LowBatteryAction::None, guard.feed(true, 3599));
  TEST_ASSERT_EQUAL_INT(1, guard.consecutiveLow());
}

// An invalid reading anywhere in the confirmation keeps protection.
void test_invalid_reading_keeps_protection(void) {
  ProtectionWakeConfirm confirm;
  TEST_ASSERT_EQUAL(ProtectionWakeAction::KeepSleeping, confirm.feed(true, 3800));
  TEST_ASSERT_EQUAL(ProtectionWakeAction::KeepSleeping, confirm.feed(false, 0));
  TEST_ASSERT_EQUAL_INT(0, confirm.confirmedReadings());
}

void test_invalid_then_valid_still_lacks_confirmation(void) {
  ProtectionWakeConfirm confirm;
  TEST_ASSERT_EQUAL(ProtectionWakeAction::KeepSleeping, confirm.feed(false, 0));
  TEST_ASSERT_EQUAL(ProtectionWakeAction::KeepSleeping, confirm.feed(true, 4000));
  TEST_ASSERT_EQUAL_INT(1, confirm.confirmedReadings());
}

void test_all_invalid_readings_keep_protection(void) {
  ProtectionWakeConfirm confirm;
  for (int i = 0; i < 6; ++i) {
    TEST_ASSERT_EQUAL(ProtectionWakeAction::KeepSleeping, confirm.feed(false, 0));
  }
  TEST_ASSERT_EQUAL_INT(0, confirm.confirmedReadings());
}

// The battery percentage NEVER controls protection: 3700 mV displays as a
// healthy-looking 24% under the nonlinear lookup, yet recovery still keeps
// the board asleep because 3700 < 3750.
void test_percent_display_never_controls_recovery(void) {
  TEST_ASSERT_EQUAL_INT(24, mapBatteryPercent(3700));
  ProtectionWakeConfirm confirm;
  TEST_ASSERT_EQUAL(ProtectionWakeAction::KeepSleeping, confirm.feed(true, 3700));
  TEST_ASSERT_EQUAL(ProtectionWakeAction::KeepSleeping, confirm.feed(true, 3700));
  TEST_ASSERT_EQUAL_INT(0, confirm.confirmedReadings());
}

// --- The actual wake call path: protectionWakeStep ---
// batteryProtectionEarlyCheck() samples, calls protectionWakeStep() after
// each reading, and acts: SleepNow after reading 1 = immediate deep sleep
// (no 1 s wait, no second reading); ContinueToSecondReading = the caller
// waits ~1 s and samples again; ResumeNormal = boot. These tests execute
// that exact decision function.

// The regression that motivated this suite: a HEALTHY first reading returns
// KeepSleeping from ProtectionWakeConfirm::feed (it still needs the second
// reading), so the wake path must decide via confirmedReadings(), never via
// feed()'s return alone - otherwise recovery could never happen.
void test_wake_path_healthy_first_reading_continues_not_sleeps(void) {
  ProtectionWakeConfirm confirm;
  TEST_ASSERT_EQUAL(static_cast<int>(ProtectionWakeStep::ContinueToSecondReading),
                    static_cast<int>(protectionWakeStep(confirm, true, 3750, false)));
  TEST_ASSERT_EQUAL_INT(1, confirm.confirmedReadings());
  // Fresh confirm per first reading: one qualifying sample leaves the
  // protocol waiting for its second, never done after a single feed.
  ProtectionWakeConfirm fresh;
  TEST_ASSERT_EQUAL(static_cast<int>(ProtectionWakeStep::ContinueToSecondReading),
                    static_cast<int>(protectionWakeStep(fresh, true, 4200, false)));
  TEST_ASSERT_EQUAL_INT(1, fresh.confirmedReadings());
}

void test_wake_path_first_invalid_sleeps_immediately(void) {
  ProtectionWakeConfirm confirm;
  TEST_ASSERT_EQUAL(static_cast<int>(ProtectionWakeStep::SleepNow),
                    static_cast<int>(protectionWakeStep(confirm, false, 0, false)));
  TEST_ASSERT_EQUAL_INT(0, confirm.confirmedReadings());
}

void test_wake_path_first_3749_sleeps_immediately(void) {
  ProtectionWakeConfirm confirm;
  TEST_ASSERT_EQUAL(static_cast<int>(ProtectionWakeStep::SleepNow),
                    static_cast<int>(protectionWakeStep(confirm, true, 3749, false)));
  TEST_ASSERT_EQUAL_INT(0, confirm.confirmedReadings());
}

// Full protocol walk mirroring the caller: step one, and only on
// ContinueToSecondReading take step two (exactly when the firmware would).
ProtectionWakeStep walkWakePath(bool firstValid, int firstMv, bool secondValid, int secondMv,
                                ProtectionWakeStep* secondOutcome) {
  ProtectionWakeConfirm confirm;
  const ProtectionWakeStep firstOutcome = protectionWakeStep(confirm, firstValid, firstMv, false);
  if (firstOutcome != ProtectionWakeStep::ContinueToSecondReading) {
    *secondOutcome = firstOutcome;  // caller sleeps now; no second reading
    return firstOutcome;
  }
  *secondOutcome = protectionWakeStep(confirm, secondValid, secondMv, true);
  return firstOutcome;
}

void test_wake_path_both_qualified_resumes(void) {
  ProtectionWakeStep second;
  const ProtectionWakeStep first = walkWakePath(true, 3750, true, 3750, &second);
  TEST_ASSERT_EQUAL(static_cast<int>(ProtectionWakeStep::ContinueToSecondReading),
                    static_cast<int>(first));
  TEST_ASSERT_EQUAL(static_cast<int>(ProtectionWakeStep::ResumeNormal), static_cast<int>(second));

  walkWakePath(true, 3800, true, 4200, &second);
  TEST_ASSERT_EQUAL(static_cast<int>(ProtectionWakeStep::ResumeNormal), static_cast<int>(second));
}

void test_wake_path_first_fail_never_reaches_second_reading(void) {
  ProtectionWakeStep second;
  TEST_ASSERT_EQUAL(static_cast<int>(ProtectionWakeStep::SleepNow),
                    static_cast<int>(walkWakePath(false, 0, true, 4200, &second)));
  TEST_ASSERT_EQUAL(static_cast<int>(ProtectionWakeStep::SleepNow),
                    static_cast<int>(walkWakePath(true, 3749, true, 4200, &second)));
}

void test_wake_path_second_fail_still_sleeps(void) {
  ProtectionWakeStep second;
  walkWakePath(true, 3800, false, 0, &second);
  TEST_ASSERT_EQUAL(static_cast<int>(ProtectionWakeStep::SleepNow), static_cast<int>(second));
  walkWakePath(true, 3800, true, 3749, &second);
  TEST_ASSERT_EQUAL(static_cast<int>(ProtectionWakeStep::SleepNow), static_cast<int>(second));
  // Boundary: exactly 3750 mV on the second reading qualifies.
  walkWakePath(true, 3750, true, 3750, &second);
  TEST_ASSERT_EQUAL(static_cast<int>(ProtectionWakeStep::ResumeNormal), static_cast<int>(second));
}

// The wake cycle length the protection arms (esp_sleep_enable_timer_wakeup):
// still exactly 600 s, converted to microseconds from the seconds constant
// in exactly one place (600,000,000 us; a raw seconds value would sleep far
// too briefly).
void test_wake_cycle_remains_600_seconds(void) {
  TEST_ASSERT_EQUAL_INT(600, static_cast<int>(kBatteryProtectionSleepS));
  TEST_ASSERT_EQUAL_UINT64(600000000ULL, kProtectionSleepUs);
  TEST_ASSERT_EQUAL_UINT64(static_cast<uint64_t>(kBatteryProtectionSleepS) * 1000ULL * 1000ULL,
                           kProtectionSleepUs);
  TEST_ASSERT_TRUE(kProtectionSleepUs != static_cast<uint64_t>(kBatteryProtectionSleepS));
}

void test_reset_clears_the_streak(void) {
  LowBatteryGuard guard;
  guard.feed(true, 3500);
  guard.reset();
  TEST_ASSERT_EQUAL_INT(0, guard.consecutiveLow());
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_ninth_low_sample_does_not_protect);
  RUN_TEST(test_tenth_low_sample_enters_protection);
  RUN_TEST(test_exactly_3600mv_is_recovered);
  RUN_TEST(test_invalid_sample_resets_and_never_triggers);
  RUN_TEST(test_high_sample_resets);
  RUN_TEST(test_3599mv_counts_low_despite_3_60v_display);
  RUN_TEST(test_two_valid_readings_at_3750_resume);
  RUN_TEST(test_two_valid_readings_above_3750_resume);
  RUN_TEST(test_single_reading_never_resumes);
  RUN_TEST(test_3749mv_resets_confirmation);
  RUN_TEST(test_3600mv_counts_recovered_for_entry_but_not_for_recovery);
  RUN_TEST(test_invalid_reading_keeps_protection);
  RUN_TEST(test_invalid_then_valid_still_lacks_confirmation);
  RUN_TEST(test_all_invalid_readings_keep_protection);
  RUN_TEST(test_percent_display_never_controls_recovery);
  RUN_TEST(test_wake_path_healthy_first_reading_continues_not_sleeps);
  RUN_TEST(test_wake_path_first_invalid_sleeps_immediately);
  RUN_TEST(test_wake_path_first_3749_sleeps_immediately);
  RUN_TEST(test_wake_path_both_qualified_resumes);
  RUN_TEST(test_wake_path_first_fail_never_reaches_second_reading);
  RUN_TEST(test_wake_path_second_fail_still_sleeps);
  RUN_TEST(test_wake_cycle_remains_600_seconds);
  RUN_TEST(test_reset_clears_the_streak);
  return UNITY_END();
}

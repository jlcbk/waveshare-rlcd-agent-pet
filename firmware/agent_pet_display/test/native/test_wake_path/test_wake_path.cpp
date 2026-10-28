#include <unity.h>

#include <stdint.h>
#include <vector>

#include "Arduino.h"
#include "models.h"

// Compiles and drives the REAL batteryProtectionEarlyCheck() /
// batteryProtectionEnterDeepSleep() from src/battery_protection.cpp - not a
// re-implementation. The Arduino/ESP interfaces come from shim/, readings
// are injected, and every observable side effect (samples taken, delays,
// timer period, sleep entries, RTC flag) is asserted. Same technique as the
// scratch harness this suite is based on.
//
// Renaming samplePowerState at the preprocessor level avoids a duplicate
// symbol with the native battery_monitor.cpp translation unit; the pure
// decision logic (ProtectionWakeConfirm / protectionWakeStep) links in from
// the real src files.
#define samplePowerState wakeTestSample
#define ARDUINO 1
#include "battery_protection.cpp"
#undef ARDUINO
#undef samplePowerState

namespace {

struct SleepEvent {};

std::vector<PowerState> samples;
std::vector<unsigned long> delays;
unsigned reads = 0;
uint64_t timerUs = 0;
unsigned sleeps = 0;

PowerState reading(bool valid, int mv) {
  PowerState power{};
  power.sampleOk = valid;
  power.voltageMv = mv;
  return power;
}

}  // namespace

// External-linkage definition for the extern declaration in shim/Arduino.h
// (an anonymous-namespace one would not satisfy it at link time).
FakeSerial Serial;

void delay(unsigned long ms) {
  delays.push_back(ms);
}

int esp_sleep_get_wakeup_cause() {
  return 0;  // ESP_SLEEP_WAKEUP_UNDEFINED: the unexpected-reset branch
}

int esp_sleep_disable_wakeup_source(int) {
  return 0;
}

int esp_sleep_enable_timer_wakeup(uint64_t us) {
  timerUs = us;
  return 0;
}

void esp_deep_sleep_start() {
  ++sleeps;
  throw SleepEvent{};
}

PowerState wakeTestSample(bool) {
  TEST_ASSERT_TRUE_MESSAGE(reads < samples.size(), "firmware sampled more often than the protocol allows");
  return samples[reads++];
}

namespace {

// Enters protection through the real entry point so the RTC flag, wake
// counter and timer are set exactly as on the board.
void enterProtectionThroughRealPath() {
  try {
    batteryProtectionEnterDeepSleep();
    TEST_FAIL_MESSAGE("enterDeepSleep returned");
  } catch (const SleepEvent&) {
  }
  TEST_ASSERT_TRUE(batteryProtectionWasActive());
}

// Runs one early-wake protocol: injected warm-up + first (+ second) readings.
// resumes: expected normal-boot return; expectedReads counts the warm-up.
void runWake(const PowerState& first, const PowerState& second, bool resumes, unsigned expectedReads) {
  enterProtectionThroughRealPath();
  const uint32_t cyclesBefore = batteryProtectionWakeCount();

  samples = {reading(false, 0), first, second};  // warm-up discarded, then decisions
  reads = 0;
  delays.clear();
  timerUs = 0;
  sleeps = 0;

  bool returned = false;
  try {
    batteryProtectionEarlyCheck();
    returned = true;
  } catch (const SleepEvent&) {
  }

  TEST_ASSERT_EQUAL_MESSAGE(resumes, returned, "resume/sleep outcome");
  TEST_ASSERT_EQUAL_UINT32(expectedReads, reads);
  // delays: always the 120 ms warm-up gap; the 1000 ms confirmation gap
  // exists only when the first reading qualified (expectedReads == 3).
  TEST_ASSERT_EQUAL_UINT32(expectedReads == 3 ? 2u : 1u, delays.size());
  TEST_ASSERT_EQUAL_UINT32(120, delays[0]);
  if (expectedReads == 3) {
    TEST_ASSERT_EQUAL_UINT32(1000, delays[1]);
  }
  TEST_ASSERT_TRUE(batteryProtectionWasActive() != resumes);
  if (!resumes) {
    TEST_ASSERT_EQUAL_UINT32(1, sleeps);
    TEST_ASSERT_EQUAL_UINT64(600000000ULL, timerUs);
    TEST_ASSERT_TRUE(batteryProtectionWakeCount() == cyclesBefore + 1);
  } else {
    TEST_ASSERT_EQUAL_UINT32(0, sleeps);
  }
}

}  // namespace

void setUp(void) {}

void tearDown(void) {}

void test_wake_two_qualified_readings_resume_normal_boot(void) {
  runWake(reading(true, 3750), reading(true, 3750), true, 3);
}

void test_wake_first_3749_sleeps_immediately(void) {
  runWake(reading(true, 3749), reading(true, 4000), false, 2);
}

void test_wake_first_invalid_sleeps_immediately(void) {
  runWake(reading(false, 4000), reading(true, 4000), false, 2);
}

void test_wake_second_3749_still_sleeps(void) {
  runWake(reading(true, 4000), reading(true, 3749), false, 3);
}

void test_wake_second_invalid_still_sleeps(void) {
  runWake(reading(true, 4000), reading(false, 4000), false, 3);
}

// A healthy first reading must pay the 1 s gap and take the second sample -
// the regression where feed(first)==KeepSleeping was misread as "first
// failed" (recovery impossible) is pinned by the read/delay counts here.
void test_wake_healthy_first_pays_one_second_then_confirms(void) {
  runWake(reading(true, 4200), reading(true, 4200), true, 3);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_wake_two_qualified_readings_resume_normal_boot);
  RUN_TEST(test_wake_first_3749_sleeps_immediately);
  RUN_TEST(test_wake_first_invalid_sleeps_immediately);
  RUN_TEST(test_wake_second_3749_still_sleeps);
  RUN_TEST(test_wake_second_invalid_still_sleeps);
  RUN_TEST(test_wake_healthy_first_pays_one_second_then_confirms);
  return UNITY_END();
}

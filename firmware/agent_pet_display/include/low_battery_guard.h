#pragma once

#include <stdint.h>

#include "app_config.h"

// Pure decision logic for the low-battery protection (unit-tested natively;
// exactly what runs on the board). Two independent halves:
//
//  1. LowBatteryGuard: while the firmware runs, counts consecutive *valid*
//     samples strictly below the entry threshold (3.60 V). Invalid ADC
//     samples and samples at/above the threshold reset the count: a broken
//     sensor must never be read as "battery low", and 3.60 V exactly counts
//     as recovered. Percent never participates - raw millivolts only.
//  2. ProtectionWakeConfirm: after each 600 s protection wake, the early
//     boot path must see TWO fresh consecutive valid readings at/above the
//     recovery threshold (3.75 V, deliberately above entry so a resting cell
//     cannot bounce straight back into the load that collapsed it), fed
//     about 1 s apart by the caller after the discarded warm-up conversion.
//     Any invalid or sub-3.75 V reading restarts the count and keeps the
//     board protected; resuming on the strength of one sample is impossible.
//
// Display formatting (the 0.01 V string in battery_monitor) is deliberately
// not used here: protection always decides on raw millivolts.

enum class LowBatteryAction {
  None,
  EnterProtection,
};

enum class ProtectionWakeAction {
  KeepSleeping,
  ResumeNormal,
};

class LowBatteryGuard {
 public:
  static constexpr int kThresholdMv = kBatteryProtectionThresholdMv;
  static constexpr int kConsecutiveLow = kBatteryProtectionConsecutive;

  LowBatteryAction feed(bool sampleValid, int voltageMv);
  int consecutiveLow() const { return consecutiveLow_; }
  void reset();

 private:
  int consecutiveLow_ = 0;
};

// Decision object actually used by batteryProtectionEarlyCheck(): the native
// tests exercise this exact class, not a reimplementation.
class ProtectionWakeConfirm {
 public:
  static constexpr int kThresholdMv = kBatteryRecoveryThresholdMv;
  static constexpr int kRequiredReadings = 2;

  ProtectionWakeAction feed(bool sampleValid, int voltageMv);
  int confirmedReadings() const { return confirmed_; }
  void reset();

 private:
  int confirmed_ = 0;
};

// The per-reading decision batteryProtectionEarlyCheck() actually runs: it
// samples the battery, calls this once per reading (first, then - only if
// the first qualified - the second after ~1 s) and acts on the result.
// Extracted verbatim from the wake path so the native tests execute the
// real call path's logic, not a description of it.
enum class ProtectionWakeStep {
  SleepNow,                 // stop measuring; re-enter the 600 s deep sleep
  ContinueToSecondReading,  // first reading qualified: wait ~1 s, read again
  ResumeNormal,             // both readings qualified: boot normally
};

ProtectionWakeStep protectionWakeStep(ProtectionWakeConfirm& confirm,
                                      bool sampleValid,
                                      int voltageMv,
                                      bool secondReading);

// Deep-sleep timer period in microseconds: esp_sleep_enable_timer_wakeup()
// takes microseconds, so the seconds constant is converted in exactly one
// place (unit-tested).
constexpr uint64_t kProtectionSleepUs =
    static_cast<uint64_t>(kBatteryProtectionSleepS) * 1000ULL * 1000ULL;

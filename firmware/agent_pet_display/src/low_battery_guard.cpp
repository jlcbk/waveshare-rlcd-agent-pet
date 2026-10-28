#include "low_battery_guard.h"

LowBatteryAction LowBatteryGuard::feed(bool sampleValid, int voltageMv) {
  if (!sampleValid || voltageMv >= kThresholdMv) {
    consecutiveLow_ = 0;
    return LowBatteryAction::None;
  }
  if (consecutiveLow_ < kConsecutiveLow) {
    ++consecutiveLow_;
  }
  if (consecutiveLow_ >= kConsecutiveLow) {
    return LowBatteryAction::EnterProtection;
  }
  return LowBatteryAction::None;
}

void LowBatteryGuard::reset() {
  consecutiveLow_ = 0;
}

ProtectionWakeAction ProtectionWakeConfirm::feed(bool sampleValid, int voltageMv) {
  if (!sampleValid || voltageMv < kThresholdMv) {
    confirmed_ = 0;
    return ProtectionWakeAction::KeepSleeping;
  }
  if (confirmed_ < kRequiredReadings) {
    ++confirmed_;
  }
  return confirmed_ >= kRequiredReadings ? ProtectionWakeAction::ResumeNormal
                                         : ProtectionWakeAction::KeepSleeping;
}

void ProtectionWakeConfirm::reset() {
  confirmed_ = 0;
}

ProtectionWakeStep protectionWakeStep(ProtectionWakeConfirm& confirm,
                                      bool sampleValid,
                                      int voltageMv,
                                      bool secondReading) {
  const ProtectionWakeAction action = confirm.feed(sampleValid, voltageMv);
  if (action == ProtectionWakeAction::ResumeNormal) {
    return ProtectionWakeStep::ResumeNormal;
  }
  if (secondReading) {
    return ProtectionWakeStep::SleepNow;
  }
  // First reading: it qualified unless the confirmation count is back at
  // zero (invalid or sub-threshold sample resets it in feed()).
  return confirm.confirmedReadings() == 0 ? ProtectionWakeStep::SleepNow
                                          : ProtectionWakeStep::ContinueToSecondReading;
}

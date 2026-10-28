#pragma once

#include <stdint.h>

// Accumulates awake CPU cycles against wall-clock time between loop samples.
// While the chip is in auto light sleep the loop does not run and the cycle
// counter is stopped, so awakeCycles grows slower than wall time. The fraction
// is the firmware-side evidence of time actually spent asleep (a reading, not
// a configuration check). Pure logic: unit-tested in the native environment.
class AwakeStats {
 public:
  void begin(uint32_t cycles, int64_t wallUs);
  void sample(uint32_t cycles, int64_t wallUs);
  int64_t windowUs() const { return wallUs_ - windowStartUs_; }
  // Awake cycles scaled to microseconds at the configured CPU clock.
  int64_t awakeUs(int cpuFreqMhz) const;
  int awakePercent(int cpuFreqMhz) const;
  void resetWindow(uint32_t cycles, int64_t wallUs);

 private:
  uint32_t lastCycles_ = 0;
  int64_t lastWallUs_ = 0;
  int64_t awakeCycles_ = 0;
  int64_t wallUs_ = 0;
  int64_t windowStartUs_ = 0;
};

// Boot-time policy: try esp_pm_configure(80/80, light sleep off) first; if the
// precompiled SDK has no PM support (stub returns ESP_ERR_NOT_SUPPORTED) fall
// back to the fixed 80 MHz clock used by earlier builds.
void pmBegin();

bool pmSupported();

// Light sleep is armed after the boot log window so early logs are reliable.
void pmEnableLightSleep();
bool pmLightSleepArmed();

// Egg playback runs at full clock; keep one frequency authority (esp_pm when
// available, setCpuFrequencyMhz otherwise) instead of letting both fight.
void pmEggPerformanceOn();
void pmEggPerformanceOff();

// RTC-pad GPIO wakeup (official light-sleep wake path) plus a plain interrupt
// for BOOT and KEY, so a press resumes the loop promptly.
void pmSetupButtonWakeup(int bootPin, int keyPin);

// Before a deliberate deep sleep (low-battery protection): turns auto light
// sleep off so no idle sleep can race the sleep entry.
void pmPrepareDeepSleep();

// Called from the main loop: samples the awake stats and logs one evidence
// line per window (kPmStatsWindowMs). Low rate on purpose: heavy logging
// itself keeps the USB console and CPU awake.
void pmTick(uint32_t now);

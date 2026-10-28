#pragma once

#include <stdint.h>

// Deep-sleep protection for a drained battery (see low_battery_guard.h for
// the decision rules). The running firmware enters it only after the guard
// saw 10 consecutive *valid* samples below 3.60 V; while protected, the chip
// wakes every 600 s from the RTC timer and re-measures before anything else
// starts (no Wi-Fi, no screen, no full app).

// True when the RTC-data flag marks this boot as following a protection
// sleep. The flag survives deep sleep and software resets; it is cleared on
// resume and is invalid after a power cycle (magic word check).
bool batteryProtectionWasActive();

// How many 600 s cycles the board has slept through, diagnostics only.
uint32_t batteryProtectionWakeCount();

// Call at the very top of setup(), before pmBegin()/Wi-Fi/screen. When the
// protection flag is set: re-measure the battery (ADC only); a still-low or
// invalid reading goes straight back to deep sleep (never returns); two
// valid readings at/above 3.75 V clear the flag and allow a normal boot.
// Also runs after an unexpected reset while the flag was set, so a reset
// cannot trap the board: it measures and decides instead of assuming.
void batteryProtectionEarlyCheck();

// From the running firmware, after the low-battery notice is on screen:
// turn off auto light sleep, hold the LCD control pads, arm the 600 s RTC
// timer and enter deep sleep. Does not return.
void batteryProtectionEnterDeepSleep();

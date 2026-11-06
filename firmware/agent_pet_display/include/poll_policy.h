#pragma once

#include <stdint.h>

#include "models.h"

// Pure decision logic for the bridge poll interval (unit-tested natively).
// Priority: egg fast poll > active/FAST (3 s) > MEDIUM (10 s) > SLOW (30 s) > DEEP (120 s).
//
// Retry policy: the bridge keeps a generous online window measured from the last
// request it received. The first failure retries quickly instead of waiting;
// sustained failures back off, bounded at the idle interval, so an unreachable
// bridge does not keep the radio busy.
uint32_t pollIntervalMs(bool fastPoll,
                        bool agentBusy,
                        bool hasBaseline,
                        uint32_t msSinceAgentChange,
                        uint32_t failStreak);

// Derives the user-visible sleep/refresh pacing tier:
//   - Fast:   active agent, no baseline yet, or <= 1 min since change/wake
//   - Medium: 1..10 min unchanged
//   - Slow:   10..30 min unchanged
//   - Deep:   > 30 min unchanged (wakes back to Fast on any button press)
SleepPacingMode deriveSleepPacingMode(bool fastPoll,
                                      bool agentBusy,
                                      bool hasBaseline,
                                      uint32_t msSinceAgentChange);

const char* sleepPacingLabel(SleepPacingMode mode);

// Exponential backoff for Wi-Fi station reconnect attempts (10s -> 20s -> 40s
// -> 60s -> 120s max) so an offline router does not trigger full-channel scans
// every 10 s forever.
uint32_t wifiReconnectIntervalMs(uint32_t reconnectAttempts);

// Dynamic RTC clock sample delay: 1 s while invalid or across the :59->:00
// rollover; up to 59 s right after a minute rollover so I2C reads drop from
// 60/min to 2/min.
uint32_t nextClockSampleDelayMs(bool clockValid, int currentSecond);

// True when climate validity changed or temperature/humidity drifted across
// the display precision deadband (0.1 C / 1% RH).
bool climateDisplayChanged(bool valid,
                           float tempC,
                           float humidityPct,
                           bool prevValid,
                           float shownTempC,
                           float shownHumidityPct);

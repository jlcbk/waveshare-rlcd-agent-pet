#include "poll_policy.h"

#include <math.h>

#include "app_config.h"

SleepPacingMode deriveSleepPacingMode(bool fastPoll,
                                      bool agentBusy,
                                      bool hasBaseline,
                                      uint32_t msSinceAgentChange) {
  if (fastPoll || agentBusy || !hasBaseline || msSinceAgentChange <= kActiveHoldMs) {
    return SleepPacingMode::Fast;
  }
  if (msSinceAgentChange <= kMediumHoldMs) {
    return SleepPacingMode::Medium;
  }
  if (msSinceAgentChange <= kSlowHoldMs) {
    return SleepPacingMode::Slow;
  }
  return SleepPacingMode::Deep;
}

const char* sleepPacingLabel(SleepPacingMode mode) {
  switch (mode) {
    case SleepPacingMode::Fast:
      return "FAST";
    case SleepPacingMode::Medium:
      return "MEDIUM";
    case SleepPacingMode::Slow:
      return "SLOW";
    case SleepPacingMode::Deep:
    default:
      return "DEEP";
  }
}

uint32_t pollIntervalMs(bool fastPoll,
                        bool agentBusy,
                        bool hasBaseline,
                        uint32_t msSinceAgentChange,
                        uint32_t failStreak) {
  if (fastPoll) {
    return kFastPollMs;
  }
  if (failStreak > 0) {
    if (failStreak == 1) {
      // One idle timeout (2.5 s) plus its 10 s slot already reach ~12.5 s of
      // the bridge's online window; retry fast to stay inside it.
      return kPollRetryMs;
    }
    // Sustained failures back off, bounded by the idle interval, so an
    // unreachable bridge does not keep the radio busy.
    const uint32_t backoff = kPollRetryMs + failStreak * 3 * 1000;
    return backoff < kIdlePollMs ? backoff : kIdlePollMs;
  }
  switch (deriveSleepPacingMode(fastPoll, agentBusy, hasBaseline, msSinceAgentChange)) {
    case SleepPacingMode::Fast:
      return kBridgePollMs;
    case SleepPacingMode::Medium:
      return kIdlePollMs;
    case SleepPacingMode::Slow:
      return kSlowPollMs;
    case SleepPacingMode::Deep:
    default:
      return kDeepIdlePollMs;
  }
}

uint32_t wifiReconnectIntervalMs(uint32_t reconnectAttempts) {
  if (reconnectAttempts <= 1) {
    return kWifiReconnectMs;
  }
  if (reconnectAttempts == 2) {
    return 20000U;
  }
  if (reconnectAttempts == 3) {
    return 40000U;
  }
  if (reconnectAttempts == 4) {
    return 60000U;
  }
  return kWifiReconnectMaxMs;
}

uint32_t nextClockSampleDelayMs(bool clockValid, int currentSecond) {
  if (!clockValid || currentSecond < 0 || currentSecond >= 58) {
    return kClockSampleMs;
  }
  return static_cast<uint32_t>(59 - currentSecond) * 1000U;
}

bool climateDisplayChanged(bool valid,
                           float tempC,
                           float humidityPct,
                           bool prevValid,
                           float shownTempC,
                           float shownHumidityPct) {
  if (valid != prevValid) {
    return true;
  }
  if (!valid) {
    return false;
  }
  return fabsf(tempC - shownTempC) >= kClimateTempDeadbandC ||
         fabsf(humidityPct - shownHumidityPct) >= kClimateHumidityDeadbandPct;
}

#include "poll_policy.h"

#include "app_config.h"

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
      // the bridge's 15 s online window; retry fast to stay inside it.
      return kPollRetryMs;
    }
    // Sustained failures back off, bounded by the idle interval, so an
    // unreachable bridge does not keep the radio busy.
    const uint32_t backoff = kPollRetryMs + failStreak * 3 * 1000;
    return backoff < kIdlePollMs ? backoff : kIdlePollMs;
  }
  if (agentBusy || !hasBaseline || msSinceAgentChange <= kActiveHoldMs) {
    return kBridgePollMs;
  }
  return kIdlePollMs;
}

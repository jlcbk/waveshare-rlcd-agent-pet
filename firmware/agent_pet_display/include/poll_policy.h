#pragma once

#include <stdint.h>

// Pure decision logic for the bridge poll interval (unit-tested natively).
// Priority: egg fast poll > active (3 s) > idle (10 s).
//
// Retry policy: the bridge keeps a 15 s online window measured from the last
// request it received. One idle-poll timeout (2.5 s) already eats 12.5 s of
// that window, so the first failure retries quickly instead of waiting
// another 10 s; sustained failures back off, bounded at the idle interval,
// so an unreachable bridge does not keep the radio busy.
uint32_t pollIntervalMs(bool fastPoll,
                        bool agentBusy,
                        bool hasBaseline,
                        uint32_t msSinceAgentChange,
                        uint32_t failStreak);

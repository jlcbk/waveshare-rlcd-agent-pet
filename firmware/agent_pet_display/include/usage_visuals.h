#pragma once

#include <string>

struct QuotaUsage {
  int fiveHourPercent{-1};
  int weekPercent{-1};
  std::string fiveHourReset{};
  std::string weekReset{};
};

QuotaUsage parseQuotaUsage(const std::string& text);

// Returns true when two quota strings represent the same quota state ignoring
// passive "(Xm)" / "(XdYh)" reset countdown ticks, so minute-by-minute
// countdown updates do not keep the network poller in its 3 s active-hold mode.
bool quotaUsageSameIgnoringCountdown(const std::string& a, const std::string& b);

// Parses a context usage string such as "60.8k / 256k" or "1.2M / 2M" into a
// 0..100 percentage, or returns -1 when not parseable.
int parseContextUsagePercent(const std::string& text);

// Home page unit helpers:
// - expandTodayTokenUnit("49M tok") -> "49M Tokens"
// - stripTodayTokenUnit("49M tok")  -> "49M"
std::string expandTodayTokenUnit(const std::string& value);
std::string stripTodayTokenUnit(const std::string& value);

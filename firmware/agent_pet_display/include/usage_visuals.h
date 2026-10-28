#pragma once

#include <string>

struct QuotaUsage {
  int fiveHourPercent{-1};
  int weekPercent{-1};
};

QuotaUsage parseQuotaUsage(const std::string& text);

// Home page only: expands a trailing " tok" unit to the full word
// ("49M tok" -> "49M Token"). A "tok" that is not the trailing unit
// ("49M tok/min") and unit-less values pass through untouched. Applied at
// the Overview drawing site only - the Usage page and the protocol keep the
// bridge's original spelling.
std::string expandTodayTokenUnit(const std::string& value);

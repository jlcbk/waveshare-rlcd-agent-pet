#include "screen_layout.h"

namespace {

// Every Overview section spans the full usable width (page margin 12 on each
// side of the 400px panel); the old 256px metric column + pet panel is gone.
constexpr ScreenRect kOverviewTimeCard{12, 29, 376, 51};
constexpr ScreenRect kOverviewStatusCard{12, 86, 376, 65};
constexpr ScreenRect kOverviewMetricCards[3]{
    {12, 156, 376, 33},
    {12, 191, 376, 33},
    {12, 227, 376, 41},
};

}  // namespace

ScreenRect overviewTimeCardRect() {
  return kOverviewTimeCard;
}

ScreenRect overviewStatusCardRect() {
  return kOverviewStatusCard;
}

ScreenRect overviewMetricCardRect(int index) {
  if (index < 0) {
    return kOverviewMetricCards[0];
  }
  if (index > 2) {
    return kOverviewMetricCards[2];
  }
  return kOverviewMetricCards[index];
}

QuotaBarLayout overviewQuotaBarLayout() {
  const ScreenRect card = overviewMetricCardRect(2);
  // Align the first bar with the metric value column (card.x + 104).
  // The remaining 376 - 104 = 272px splits into two 124px bars with a 24px
  // gap; they end at card.x + 376 (x=388), matching the divider lines.
  return QuotaBarLayout{
      {card.x + 104, card.y + 26, 124, 4},
      {card.x + 252, card.y + 26, 124, 4},
  };
}

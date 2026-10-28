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
  // Label column ("QUOTA", clips at 66px) stays on the left, so the bars start
  // at card.x + 70 as before. The remaining 376 - 70 - 12 = 294px splits into
  // two 135px bars with a 24px gap; they end at card.x + 364, keeping the same
  // 12px inner margin as the page edges. "5H n%" / "WK n%" draw above their
  // bars and measure well under 135px at helvB12.
  return QuotaBarLayout{
      {card.x + 70, card.y + 26, 135, 4},
      {card.x + 229, card.y + 26, 135, 4},
  };
}

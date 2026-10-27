#include "screen_layout.h"

namespace {

// Left column: full-width header, time and status; the three metric cards
// share a 256px column so the static pet panel fits on the right.
constexpr ScreenRect kOverviewTimeCard{12, 29, 376, 51};
constexpr ScreenRect kOverviewStatusCard{12, 86, 376, 65};
constexpr ScreenRect kOverviewMetricCards[3]{
    {12, 156, 256, 33},
    {12, 191, 256, 33},
    {12, 227, 256, 41},
};
constexpr ScreenRect kOverviewPetPanel{278, 156, 110, 112};

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

ScreenRect overviewPetPanelRect() {
  return kOverviewPetPanel;
}

QuotaBarLayout overviewQuotaBarLayout() {
  const ScreenRect card = overviewMetricCardRect(2);
  // Label column ("QUOTA") on the left; "5H n%" / "WK n%" sit above their bars.
  return QuotaBarLayout{
      {card.x + 70, card.y + 26, 88, 4},
      {card.x + 166, card.y + 26, 84, 4},
  };
}

#pragma once

struct ScreenRect {
  int x;
  int y;
  int w;
  int h;
};

struct QuotaBarLayout {
  ScreenRect fiveHourBar;
  ScreenRect weekBar;
};

ScreenRect overviewTimeCardRect();
ScreenRect overviewStatusCardRect();
ScreenRect overviewMetricCardRect(int index);
ScreenRect overviewPetPanelRect();
QuotaBarLayout overviewQuotaBarLayout();

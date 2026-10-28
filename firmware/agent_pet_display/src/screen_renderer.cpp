#include "screen_renderer.h"

#include <Arduino.h>
#include <U8g2lib.h>

#include <stdio.h>
#include <string>

#include "ST7305_U8g2.h"
#include "app_config.h"
#include "battery_monitor.h"
#include "clock_faces.h"
#include "doto_font.h"
#include "screen_layout.h"
#include "usage_visuals.h"

namespace {

constexpr int kLcdWidth = 400;
constexpr int kLcdHeight = 300;
constexpr int kTopBarHeight = 24;
constexpr int kFooterHeight = 28;
constexpr int kLabelChipHeight = 11;
constexpr int kUsagePanelX = 12;
constexpr int kUsagePanelWidth = 376;
constexpr int kUsageHeaderY = 34;
constexpr int kUsageSmallCardY = 60;
constexpr int kUsageSmallCardWidth = 182;
constexpr int kUsageSmallCardHeight = 40;
constexpr int kUsageBigCardX = 12;
constexpr int kUsageBigCardWidth = 376;
constexpr int kUsageBigCardHeight = 48;
constexpr int kUsageBigCardGap = 8;

// LCD wiring comes from app_config.h (kLcdSckPin/kLcdMosiPin/kLcdDcPin/
// kLcdCsPin/kLcdRstPin = 11/12/5/40/41).
ST7305_U8g2 gLcd(kLcdSckPin, kLcdMosiPin, kLcdDcPin, kLcdCsPin, kLcdRstPin);
U8G2* gU8g2 = nullptr;

std::string clipTextToPixelWidth(const std::string& text, int maxWidth) {
  if (!gU8g2 || maxWidth <= 0 || text.empty()) {
    return maxWidth <= 0 ? std::string() : text;
  }
  if (gU8g2->getStrWidth(text.c_str()) <= maxWidth) {
    return text;
  }

  for (size_t length = text.size(); length > 0; --length) {
    const std::string candidate = text.substr(0, length - 1) + "..";
    if (gU8g2->getStrWidth(candidate.c_str()) <= maxWidth) {
      return candidate;
    }
  }
  return "..";
}

std::string clipDotoTextToPixelWidth(const std::string& text, int maxWidth, DotoFontSize size) {
  if (maxWidth <= 0 || text.empty()) {
    return maxWidth <= 0 ? std::string() : text;
  }
  if (dotoTextWidth(size, text.c_str()) <= maxWidth) {
    return text;
  }

  for (size_t length = text.size(); length > 0; --length) {
    const std::string candidate = text.substr(0, length - 1) + "..";
    if (dotoTextWidth(size, candidate.c_str()) <= maxWidth) {
      return candidate;
    }
  }
  return "..";
}

void drawDotoText(DotoFontSize size, int x, int y, const std::string& text) {
  if (!gU8g2 || text.empty()) {
    return;
  }
  gU8g2->setDrawColor(0);
  int cursorX = x;
  const int height = dotoTextHeight(size);
  for (char ch : text) {
    const int advance = dotoGlyphAdvance(size, ch);
    for (int row = 0; row < height; ++row) {
      int runStart = -1;
      for (int col = 0; col <= advance; ++col) {
        const bool isOn = col < advance &&
                          dotoGlyphPixel(size, ch, static_cast<uint8_t>(col), static_cast<uint8_t>(row));
        if (isOn && runStart < 0) {
          runStart = col;
        } else if (!isOn && runStart >= 0) {
          gU8g2->drawHLine(cursorX + runStart, y + row, col - runStart);
          runStart = -1;
        }
      }
    }
    cursorX += advance;
  }
}

// Keep the existing header slot and RSSI buckets while drawing solid bars.
constexpr int kWifiIconWidth = 22;
constexpr int kWifiSuffixOffsetX = kWifiIconWidth + 3;

int wifiIconBarCount(const DisplayState& display) {
  switch (display.wifiIconState) {
    case WifiIconState::Connected:
      return display.wifiIconLevel < 1 ? 1 : (display.wifiIconLevel > 4 ? 4 : display.wifiIconLevel);
    case WifiIconState::Connecting:
      return 1;
    case WifiIconState::ConnectedNoRssi:
    case WifiIconState::Unknown:
    case WifiIconState::Off:
    default:
      return 0;
  }
}

// Short state suffix next to the icon, in the page's own top-bar font:
// "--" unknown, "TRY" connecting, "OFF" off, "OK" no RSSI. A connected
// signal gets no suffix: the number of bars shows the level.
const char* wifiSuffixText(const DisplayState& display) {
  switch (display.wifiIconState) {
    case WifiIconState::Unknown:
      return "--";
    case WifiIconState::Connecting:
      return "TRY";
    case WifiIconState::Off:
      return "OFF";
    case WifiIconState::ConnectedNoRssi:
      return "OK";
    case WifiIconState::Connected:
    default:
      return "";
  }
}

// The caller renders the optional state suffix in the page's own font.
void drawWifiIcon(int leftX, const DisplayState& display) {
  constexpr int heights[] = {5, 8, 11, 14};
  gU8g2->setDrawColor(0);
  const int bars = wifiIconBarCount(display);
  for (int i = 0; i < bars; ++i) {
    gU8g2->drawBox(leftX + 1 + i * 5, 20 - heights[i], 4, heights[i]);
  }
}

std::string heroStatusText(const DisplayState& display) {
  if (display.offline) {
    return "OFFLINE";
  }
  return display.statusDetail;
}

void drawLabelChip(int x, int y, int width, const char* label) {
  gU8g2->drawRBox(x, y, width, kLabelChipHeight, 3);
  gU8g2->setDrawColor(1);
  gU8g2->setFont(u8g2_font_helvB08_tr);
  gU8g2->drawStr(x + 4, y + 1, clipTextToPixelWidth(label ? label : "", width - 8).c_str());
  gU8g2->setDrawColor(0);
}

int chipWidthForLabel(const char* label, int minWidth, int maxWidth) {
  gU8g2->setFont(u8g2_font_helvB08_tr);
  const int textWidth = gU8g2->getStrWidth(label ? label : "");
  int width = textWidth + 8;
  if (width < minWidth) {
    width = minWidth;
  }
  if (width > maxWidth) {
    width = maxWidth;
  }
  return width;
}

void drawUsageMiniCard(int x, int y, int width, const char* label, const std::string& line1, const std::string& line2) {
  gU8g2->drawRFrame(x, y, width, kUsageSmallCardHeight, 5);
  drawLabelChip(x + 8, y + 4, chipWidthForLabel(label, 28, 64), label);
  drawDotoText(DotoFontSize::Medium,
               x + 8,
               y + 16,
               clipDotoTextToPixelWidth(line1, width - 16, DotoFontSize::Medium));
  drawDotoText(DotoFontSize::Small,
               x + 8,
               y + 28,
               clipDotoTextToPixelWidth(line2, width - 16, DotoFontSize::Small));
}

void drawUsageDetailCard(int y,
                         const char* label,
                         const std::string& value,
                         const std::string& hint) {
  gU8g2->drawRFrame(kUsageBigCardX, y, kUsageBigCardWidth, kUsageBigCardHeight, 6);
  drawLabelChip(kUsageBigCardX + 8, y + 4, chipWidthForLabel(label, 34, 64), label);
  drawDotoText(DotoFontSize::Medium,
               kUsageBigCardX + 8,
               y + 16,
               clipDotoTextToPixelWidth(value, kUsageBigCardWidth - 16, DotoFontSize::Medium));
  drawDotoText(DotoFontSize::Small,
               kUsageBigCardX + 8,
               y + 33,
               clipDotoTextToPixelWidth(hint, kUsageBigCardWidth - 16, DotoFontSize::Small));
}

void drawOverviewMetric(const ScreenRect& rect, const std::string& label, const std::string& value) {
  // Full-width card (376px): the value column starts at +104 and gets 272px,
  // so the font step-down below only triggers for pathological input — every
  // stock reading ("49M Token", "189k / 258k", "12 / 250") fits at helvB24.
  // The label column keeps helvB12 and clips at 92px; the longest stock label
  // (CONTEXT) measures 79px.
  constexpr int valueOffset = 104;
  gU8g2->setFont(u8g2_font_helvB12_tr);
  gU8g2->drawStr(rect.x, rect.y + 9, clipTextToPixelWidth(label, valueOffset - 12).c_str());
  // Step the value down helvB24 -> helvB18 -> helvB12 before ever clipping, so
  // real readings never lose digits; ".." is reserved for pathological input.
  // The y offsets keep all three sizes on the helvB24 baseline (y + 27).
  const int available = rect.w - valueOffset;
  const uint8_t* fonts[] = {u8g2_font_helvB24_tr, u8g2_font_helvB18_tr, u8g2_font_helvB12_tr};
  const int yOffsets[] = {2, 8, 15};
  int pick = 0;
  gU8g2->setFont(fonts[pick]);
  while (pick < 2 && gU8g2->getStrWidth(value.c_str()) > available) {
    gU8g2->setFont(fonts[++pick]);
  }
  gU8g2->drawStr(rect.x + valueOffset, rect.y + yOffsets[pick],
                 clipTextToPixelWidth(value, available).c_str());
}

void drawOverviewQuota(const ScreenRect& rect, const std::string& label, const std::string& value) {
  const QuotaUsage usage = parseQuotaUsage(value);
  const QuotaBarLayout bars = overviewQuotaBarLayout();
  gU8g2->setFont(u8g2_font_helvB12_tr);
  // 66px: measured "QUOTA" is 60px and must not clip (".." was shown at 56px);
  // still 4px clear of the "5H n%" text that starts at card.x + 70.
  gU8g2->drawStr(rect.x, rect.y + 5, clipTextToPixelWidth(label, 66).c_str());
  const int values[] = {usage.fiveHourPercent, usage.weekPercent};
  const char* names[] = {"5H", "WK"};
  const ScreenRect barRects[] = {bars.fiveHourBar, bars.weekBar};
  for (int index = 0; index < 2; ++index) {
    char text[24];
    if (values[index] < 0) {
      snprintf(text, sizeof(text), "%s --", names[index]);
    } else {
      snprintf(text, sizeof(text), "%s %d%%", names[index], values[index]);
    }
    const ScreenRect& bar = barRects[index];
    gU8g2->drawStr(bar.x, rect.y + 5, clipTextToPixelWidth(text, bar.w).c_str());
    gU8g2->drawFrame(bar.x, bar.y, bar.w, bar.h);
    if (values[index] > 0) {
      const int percent = values[index] > 100 ? 100 : values[index];
      gU8g2->drawBox(bar.x + 1, bar.y + 1, ((bar.w - 2) * percent) / 100, bar.h - 2);
    }
  }
}

void renderOverviewPage(const DisplayState& display, const std::string& batteryLine) {
  const ScreenRect time = overviewTimeCardRect();
  const ScreenRect status = overviewStatusCardRect();
  gU8g2->setFont(u8g2_font_helvB12_tr);
  // batteryLine arrives as "4.09V  87%" for a valid sample, plain percent
  // otherwise; clip keeps the climate label's room even in the worst case.
  const std::string batteryText = clipTextToPixelWidth(batteryLine, 160);
  const int batteryX = 388 - gU8g2->getStrWidth(batteryText.c_str());
  // Icon and optional status stay clear of climate at x=132.
  drawWifiIcon(12, display);
  gU8g2->drawStr(12 + kWifiSuffixOffsetX, 5,
                 clipTextToPixelWidth(wifiSuffixText(display), 110 - kWifiSuffixOffsetX).c_str());
  gU8g2->drawStr(132, 5, clipTextToPixelWidth(display.sidebarClimate, batteryX - 140).c_str());
  gU8g2->drawStr(batteryX, 5, batteryText.c_str());
  gU8g2->drawHLine(12, 24, 376);

  gU8g2->setFont(u8g2_font_logisoso42_tn);
  gU8g2->drawStr(time.x, time.y, clipTextToPixelWidth(display.sidebarTime, 230).c_str());
  gU8g2->setFont(u8g2_font_helvB12_tr);
  const std::string date = clipTextToPixelWidth(display.sidebarDate, 130);
  const std::string focus = clipTextToPixelWidth(display.focusLabel, 130);
  gU8g2->drawStr(time.x + time.w - gU8g2->getStrWidth(date.c_str()), time.y + 3, date.c_str());
  gU8g2->drawStr(time.x + time.w - gU8g2->getStrWidth(focus.c_str()), time.y + 23, focus.c_str());
  gU8g2->drawHLine(12, 82, 376);

  const std::string source = clipTextToPixelWidth(display.sourceLabel, 200);
  const std::string bridge = clipTextToPixelWidth(display.bridgeLine, 150);
  gU8g2->drawStr(status.x, status.y, source.c_str());
  gU8g2->drawStr(status.x + status.w - gU8g2->getStrWidth(bridge.c_str()), status.y, bridge.c_str());
  gU8g2->setFont(u8g2_font_helvB24_tr);
  gU8g2->drawStr(status.x, status.y + 16,
                 clipTextToPixelWidth(heroStatusText(display), status.w).c_str());
  gU8g2->setFont(u8g2_font_helvB12_tr);
  gU8g2->drawStr(status.x, status.y + 48,
                 clipTextToPixelWidth(display.taskLine, status.w).c_str());
  gU8g2->drawHLine(12, 153, 376);

  // Home page only: the TODAY card spells the unit out ("49M Token"); the
  // Usage page below keeps the bridge's original "49M tok" spelling.
  drawOverviewMetric(overviewMetricCardRect(0), display.sidebarTokensLabel,
                     expandTodayTokenUnit(display.sidebarTokens));
  drawOverviewMetric(overviewMetricCardRect(1), display.sidebarContextLabel, display.sidebarContext);
  if (display.sidebarQuotaStyle == "quota") {
    drawOverviewQuota(overviewMetricCardRect(2), display.sidebarQuotaLabel, display.sidebarQuota);
  } else {
    drawOverviewMetric(overviewMetricCardRect(2), display.sidebarQuotaLabel, display.sidebarQuota);
  }
  gU8g2->drawHLine(12, 272, 376);
  gU8g2->setFont(u8g2_font_helvB10_tr);
  gU8g2->drawStr(12, 280, clipTextToPixelWidth(display.footerMessage, 310).c_str());
  char pageLabel[16];
  snprintf(pageLabel, sizeof(pageLabel), "P%d/%d", screenPageNumber(display.page), kScreenPageCount);
  gU8g2->drawStr(388 - gU8g2->getStrWidth(pageLabel), 280, pageLabel);
}

void renderUsagePage(const DisplayState& display) {
  drawDotoText(DotoFontSize::Medium, kUsagePanelX, kUsageHeaderY - 2, "USAGE DETAILS");
  // Place the agent label right of the heading (measured, not hardcoded: the
  // proportional Inter advances differ from the old monospaced Doto cells).
  const int headingWidth = dotoTextWidth(DotoFontSize::Medium, "USAGE DETAILS");
  const int agentX = kUsagePanelX + headingWidth + 8;
  drawDotoText(DotoFontSize::Small,
               agentX,
               kUsageHeaderY + 1,
               clipDotoTextToPixelWidth(display.sidebarAgent, kUsagePanelX + kUsagePanelWidth - agentX, DotoFontSize::Small));

  drawUsageMiniCard(kUsagePanelX, kUsageSmallCardY, kUsageSmallCardWidth, "TIME", display.sidebarTime, display.sidebarDate);
  drawUsageMiniCard(kUsagePanelX + kUsageSmallCardWidth + 12,
                    kUsageSmallCardY,
                    kUsageSmallCardWidth,
                    "CLIMATE",
                    display.sidebarClimate,
                    display.statusDetail);

  const int detailStartY = kUsageSmallCardY + kUsageSmallCardHeight + 8;
  drawUsageDetailCard(detailStartY,
                      display.sidebarTokensLabel.c_str(),
                      display.sidebarTokens,
                      display.sidebarTokensHint);
  drawUsageDetailCard(detailStartY + kUsageBigCardHeight + kUsageBigCardGap,
                      display.sidebarContextLabel.c_str(),
                      display.sidebarContext,
                      display.sidebarContextHint);
  drawUsageDetailCard(detailStartY + (kUsageBigCardHeight + kUsageBigCardGap) * 2,
                      display.sidebarQuotaLabel.c_str(),
                      display.sidebarQuota,
                      display.sidebarQuotaHint);
}

int drawFocusChip(const DisplayState& display, int rightEdge) {
  const int width = chipWidthForLabel(display.focusLabel.c_str(), 34, 52);
  const int x = rightEdge - width;
  drawLabelChip(x, 7, width, display.focusLabel.c_str());
  return x;
}

uint32_t gLastFrameHash = 0;
bool gHasFrame = false;

uint32_t frameHash() {
  const uint8_t* buffer = gU8g2->getBufferPtr();
  const size_t size = static_cast<size_t>(gU8g2->getBufferTileWidth()) * 8U * gU8g2->getBufferTileHeight();
  uint32_t hash = 2166136261u;
  for (size_t index = 0; index < size; ++index) {
    hash ^= buffer[index];
    hash *= 16777619u;
  }
  return hash;
}

// Pushing 15 KB over SPI costs power; skip it when nothing changed.
void flushIfChanged() {
  const uint32_t hash = frameHash();
  if (gHasFrame && hash == gLastFrameHash) {
    return;
  }
  gU8g2->sendBuffer();
  gLastFrameHash = hash;
  gHasFrame = true;
}

}  // namespace

void ScreenRenderer::begin() {
  gLcd.begin(0, U8G2_R1);
  gU8g2 = gLcd.getU8g2();
  gU8g2->setFontPosTop();
  gU8g2->setFont(u8g2_font_t0_16b_tr);
}

U8G2* ScreenRenderer::u8g2() {
  return gU8g2;
}

void ScreenRenderer::invalidate() {
  gHasFrame = false;
}

void ScreenRenderer::render(const DisplayState& display, const PowerState& power) {
  if (!gU8g2) {
    return;
  }

  if (display.page == ScreenPage::Clock) {
    gU8g2->clearBuffer();
    gU8g2->setDrawColor(1);
    gU8g2->drawBox(0, 0, kLcdWidth, kLcdHeight);
    gU8g2->setDrawColor(0);
    gU8g2->setFontPosTop();
    drawClockFace(*gU8g2, display.clock, millis());
    flushIfChanged();
    return;
  }

  char battery[32];
  // Integer percent for a valid sample; a failed/un-calibrated read is
  // missing data ("--%"), never "0% LOW" - zero would read as an empty
  // battery when the sensor is what failed.
  if (!power.sampleOk) {
    snprintf(battery, sizeof(battery), "--%%");
  } else if (display.showBatteryDetail && power.lowBattery) {
    snprintf(battery, sizeof(battery), "%d%% LOW", power.percent);
  } else if (display.showBatteryDetail && power.charging) {
    snprintf(battery, sizeof(battery), "%d%% CHG", power.percent);
  } else {
    snprintf(battery, sizeof(battery), "%d%%", power.percent);
  }
  // Home page top bar reads "4.09V  87%" (or plain "87%"); the Usage page
  // keeps its original percent-only bar so the voltage prefix cannot crowd
  // the source/focus labels on the second page.
  std::string batteryLine = battery;
  if (power.sampleOk && power.voltageMv > 0) {
    batteryLine = formatBatteryVoltage(power.voltageMv) + "  " + batteryLine;
  }

  gU8g2->clearBuffer();
  gU8g2->setDrawColor(1);
  gU8g2->drawBox(0, 0, kLcdWidth, kLcdHeight);
  gU8g2->setDrawColor(0);
  gU8g2->setFontPosTop();
  if (display.page != ScreenPage::Usage) {
    renderOverviewPage(display, batteryLine);
    flushIfChanged();
    return;
  }
  gU8g2->drawFrame(0, 0, kLcdWidth, kLcdHeight);
  gU8g2->drawHLine(8, kTopBarHeight, kLcdWidth - 16);

  const int batteryWidth = dotoTextWidth(DotoFontSize::Small, battery);
  const int batteryX = kLcdWidth - 12 - batteryWidth;
  const int focusChipLeft = drawFocusChip(display, batteryX - 8);
  // Same icon as the Overview top bar; icon + Doto suffix stays inside the
  // original 76 px linkLabel budget, clear of climate at x=92.
  drawWifiIcon(12, display);
  drawDotoText(DotoFontSize::Small,
               12 + kWifiSuffixOffsetX,
               5,
               clipDotoTextToPixelWidth(wifiSuffixText(display), 76 - kWifiSuffixOffsetX, DotoFontSize::Small));
  drawDotoText(DotoFontSize::Small, 92, 5, clipDotoTextToPixelWidth(display.sidebarClimate, 72, DotoFontSize::Small));
  const int sourceX = 170;
  const int sourceWidth = focusChipLeft - sourceX - 8;
  drawDotoText(DotoFontSize::Small,
               sourceX,
               5,
               clipDotoTextToPixelWidth(display.sourceLabel, sourceWidth, DotoFontSize::Small));
  drawDotoText(DotoFontSize::Small, batteryX, 5, battery);

  renderUsagePage(display);

  const int footerY = kLcdHeight - kFooterHeight;
  const char* footerLabel = display.page == ScreenPage::Usage ? "KEYS" : "LIVE";
  gU8g2->drawHLine(8, footerY, kLcdWidth - 16);
  drawLabelChip(12, footerY + 6, chipWidthForLabel(footerLabel, 32, 40), footerLabel);
  drawDotoText(DotoFontSize::Small,
               58,
               footerY + 7,
               clipDotoTextToPixelWidth(display.footerMessage, 260, DotoFontSize::Small));
  char pageLabel[16];
  snprintf(pageLabel, sizeof(pageLabel), "P%d/%d", screenPageNumber(display.page), kScreenPageCount);
  drawDotoText(DotoFontSize::Small, 338, footerY + 7, pageLabel);
  flushIfChanged();
}

void ScreenRenderer::renderLowBatteryNotice(const PowerState& power) {
  if (!gU8g2) {
    return;
  }
  // Same polarity as every other page: full dark backdrop, light text.
  gU8g2->clearBuffer();
  gU8g2->setDrawColor(1);
  gU8g2->drawBox(0, 0, kLcdWidth, kLcdHeight);
  gU8g2->setDrawColor(0);
  gU8g2->setFontPosTop();

  constexpr int kMarginX = 28;
  gU8g2->setFont(u8g2_font_helvB24_tr);
  gU8g2->drawStr(kMarginX, 44, "LOW BATTERY");
  gU8g2->drawHLine(kMarginX, 86, kLcdWidth - 2 * kMarginX);

  // Raw reading, never a filtered/deadband value: formatBatteryVoltage returns
  // "-- V" unless the millivolt reading is positive, and an invalid sample is
  // forced to that path too so "0.00V" can never reach the panel.
  const int readingMv = (power.sampleOk && power.voltageMv > 0) ? power.voltageMv : 0;
  const std::string reading = formatBatteryVoltage(readingMv);
  gU8g2->setFont(u8g2_font_helvB24_tr);
  gU8g2->drawStr(kMarginX, 118, reading.c_str());

  // ASCII only: the _tr fonts cover chars 32-127, so an em dash would render
  // as nothing.
  gU8g2->setFont(u8g2_font_helvB12_tr);
  gU8g2->drawStr(kMarginX, 176, "Deep sleep - re-checks every 10 min");
  gU8g2->drawStr(kMarginX, 196, "Charge the battery to continue");

  // This frame must reach the panel: the chip enters deep sleep right after
  // this call, so push unconditionally instead of trusting the change hash.
  invalidate();
  flushIfChanged();
}

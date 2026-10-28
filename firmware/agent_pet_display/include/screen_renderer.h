#pragma once

#include "models.h"

class U8G2;

class ScreenRenderer {
 public:
  void begin();
  void render(const DisplayState& display, const PowerState& power);
  // Independent full-screen notice drawn right before deep sleep on low
  // battery: LOW BATTERY title, the sleep/re-check schedule and the current
  // voltage reading. Completes its frame push synchronously — the caller
  // enters deep sleep immediately afterwards.
  void renderLowBatteryNotice(const PowerState& power);
  // Forces the next render() to push the frame even if it looks unchanged
  // (e.g. after the easter-egg player drew directly into the panel).
  void invalidate();
  // Direct access for full-screen players; nullptr before begin().
  U8G2* u8g2();
};

#pragma once

#include <stdint.h>

// Wi-Fi and bridge credentials live in include/secrets.h, which git ignores.
// Copy include/secrets.example.h to include/secrets.h and fill it in, or run
// tools/import-legacy-secrets.sh to reuse the values of an older checkout.
#if __has_include("secrets.h")
#include "secrets.h"
#else
static constexpr const char* kWifiSsid = "YOUR_WIFI_SSID";
static constexpr const char* kWifiPassword = "YOUR_WIFI_PASSWORD";
static constexpr const char* kBridgeUrl =
    "http://<bridge-host>:17366/esp32/poll?token=<token>";
#endif

static constexpr uint32_t kBridgePollMs = 3000;
static constexpr uint32_t kIdlePollMs = 10000;    // adaptive polling: quiet agent
static constexpr uint32_t kActiveHoldMs = 60000;  // stay at kBridgePollMs this long after a change
static constexpr uint32_t kFastPollMs = 600;      // egg request window
static constexpr uint32_t kPollRetryMs = 2000;    // first retry after a failed poll
static constexpr uint32_t kWifiReconnectMs = 10000;
static constexpr uint32_t kWifiReportMs = 15000;
static constexpr uint32_t kBatterySampleMs = 5000;
static constexpr uint32_t kEnvironmentSampleMs = 1000;  // climate + legacy callers
static constexpr uint32_t kClockSampleMs = 1000;        // RTC read; minute-level display, wake-budgeted
static constexpr uint32_t kClimateSampleMs = 5000;
static constexpr uint32_t kRtcResyncMs = 6UL * 3600UL * 1000UL;
static constexpr int kBootButtonPin = 0;
static constexpr int kAgentFocusButtonPin = 18;  // "KEY" on the board (factory firmware uses GPIO18)
static constexpr uint32_t kPageToggleDebounceMs = 250;
static constexpr uint32_t kButtonLongPressMs = 1500;
static constexpr uint32_t kButtonVeryLongPressMs = 5000;
static constexpr uint32_t kEggChordHoldMs = 2000;
static constexpr uint32_t kFocusAutoReturnMs = 45000;
// Round 3: content-driven redraws carry page updates; the periodic gate is a
// 1 s fallback so the CPU can idle between sensor/battery/agent changes.
static constexpr uint32_t kRenderIntervalMs = 1000;
// Keep light sleep off for this long after boot so USB console/flashing stay
// dependable (USB Serial/JTAG misbehaves under auto light sleep on IDF 4.4).
static constexpr uint32_t kPmArmDelayMs = 45000;
static constexpr uint32_t kSettingsSaveDelayMs = 2000;
static constexpr int kChargeSensePin = -1;

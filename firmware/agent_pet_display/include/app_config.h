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

#ifndef kBridgeMdnsHost
static constexpr const char* kBridgeMdnsHost = "cuis-Mac-mini";
#endif

static constexpr uint32_t kBridgePollMs = 3000;
static constexpr uint32_t kIdlePollMs = 10000;    // MEDIUM tier (1..10 min unchanged)
static constexpr uint32_t kSlowPollMs = 30000;    // SLOW tier (10..30 min unchanged)
static constexpr uint32_t kDeepIdlePollMs = 120000;  // DEEP tier (>30 min unchanged)
static constexpr uint32_t kActiveHoldMs = 60000;  // stay at FAST (kBridgePollMs) this long after a change
static constexpr uint32_t kMediumHoldMs = 10UL * 60UL * 1000UL;  // stay at MEDIUM up to 10 min
static constexpr uint32_t kSlowHoldMs = 30UL * 60UL * 1000UL;    // stay at SLOW up to 30 min, then DEEP
static constexpr uint32_t kFastPollMs = 600;      // egg request window
static constexpr uint32_t kPollRetryMs = 2000;    // first retry after a failed poll
static constexpr uint32_t kWifiReconnectMs = 10000;
static constexpr uint32_t kWifiReconnectMaxMs = 120000;
static constexpr uint32_t kWifiReportMs = 15000;
static constexpr uint32_t kBatterySampleMs = 5000;
static constexpr uint32_t kEnvironmentSampleMs = 1000;  // climate + legacy callers
static constexpr uint32_t kClockSampleMs = 1000;        // RTC initial/rollover read; minute-aligned once valid
static constexpr uint32_t kClimateSampleMs = 30000;
static constexpr float kClimateTempDeadbandC = 0.1f;
static constexpr float kClimateHumidityDeadbandPct = 1.0f;
static constexpr uint32_t kRtcResyncMs = 6UL * 3600UL * 1000UL;
static constexpr int kBootButtonPin = 0;
static constexpr int kAgentFocusButtonPin = 18;  // "KEY" on the board (factory firmware uses GPIO18)
static constexpr int kAudioPaCtrlPin = 46;       // onboard NS4150B speaker amplifier enable (LOW = shutdown)
static constexpr uint32_t kPageToggleDebounceMs = 250;
static constexpr uint32_t kButtonLongPressMs = 1500;
static constexpr uint32_t kButtonVeryLongPressMs = 5000;
static constexpr uint32_t kEggChordHoldMs = 2000;
static constexpr uint32_t kFocusAutoReturnMs = 45000;
// Round 3/4: content-driven redraws carry page updates; the periodic gate is a
// 60 s fallback so the CPU can idle between sensor/battery/agent changes.
static constexpr uint32_t kRenderIntervalMs = 60000;
// Keep light sleep off for this long after boot so USB console/flashing stay
// dependable (USB Serial/JTAG misbehaves under auto light sleep on IDF 4.4).
static constexpr uint32_t kPmArmDelayMs = 45000;
static constexpr uint32_t kSettingsSaveDelayMs = 2000;
static constexpr int kChargeSensePin = -1;
// Low-battery protection: 10 consecutive *valid* samples strictly below
// 3.60 V (5 s apart -> ~50 s low) enter a 600 s deep-sleep cycle. Strictly
// below: 3.60 V itself counts as recovered. Invalid samples reset the count.
static constexpr int kBatteryProtectionThresholdMv = 3600;
static constexpr int kBatteryProtectionConsecutive = 10;
static constexpr uint32_t kBatteryProtectionSleepS = 600;
// Recovery needs MORE than the entry threshold so a resting (load-free)
// cell cannot bounce straight back into the awake path it just collapsed
// from: two fresh valid readings at/above this, about 1 s apart, confirm
// the voltage has recovered; they do not detect a charge source. The extra
// awake second happens only inside the
// existing 600 s protection wake cycle.
static constexpr int kBatteryRecoveryThresholdMv = 3750;
// ADC noise sits around ±20 mV while the panel update costs a full frame
// write; the voltage shown on screen only moves when it drifts this far, so
// jitter cannot force a redraw every 5 s sample.
static constexpr int kBatteryDisplayDeadbandMv = 20;
// LCD wiring on this board (see screen_renderer.cpp / schematic).
static constexpr int kLcdSckPin = 11;
static constexpr int kLcdMosiPin = 12;
static constexpr int kLcdDcPin = 5;
static constexpr int kLcdCsPin = 40;
static constexpr int kLcdRstPin = 41;

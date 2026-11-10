#include "app_controller.h"

#include <Arduino.h>

#include "app_config.h"
#include "battery_monitor.h"
#include "battery_protection.h"
#include "bridge_client.h"
#include "button_actions.h"
#include "device_settings.h"
#include "egg_player.h"
#include "environment_monitor.h"
#include "firmware_version.h"
#include "focus_controller.h"
#include "low_battery_guard.h"
#include "models.h"
#include "net_worker.h"
#include "pet_state_machine.h"
#include "pm_controller.h"
#include "poll_policy.h"
#include "screen_renderer.h"

namespace {

ScreenRenderer gRenderer;
DeviceSettingsState gSettings;
AgentState gAgent;
PowerState gPower;
EnvironmentState gEnvironment;
NetworkState gNetwork;
NetSnapshot gSnapshot;
FocusRequestState gFocusRequest;
ButtonGestureState gBootButton;
ButtonGestureState gKeyButton;
ChordGestureState gEggChord;

uint64_t gLastClockSample = 0;
bool gClockSampled = false;
uint32_t gClockSampleWaitMs = kClockSampleMs;
uint64_t gLastClimateSample = 0;
bool gClimateSampled = false;
uint64_t gLastBatterySample = 0;
bool gBatterySampled = false;
uint64_t gLastRender = 0;
uint64_t gLastTelemetry = 0;
bool gTelemetrySent = false;
uint32_t gLastPollSeen = 0;
uint64_t gLastRtcWrite = 0;
bool gRtcWrittenThisBoot = false;
bool gSettingsDirty = false;
uint64_t gSettingsChangedAt = 0;
bool gForceRender = true;
bool gTelemetryDirty = true;
uint64_t gPmBootAtMs = 0;
bool gPmArmed = false;
int gPrevWifiConnected = -1;  // -1 unknown, 0/1 last seen connection state
SleepPacingMode gPacingMode = SleepPacingMode::Fast;

uint32_t gEggRequest = 0;       // counter sent to the bridge
uint64_t gEggRequestedAt = 0;   // waiting for the bridge to answer
bool gEggPending = false;
uint32_t gLastEggRev = 0;       // last egg command we started
bool gEggWasActive = false;

LowBatteryGuard gLowBatGuard;
// Voltage and climate currently shown on the panel (hysteresis-filtered, see
// sampleSensors): 0 / false until the first valid sample.
int gShownVoltageMv = 0;
bool gShownClimateValid = false;
float gShownTemperatureC = 0.0f;
float gShownHumidityPct = 0.0f;

void markSettingsDirty(uint64_t now64) {
  gSettingsDirty = true;
  gSettingsChangedAt = now64;
  gTelemetryDirty = true;
}

void saveSettingsIfDue(uint64_t now64) {
  if (gSettingsDirty && now64 - gSettingsChangedAt >= kSettingsSaveDelayMs) {
    saveDeviceSettings(gSettings);
    gSettingsDirty = false;
    Serial.println("[CFG] saved");
  }
}

void setPage(ScreenPage page, uint64_t now64) {
  if (gSettings.page == page) {
    return;
  }
  gSettings.page = page;
  markSettingsDirty(now64);
  gForceRender = true;
  Serial.printf("[UI] page=%s\n", screenPageName(page));
}

void cycleAgentFocus(uint32_t now32) {
  advanceFocusSelection(gFocusRequest, gAgent, now32, kFocusAutoReturnMs);
  netWorkerSetFocus(gFocusRequest);
  gForceRender = true;
  Serial.printf("[UI] focus=%s index=%d count=%d\n",
                gFocusRequest.mode == LocalFocusMode::Pinned ? "pinned" : "auto",
                gFocusRequest.visibleIndex,
                gFocusRequest.visibleCount);
}

void requestEgg(uint64_t now64) {
  gEggRequest += 1;
  gEggRequestedAt = now64;
  gEggPending = true;
  gTelemetryDirty = true;
  netWorkerSetFastPoll(true);
  Serial.printf("[EGG] requested #%lu\n", static_cast<unsigned long>(gEggRequest));
}

void handleAction(ButtonUiAction action, uint64_t now64, uint32_t now32) {
  if (action == ButtonUiAction::None) {
    return;
  }
  const bool wasDeep = gPacingMode == SleepPacingMode::Deep;
  if (gPacingMode != SleepPacingMode::Fast || !gNetwork.wifiConnected) {
    netWorkerWake();  // user interaction wakes sleep pacing to FAST and resets Wi-Fi backoff
    if (gPacingMode != SleepPacingMode::Fast) {
      gPacingMode = SleepPacingMode::Fast;
      gForceRender = true;
      Serial.println("[UI] button wake -> FAST");
    }
  }
  if (eggActive()) {
    // Any button stops the show.
    eggStop();
    return;
  }
  if (wasDeep && action != ButtonUiAction::Egg) {
    // First button press from DEEP (>30 min idle) wakes the board into FAST
    // refresh on the current page without unexpectedly flipping away.
    return;
  }
  switch (contextualAction(action, gSettings.page)) {
    case ButtonUiAction::Page:
      setPage(nextScreenPage(gSettings.page), now64);
      break;
    case ButtonUiAction::AgentFocus:
      cycleAgentFocus(now32);
      break;
    case ButtonUiAction::ClockStyle:
      gSettings.display.clockStyle = nextClockStyle(gSettings.display.clockStyle);
      markSettingsDirty(now64);
      gForceRender = true;
      Serial.printf("[UI] clock=%s\n", clockStyleName(gSettings.display.clockStyle));
      break;
    case ButtonUiAction::Egg:
      requestEgg(now64);
      break;
    default:
      break;
  }
}

void readButtons(uint64_t now64, uint32_t now32) {
  const bool bootPressed = digitalRead(kBootButtonPin) == LOW;
  const bool keyPressed = digitalRead(kAgentFocusButtonPin) == LOW;

  const ButtonUiAction bootAction = updateButtonGestureEx(gBootButton,
                                                          bootPressed,
                                                          now32,
                                                          kButtonLongPressMs,
                                                          kButtonVeryLongPressMs,
                                                          ButtonUiAction::Page,
                                                          ButtonUiAction::AgentFocus,
                                                          ButtonUiAction::Egg);
  const ButtonUiAction keyAction = updateButtonGestureEx(gKeyButton,
                                                         keyPressed,
                                                         now32,
                                                         kButtonLongPressMs,
                                                         0,
                                                         ButtonUiAction::AgentFocus,
                                                         ButtonUiAction::AgentFocus,
                                                         ButtonUiAction::None);
  const bool chordFired =
      updateChordGesture(gEggChord, gBootButton, gKeyButton, bootPressed, keyPressed, now32, kEggChordHoldMs);
  if (chordFired) {
    handleAction(ButtonUiAction::Egg, now64, now32);
    return;
  }
  if (gEggChord.active) {
    return;  // both buttons are down: single-button gestures are swallowed
  }
  handleAction(bootAction, now64, now32);
  handleAction(keyAction, now64, now32);
}

// Ten consecutive valid samples strictly below 3.60 V (~50 s at 5 s per
// sample): put the notice on the panel, then deep sleep for 600 s at a time.
// Never returns (esp_deep_sleep_start); a reset or power cycle is the only
// way out besides a recovered battery.
void enterBatteryProtection() {
  Serial.printf("[BAT-PROT] %d consecutive valid samples < %dmV: protecting\n",
                gLowBatGuard.consecutiveLow(), LowBatteryGuard::kThresholdMv);
  if (eggActive()) {
    eggStop();  // the 240 MHz show must not straddle the sleep entry
  }
  netWorkerSetFastPoll(false);
  netWorkerStopForDeepSleep();
  pmPrepareDeepSleep();  // auto light sleep off: nothing may race the entry
  // Notice last: low battery, deep sleep and the raw reading (not the
  // deadband-filtered value the top bar shows).
  gRenderer.renderLowBatteryNotice(gPower);
  delay(200);  // let the SPI frame transfer finish before the power domains drop
  batteryProtectionEnterDeepSleep();  // holds LCD pads, arms the 600 s RTC timer
}

void sampleSensors(uint64_t now64) {
  if (!gClockSampled || now64 - gLastClockSample >= gClockSampleWaitMs) {
    // Round 3/4: no page shows seconds any more, so redraw on minute changes
    // and on the invalid->valid transition (SYNCING TIME -> real time), and
    // sleep the RTC I2C read until the next minute boundary once valid.
    const int prevMinute = gEnvironment.clockValid ? gEnvironment.hour * 60 + gEnvironment.minute : -1;
    const bool prevValid = gEnvironment.clockValid;
    sampleClock(gEnvironment, gSettings.tz.c_str());
    if (gEnvironment.clockValid != prevValid ||
        (gEnvironment.clockValid && gEnvironment.hour * 60 + gEnvironment.minute != prevMinute)) {
      gForceRender = true;
    }
    gLastClockSample = now64;
    gClockSampled = true;
    gClockSampleWaitMs = nextClockSampleDelayMs(gEnvironment.clockValid, gEnvironment.second);
  }
  const uint32_t climateInterval = (gPacingMode == SleepPacingMode::Deep) ? 120000U : kClimateSampleMs;
  if (!gClimateSampled || now64 - gLastClimateSample >= climateInterval) {
    sampleClimate(gEnvironment);
    if (climateDisplayChanged(gEnvironment.climateValid,
                              gEnvironment.temperatureC,
                              gEnvironment.humidityPct,
                              gShownClimateValid,
                              gShownTemperatureC,
                              gShownHumidityPct)) {
      gShownClimateValid = gEnvironment.climateValid;
      gShownTemperatureC = gEnvironment.temperatureC;
      gShownHumidityPct = gEnvironment.humidityPct;
      gForceRender = true;
    }
    gLastClimateSample = now64;
    gClimateSampled = true;
    gTelemetryDirty = true;
  }
  const uint32_t batteryInterval = (gPacingMode == SleepPacingMode::Deep) ? 30000U : kBatterySampleMs;
  if (!gBatterySampled || now64 - gLastBatterySample >= batteryInterval) {
    const PowerState prevPower = gPower;
    gPower = samplePowerState(false);
    // Display hysteresis: the panel shows the last value that crossed
    // kBatteryDisplayDeadbandMv, so ADC noise around a 0.01 V step cannot
    // force a redraw every 5 s sample. Telemetry and the guard use the raw
    // reading; displayed voltage and percent follow this filtered value.
    const int shownBefore = gShownVoltageMv;
    const int deltaMv = gPower.voltageMv > shownBefore ? gPower.voltageMv - shownBefore
                                                       : shownBefore - gPower.voltageMv;
    if (!(gPower.sampleOk && shownBefore > 0 && deltaMv < kBatteryDisplayDeadbandMv)) {
      gShownVoltageMv = gPower.voltageMv;  // crossing, first value, or invalid (0)
    }
    if (gPower.lowBattery != prevPower.lowBattery || gPower.charging != prevPower.charging ||
        gShownVoltageMv != shownBefore) {
      gForceRender = true;
    }
    gLastBatterySample = now64;
    gBatterySampled = true;
    gTelemetryDirty = true;
    if (gLowBatGuard.feed(gPower.sampleOk, gPower.voltageMv) == LowBatteryAction::EnterProtection) {
      enterBatteryProtection();  // does not return
    }
  }
  // SNTP sets the system clock asynchronously; mirror it into the RTC now and then.
  if (systemClockPlausible() && (!gRtcWrittenThisBoot || now64 - gLastRtcWrite >= kRtcResyncMs)) {
    if (writeRtcFromSystemClock()) {
      gRtcWrittenThisBoot = true;
      gLastRtcWrite = now64;
      gClockSampled = false;  // re-sample RTC immediately after sync
      Serial.println("[TIME] RTC synced from NTP (UTC)");
    }
  }
}

void applyBridge(uint64_t now64, uint32_t now32) {
  if (!netWorkerSnapshot(gSnapshot)) {
    return;
  }
  const int wifiNow = gSnapshot.network.wifiConnected ? 1 : 0;
  if (wifiNow != gPrevWifiConnected) {
    gPrevWifiConnected = wifiNow;
    gForceRender = true;
  }
  if (gSnapshot.pacingMode != gPacingMode) {
    gPacingMode = gSnapshot.pacingMode;
    gForceRender = true;
  }
  gNetwork = gSnapshot.network;
  if (gSnapshot.pollSeq == gLastPollSeen) {
    if (!gNetwork.wifiConnected) {
      gAgent.connected = false;
    }
    return;
  }
  gLastPollSeen = gSnapshot.pollSeq;
  gAgent = gSnapshot.agent;
  gForceRender = true;
  if (!gSnapshot.lastPollOk) {
    return;
  }
  syncFocusSelectionFromBridge(gFocusRequest, gAgent, now32, kFocusAutoReturnMs);
  netWorkerSetFocus(gFocusRequest);

  const BridgeCommands& commands = gSnapshot.commands;
  if (applyDisplayCommand(gSettings, commands.display)) {
    markSettingsDirty(now64);
    Serial.printf("[CFG] from bridge rev=%lu clock=%s page=%s\n",
                  static_cast<unsigned long>(gSettings.displayRev),
                  clockStyleName(gSettings.display.clockStyle),
                  screenPageName(gSettings.page));
  }
  if (commands.hasTz && applyTzCommand(gSettings, commands.tz)) {
    markSettingsDirty(now64);
    gClockSampled = false;  // re-sample local clock immediately on timezone change
    Serial.printf("[TIME] tz=%s\n", gSettings.tz.c_str());
  }
  if (commands.egg.present && commands.egg.rev != gLastEggRev) {
    gLastEggRev = commands.egg.rev;
    gEggPending = false;
    gTelemetryDirty = true;
    netWorkerSetFastPoll(false);
    eggStartStream(commands.egg, gSnapshot, now32);
  }
}

void updateTelemetry(uint64_t now64) {
  if (!gTelemetryDirty && gTelemetrySent && now64 - gLastTelemetry < kBridgePollMs) {
    return;
  }
  gTelemetryDirty = false;
  gTelemetrySent = true;
  gLastTelemetry = now64;
  DeviceTelemetry telemetry{};
  telemetry.firmware = kFirmwareVersion;
  telemetry.batteryValid = gPower.sampleOk;
  telemetry.batteryPercent = gPower.percent;
  telemetry.batteryMv = gPower.voltageMv;
  telemetry.charging = gPower.charging;
  telemetry.climateValid = gEnvironment.climateValid;
  telemetry.temperatureC = gEnvironment.temperatureC;
  telemetry.humidityPct = gEnvironment.humidityPct;
  telemetry.rssi = gNetwork.wifiConnected ? gNetwork.rssi : 0;
  telemetry.uptimeS = static_cast<uint32_t>(now64 / 1000ULL);
  telemetry.page = gSettings.page;
  telemetry.clockStyle = gSettings.display.clockStyle;
  telemetry.settingsRev = gSettings.displayRev;
  telemetry.eggRev = gLastEggRev;
  telemetry.freeHeap = ESP.getFreeHeap();
  telemetry.timeValid = gEnvironment.clockValid;
  telemetry.eggRequest = gEggRequest;
  netWorkerSetTelemetry(telemetry);
}

void checkEggRequestTimeout(uint64_t now64, uint32_t now32) {
  // The bridge had ~3 s to announce an animation; otherwise play the built-in one.
  if (gEggPending && now64 - gEggRequestedAt > 3000) {
    gEggPending = false;
    netWorkerSetFastPoll(false);
    if (!eggActive()) {
      eggStartBuiltin(now32);
    }
  }
}

uint32_t remainingToDeadline(uint64_t now64, uint64_t baseMs, uint32_t intervalMs) {
  const uint64_t elapsed = now64 >= baseMs ? (now64 - baseMs) : 0ULL;
  if (elapsed >= intervalMs) {
    return 0;
  }
  return static_cast<uint32_t>(intervalMs - elapsed);
}

uint32_t computeIdleSleepMs(uint64_t now64, uint32_t now32) {
  if (gForceRender || !gClockSampled || !gClimateSampled || !gBatterySampled) {
    return 20;
  }
  uint32_t sleepMs = 30000;
  auto clampMin = [&sleepMs](uint32_t candidate) {
    if (candidate < sleepMs) {
      sleepMs = candidate;
    }
  };

  clampMin(remainingToDeadline(now64, gLastClockSample, gClockSampleWaitMs));
  const uint32_t batteryInterval = (gPacingMode == SleepPacingMode::Deep) ? 30000U : kBatterySampleMs;
  clampMin(remainingToDeadline(now64, gLastBatterySample, batteryInterval));
  const uint32_t climateInterval = (gPacingMode == SleepPacingMode::Deep) ? 120000U : kClimateSampleMs;
  clampMin(remainingToDeadline(now64, gLastClimateSample, climateInterval));
  clampMin(remainingToDeadline(now64, gLastRender, kRenderIntervalMs));

  if (!gPmArmed) {
    clampMin(remainingToDeadline(now64, gPmBootAtMs, kPmArmDelayMs));
  }
  if (gSettingsDirty) {
    clampMin(remainingToDeadline(now64, gSettingsChangedAt, kSettingsSaveDelayMs));
  }
  if (gFocusRequest.mode == LocalFocusMode::Pinned) {
    const int32_t focusRem = static_cast<int32_t>(gFocusRequest.expiresAtMs - now32);
    clampMin(focusRem > 0 ? static_cast<uint32_t>(focusRem) : 0U);
  }
  return sleepMs < 20 ? 20 : sleepMs;
}

}  // namespace

void AppController::begin() {
  loadDeviceSettings(gSettings);
  gRenderer.begin();
  gAgent.connected = false;
  gPower = derivePowerState(3800, false);
  beginEnvironmentMonitor();
  if (!gSettings.rtcIsUtc) {
    const bool migrated = migrateLegacyRtcToUtc();
    gSettings.rtcIsUtc = true;
    saveDeviceSettings(gSettings);
    Serial.printf("[TIME] RTC convention -> UTC (%s)\n", migrated ? "converted from CST" : "nothing to convert");
  }
  pinMode(kBootButtonPin, INPUT_PULLUP);
  pinMode(kAgentFocusButtonPin, INPUT_PULLUP);
  pmRegisterAppTaskWakeup();
  // pmBegin() runs once in main.cpp setup(); here we only arm the wake pads + idle hook.
  pmSetupButtonWakeup(kBootButtonPin, kAgentFocusButtonPin);
  gPmBootAtMs = pmMonotonicMs();
  Serial.printf("[BOOT] firmware=%s\n", kFirmwareVersion);
  Serial.printf("[BOOT] WiFi SSID=%s\n", kWifiSsid);
  Serial.printf("[BOOT] Bridge URL=%s\n", redactUrlToken(kBridgeUrl).c_str());
  Serial.printf("[BOOT] page=%s clock=%s tz=%s\n",
                screenPageName(gSettings.page),
                clockStyleName(gSettings.display.clockStyle),
                gSettings.tz.empty() ? "(default)" : gSettings.tz.c_str());
  netWorkerBegin(kWifiSsid, kWifiPassword, kBridgeUrl);
  netWorkerSetFocus(gFocusRequest);
}

void AppController::tick() {
  const uint64_t now64 = pmMonotonicMs();
  const uint32_t now32 = millis();

  pmTick(now32);
  if (!gPmArmed && !eggActive() && now64 - gPmBootAtMs >= kPmArmDelayMs) {
    pmEnableLightSleep();
    gPmArmed = pmLightSleepArmed();
  }

  readButtons(now64, now32);
  applyBridge(now64, now32);
  sampleSensors(now64);
  updateTelemetry(now64);
  checkEggRequestTimeout(now64, now32);
  saveSettingsIfDue(now64);

  const LocalFocusMode focusBefore = gFocusRequest.mode;
  expireFocusIfNeeded(gFocusRequest, now32);
  if (focusBefore != gFocusRequest.mode) {
    netWorkerSetFocus(gFocusRequest);  // pinned agent timed out back to AUTO
    gForceRender = true;
  }

  U8G2* panel = gRenderer.u8g2();
  if (eggActive() && panel) {
    if (!gEggWasActive) {
      pmEggPerformanceOn();
      gRenderer.setPanelHighPowerMode(true);
    }
    eggTick(*panel, now32);
    gEggWasActive = true;
    delay(2);
    return;
  }
  if (gEggWasActive) {
    gEggWasActive = false;
    gRenderer.setPanelHighPowerMode(false);
    pmEggPerformanceOff();
    gRenderer.invalidate();
    gForceRender = true;
  }

  if (gForceRender || now64 - gLastRender >= kRenderIntervalMs) {
    PowerState displayPower = gPower;
    displayPower.voltageMv = gShownVoltageMv;
    displayPower.percent = gPower.sampleOk ? mapBatteryPercent(gShownVoltageMv) : gPower.percent;
    EnvironmentState displayEnv = gEnvironment;
    displayEnv.climateValid = gShownClimateValid;
    displayEnv.temperatureC = gShownTemperatureC;
    displayEnv.humidityPct = gShownHumidityPct;
    const DisplayState view = deriveDisplayState(gAgent, displayPower, displayEnv, gSettings.page,
                                                 gNetwork, gSettings.display, gPacingMode);
    gRenderer.render(view, displayPower);
    gLastRender = now64;
    gForceRender = false;
  }

  // When a button is held (or awaiting an egg response), sample at 50 ms for
  // crisp gesture timing; when idle, sleep until the next sensor/clock deadline
  // and wake immediately via Core 1's GPIO wake + idle hook or netTask notify.
  const bool buttonsActive = gBootButton.wasPressed || gKeyButton.wasPressed || gEggChord.active || gEggPending;
  if (buttonsActive) {
    delay(50);
  } else {
    pmWaitForAppWake(computeIdleSleepMs(pmMonotonicMs(), millis()));
  }
}

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

uint32_t gLastClockSample = 0;
uint32_t gClockSampleWaitMs = kClockSampleMs;
uint32_t gLastClimateSample = 0;
uint32_t gLastBatterySample = 0;
uint32_t gLastRender = 0;
uint32_t gLastTelemetry = 0;
uint32_t gLastPollSeen = 0;
uint32_t gLastRtcWrite = 0;
bool gRtcWrittenThisBoot = false;
bool gSettingsDirty = false;
uint32_t gSettingsChangedAt = 0;
bool gForceRender = true;
bool gTelemetryDirty = true;
uint32_t gPmBootAtMs = 0;
bool gPmArmed = false;
int gPrevWifiConnected = -1;  // -1 unknown, 0/1 last seen connection state
SleepPacingMode gPacingMode = SleepPacingMode::Fast;

uint32_t gEggRequest = 0;       // counter sent to the bridge
uint32_t gEggRequestedAt = 0;   // waiting for the bridge to answer
uint32_t gLastEggRev = 0;       // last egg command we started
bool gEggWasActive = false;

LowBatteryGuard gLowBatGuard;
// Voltage and climate currently shown on the panel (hysteresis-filtered, see
// sampleSensors): 0 / false until the first valid sample.
int gShownVoltageMv = 0;
bool gShownClimateValid = false;
float gShownTemperatureC = 0.0f;
float gShownHumidityPct = 0.0f;

void markSettingsDirty(uint32_t now) {
  gSettingsDirty = true;
  gSettingsChangedAt = now;
  gTelemetryDirty = true;
}

void saveSettingsIfDue(uint32_t now) {
  if (gSettingsDirty && now - gSettingsChangedAt >= kSettingsSaveDelayMs) {
    saveDeviceSettings(gSettings);
    gSettingsDirty = false;
    Serial.println("[CFG] saved");
  }
}

void setPage(ScreenPage page, uint32_t now) {
  if (gSettings.page == page) {
    return;
  }
  gSettings.page = page;
  markSettingsDirty(now);
  gForceRender = true;
  Serial.printf("[UI] page=%s\n", screenPageName(page));
}

void cycleAgentFocus(uint32_t now) {
  advanceFocusSelection(gFocusRequest, gAgent, now, kFocusAutoReturnMs);
  netWorkerSetFocus(gFocusRequest);
  gForceRender = true;
  Serial.printf("[UI] focus=%s index=%d count=%d\n",
                gFocusRequest.mode == LocalFocusMode::Pinned ? "pinned" : "auto",
                gFocusRequest.visibleIndex,
                gFocusRequest.visibleCount);
}

void requestEgg(uint32_t now) {
  gEggRequest += 1;
  gEggRequestedAt = now;
  gTelemetryDirty = true;
  netWorkerSetFastPoll(true);
  Serial.printf("[EGG] requested #%lu\n", static_cast<unsigned long>(gEggRequest));
}

void handleAction(ButtonUiAction action, uint32_t now) {
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
      setPage(nextScreenPage(gSettings.page), now);
      break;
    case ButtonUiAction::AgentFocus:
      cycleAgentFocus(now);
      break;
    case ButtonUiAction::ClockStyle:
      gSettings.display.clockStyle = nextClockStyle(gSettings.display.clockStyle);
      markSettingsDirty(now);
      gForceRender = true;
      Serial.printf("[UI] clock=%s\n", clockStyleName(gSettings.display.clockStyle));
      break;
    case ButtonUiAction::Egg:
      requestEgg(now);
      break;
    default:
      break;
  }
}

void readButtons(uint32_t now) {
  const bool bootPressed = digitalRead(kBootButtonPin) == LOW;
  const bool keyPressed = digitalRead(kAgentFocusButtonPin) == LOW;

  const ButtonUiAction bootAction = updateButtonGestureEx(gBootButton,
                                                          bootPressed,
                                                          now,
                                                          kButtonLongPressMs,
                                                          kButtonVeryLongPressMs,
                                                          ButtonUiAction::Page,
                                                          ButtonUiAction::AgentFocus,
                                                          ButtonUiAction::Egg);
  const ButtonUiAction keyAction = updateButtonGestureEx(gKeyButton,
                                                         keyPressed,
                                                         now,
                                                         kButtonLongPressMs,
                                                         0,
                                                         ButtonUiAction::AgentFocus,
                                                         ButtonUiAction::AgentFocus,
                                                         ButtonUiAction::None);
  const bool chordFired =
      updateChordGesture(gEggChord, gBootButton, gKeyButton, bootPressed, keyPressed, now, kEggChordHoldMs);
  if (chordFired) {
    handleAction(ButtonUiAction::Egg, now);
    return;
  }
  if (gEggChord.active) {
    return;  // both buttons are down: single-button gestures are swallowed
  }
  handleAction(bootAction, now);
  handleAction(keyAction, now);
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

void sampleSensors(uint32_t now) {
  if (gLastClockSample == 0 || now - gLastClockSample >= gClockSampleWaitMs) {
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
    gLastClockSample = now;
    gClockSampleWaitMs = nextClockSampleDelayMs(gEnvironment.clockValid, gEnvironment.second);
  }
  const uint32_t climateInterval = (gPacingMode == SleepPacingMode::Deep) ? 120000U : kClimateSampleMs;
  if (gLastClimateSample == 0 || now - gLastClimateSample >= climateInterval) {
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
    gLastClimateSample = now;
    gTelemetryDirty = true;
  }
  const uint32_t batteryInterval = (gPacingMode == SleepPacingMode::Deep) ? 30000U : kBatterySampleMs;
  if (gLastBatterySample == 0 || now - gLastBatterySample >= batteryInterval) {
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
    // No raw-percent comparison here on purpose: the displayed percent now
    // derives from gShownVoltageMv, so it changes exactly when this value
    // does, and raw curve steps inside the deadband redraw nothing.
    if (gPower.lowBattery != prevPower.lowBattery || gPower.charging != prevPower.charging ||
        gShownVoltageMv != shownBefore) {
      gForceRender = true;
    }
    gLastBatterySample = now;
    gTelemetryDirty = true;
    // Protection counts raw valid samples only: invalid ones reset it above
    // (feed()), so a dead sensor can never be read as an empty battery.
    if (gLowBatGuard.feed(gPower.sampleOk, gPower.voltageMv) == LowBatteryAction::EnterProtection) {
      enterBatteryProtection();  // does not return
    }
  }
  // SNTP sets the system clock asynchronously; mirror it into the RTC now and then.
  if (systemClockPlausible() && (!gRtcWrittenThisBoot || now - gLastRtcWrite >= kRtcResyncMs)) {
    if (writeRtcFromSystemClock()) {
      gRtcWrittenThisBoot = true;
      gLastRtcWrite = now;
      gLastClockSample = 0;  // re-sample RTC immediately after sync
      Serial.println("[TIME] RTC synced from NTP (UTC)");
    }
  }
}

void applyBridge(uint32_t now) {
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
  syncFocusSelectionFromBridge(gFocusRequest, gAgent, now, kFocusAutoReturnMs);
  netWorkerSetFocus(gFocusRequest);

  const BridgeCommands& commands = gSnapshot.commands;
  if (applyDisplayCommand(gSettings, commands.display)) {
    markSettingsDirty(now);
    Serial.printf("[CFG] from bridge rev=%lu clock=%s page=%s\n",
                  static_cast<unsigned long>(gSettings.displayRev),
                  clockStyleName(gSettings.display.clockStyle),
                  screenPageName(gSettings.page));
  }
  if (commands.hasTz && applyTzCommand(gSettings, commands.tz)) {
    markSettingsDirty(now);
    gLastClockSample = 0;  // re-sample local clock immediately on timezone change
    Serial.printf("[TIME] tz=%s\n", gSettings.tz.c_str());
  }
  if (commands.egg.present && commands.egg.rev != gLastEggRev) {
    gLastEggRev = commands.egg.rev;
    gEggRequestedAt = 0;
    gTelemetryDirty = true;
    netWorkerSetFastPoll(false);
    eggStartStream(commands.egg, gSnapshot, now);
  }
}

void updateTelemetry(uint32_t now) {
  if (!gTelemetryDirty && gLastTelemetry != 0 && now - gLastTelemetry < kBridgePollMs) {
    return;
  }
  gTelemetryDirty = false;
  gLastTelemetry = now;
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
  telemetry.uptimeS = now / 1000;
  telemetry.page = gSettings.page;
  telemetry.clockStyle = gSettings.display.clockStyle;
  telemetry.settingsRev = gSettings.displayRev;
  telemetry.eggRev = gLastEggRev;
  telemetry.freeHeap = ESP.getFreeHeap();
  telemetry.timeValid = gEnvironment.clockValid;
  telemetry.eggRequest = gEggRequest;
  netWorkerSetTelemetry(telemetry);
}

void checkEggRequestTimeout(uint32_t now) {
  // The bridge had ~3 s to announce an animation; otherwise play the built-in one.
  if (gEggRequestedAt != 0 && now - gEggRequestedAt > 3000) {
    gEggRequestedAt = 0;
    netWorkerSetFastPoll(false);
    if (!eggActive()) {
      eggStartBuiltin(now);
    }
  }
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
  // pmBegin() runs once in main.cpp setup(); here we only arm the wake pads.
  pmSetupButtonWakeup(kBootButtonPin, kAgentFocusButtonPin);
  gPmBootAtMs = millis();
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
  const uint32_t now = millis();

  pmTick(now);
  // Defer arming while the egg plays: it may cross the 45 s mark at 240 MHz
  // with light sleep off, and re-arming mid-playback would throttle it. After
  // the egg ends, pmEggPerformanceOff() restores 80 MHz first, then this gate
  // arms light sleep on the next tick. gPmArmed mirrors the real outcome so a
  // failed configure is retried instead of reported as success.
  if (!gPmArmed && !eggActive() && now - gPmBootAtMs >= kPmArmDelayMs) {
    pmEnableLightSleep();
    gPmArmed = pmLightSleepArmed();
  }

  readButtons(now);
  applyBridge(now);
  sampleSensors(now);
  updateTelemetry(now);
  checkEggRequestTimeout(now);
  saveSettingsIfDue(now);

  const LocalFocusMode focusBefore = gFocusRequest.mode;
  expireFocusIfNeeded(gFocusRequest, now);
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
    eggTick(*panel, now);
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

  if (gForceRender || now - gLastRender >= kRenderIntervalMs) {
    // Everything drawn derives from the hysteresis-filtered voltage and climate:
    // the nonlinear battery percent curve and 0.1 C / 1% rounding amplify small
    // sensor steps, so the panel follows the deadband-filtered values while raw
    // readings keep driving telemetry and the protection guard untouched.
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
    gLastRender = now;
    gForceRender = false;
  }
  // Idle pacing: buttons wake the chip from light sleep via the RTC pads; when
  // a button is actively held (or during the egg request window), poll every
  // 50 ms for crisp gesture timing, and relax to 200 ms when idle to halve CPU
  // wakeups.
  const bool buttonsActive = gBootButton.wasPressed || gKeyButton.wasPressed || gEggChord.active ||
                             gEggRequestedAt != 0;
  delay(buttonsActive ? 50 : 200);
}

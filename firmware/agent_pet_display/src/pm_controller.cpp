#include "pm_controller.h"

#include <math.h>

// A/B switch: waveshare_rlcd_pmoff builds the same PM-enabled SDK and the same
// code path with light sleep compiled out, so current readings differ only by
// the light-sleep bit (see platformio.ini).
#ifdef RLCD_PM_DISABLE_LIGHT_SLEEP
constexpr bool kLightSleepCompileEnabled = false;
#else
constexpr bool kLightSleepCompileEnabled = true;
#endif

// AwakeStats is pure logic and identical on host and device builds.
void AwakeStats::begin(uint32_t cycles, int64_t wallUs) {
  resetWindow(cycles, wallUs);
}

void AwakeStats::resetWindow(uint32_t cycles, int64_t wallUs) {
  lastCycles_ = cycles;
  lastWallUs_ = wallUs;
  awakeCycles_ = 0;
  wallUs_ = wallUs;
  windowStartUs_ = wallUs;
}

void AwakeStats::sample(uint32_t cycles, int64_t wallUs) {
  // Wrap-safe 32-bit cycle delta; while sleeping the loop is stopped and no
  // samples are taken, so only awake cycles are added.
  awakeCycles_ += static_cast<int64_t>(cycles - lastCycles_);
  lastCycles_ = cycles;
  wallUs_ = wallUs;
}

int64_t AwakeStats::awakeUs(int cpuFreqMhz) const {
  if (cpuFreqMhz <= 0) {
    return 0;
  }
  return awakeCycles_ / cpuFreqMhz;
}

int AwakeStats::awakePercent(int cpuFreqMhz) const {
  const int64_t window = windowUs();
  if (window <= 0) {
    return -1;
  }
  const int64_t awake = awakeUs(cpuFreqMhz);
  const int percent = static_cast<int>((awake * 100) / window);
  return percent > 100 ? 100 : percent;
}

#ifdef ARDUINO

#include <Arduino.h>
#include "driver/gpio.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "xtensa/hal.h"

// Counted inside esp_light_sleep_start by the round-3 SDK patch (see
// tools/pm-libs/). Weak so the same firmware also links against the stock
// SDK, where the counter simply reports as unavailable.
extern "C" uint32_t esp_sleep_get_light_sleep_count(void) __attribute__((weak));

namespace {

constexpr int kIdleFreqMhz = 80;
constexpr int kEggFreqMhz = 240;
// USB Serial/JTAG is unreliable under auto light sleep (ESP-IDF 4.4 docs):
// keep a quiet boot window with light sleep off so boot logs and flashing
// stay dependable, then arm it.
constexpr uint32_t kBootWindowMs = 45000;
constexpr uint32_t kStatsWindowMs = 30000;
// "PM33": marks the RTC-data slot as written by this build.
constexpr uint32_t kRtcMagic = 0x504D3333U;

bool gPmOk = false;
bool gLightSleepWanted = false;
bool gArmSettled = false;
bool gLocksDumped = false;
AwakeStats gStats;

// Survives chip resets (not power cycles): evidence for the last stats window
// even if the console dropped while the chip was sleeping.
RTC_DATA_ATTR float gLastWindowAwakePercent = -1.0f;
RTC_DATA_ATTR uint32_t gRtcMagic = 0;

esp_err_t applyPmConfig(int maxMhz, int minMhz, bool lightSleep) {
  esp_pm_config_esp32s3_t cfg = {};
  cfg.max_freq_mhz = maxMhz;
  cfg.min_freq_mhz = minMhz;
  cfg.light_sleep_enable = lightSleep;
  const esp_err_t err = esp_pm_configure(&cfg);
  if (err != ESP_OK) {
    Serial.printf("[PM] esp_pm_configure(max=%d,min=%d,ls=%d) -> 0x%x\n", maxMhz, minMhz,
                  lightSleep ? 1 : 0, err);
  }
  return err;
}

}  // namespace

void pmBegin() {
  if (gRtcMagic == kRtcMagic) {
    Serial.printf("[PM] prev window awake %d%% (survived reset)\n",
                  static_cast<int>(gLastWindowAwakePercent + 0.5f));
  }
  gRtcMagic = kRtcMagic;
  gPmOk = applyPmConfig(kIdleFreqMhz, kIdleFreqMhz, false) == ESP_OK;
  if (gPmOk) {
    Serial.printf("[PM] SDK supports esp_pm; idle %d MHz, light sleep after %lus\n", kIdleFreqMhz,
                  static_cast<unsigned long>(kBootWindowMs / 1000));
  } else {
    const bool ok = setCpuFrequencyMhz(kIdleFreqMhz);
    Serial.printf("[PM] SDK has no esp_pm support; fallback setCpuFrequencyMhz(%d) -> %s\n",
                  kIdleFreqMhz, ok ? "ok" : "FAILED");
  }
  gStats.begin(xthal_get_ccount(), esp_timer_get_time());
}

bool pmSupported() {
  return gPmOk;
}

void pmEnableLightSleep() {
  if (!gPmOk || gArmSettled) {
    return;
  }
  gLightSleepWanted = kLightSleepCompileEnabled;
  const esp_err_t err = applyPmConfig(kIdleFreqMhz, kIdleFreqMhz, kLightSleepCompileEnabled);
  gArmSettled = (err == ESP_OK);  // false -> caller retries on a later tick
  if (gArmSettled) {
    Serial.printf("[PM] idle policy settled: 80 MHz, light sleep %s\n",
                  gLightSleepWanted ? "ARMED" : "OFF (A/B build)");
  }
}

// True once the idle power policy is in place (armed, or deliberately off in
// the A/B build); the app uses it to stop retrying, not as sleep evidence.
bool pmLightSleepArmed() {
  return gArmSettled;
}

void pmEggPerformanceOn() {
  if (gPmOk) {
    const esp_err_t err = applyPmConfig(kEggFreqMhz, kIdleFreqMhz, false);
    gArmSettled = false;  // idle policy is suspended while playing
    Serial.printf("[PM] egg: 240 MHz, light sleep off -> %s\n", err == ESP_OK ? "ok" : "FAILED");
  } else {
    const bool ok = setCpuFrequencyMhz(kEggFreqMhz);
    Serial.printf("[PM] egg: setCpuFrequencyMhz(240) -> %s\n", ok ? "ok" : "FAILED");
  }
}

void pmEggPerformanceOff() {
  if (gPmOk) {
    const esp_err_t err = applyPmConfig(kIdleFreqMhz, kIdleFreqMhz, gLightSleepWanted);
    gArmSettled = (err == ESP_OK);  // wanted state restored (or still off in A/B build)
    Serial.printf("[PM] egg end: 80 MHz, light sleep %s -> %s\n", gLightSleepWanted ? "on" : "off",
                  err == ESP_OK ? "ok" : "FAILED");
  } else {
    const bool ok = setCpuFrequencyMhz(kIdleFreqMhz);
    Serial.printf("[PM] egg end: setCpuFrequencyMhz(80) -> %s\n", ok ? "ok" : "FAILED");
  }
}

void pmSetupButtonWakeup(int bootPin, int keyPin) {
  // IDF 4.4 light-sleep GPIO wake, and nothing else: attachInterrupt would
  // rewrite the same pin.int_type field (esp32-hal-gpio.c -> gpio_set_intr_type)
  // and clobber the LOW_LEVEL wake trigger, and a LOW_LEVEL CPU interrupt
  // would storm while the button is held. The buttons are polled by the main
  // loop when awake, so no runtime GPIO interrupt is needed at all.
  const esp_err_t b1 = gpio_wakeup_enable(static_cast<gpio_num_t>(bootPin), GPIO_INTR_LOW_LEVEL);
  const esp_err_t b2 = gpio_wakeup_enable(static_cast<gpio_num_t>(keyPin), GPIO_INTR_LOW_LEVEL);
  const esp_err_t g = esp_sleep_enable_gpio_wakeup();
  Serial.printf("[PM] button wake: boot=0x%x key=0x%x group=0x%x\n", b1, b2, g);
}

void pmTick(uint32_t now) {
  static uint32_t sLastLogMs = 0;
  gStats.sample(xthal_get_ccount(), esp_timer_get_time());
  if (sLastLogMs == 0) {
    sLastLogMs = now;
    return;
  }
  if (now - sLastLogMs < kStatsWindowMs) {
    return;
  }
  const int freq = getCpuFrequencyMhz();
  const int percent = gStats.awakePercent(freq);
  gLastWindowAwakePercent = static_cast<float>(percent);
  // Evidence, ordered by strength:
  //  1. light_sleep_count > 0: SDK-patched counter of completed light sleeps.
  //  2. wakeup_cause != UNDEFINED: at least one wake from an actual sleep
  //     (set only by a completed esp_light_sleep_start, which auto light
  //     sleep uses internally); says nothing about count or duration.
  //  3. duty estimate: awake-cycle vs wall-clock ratio. Frequency changes
  //     and idle behaviour skew it, so it is an estimate, not proof.
  const esp_sleep_source_t cause = esp_sleep_get_wakeup_cause();
  if (esp_sleep_get_light_sleep_count != nullptr) {
    Serial.printf("[PM] light_sleep_count=%u wakeup_cause=%d duty~%d%% (estimate, cpu=%dMHz, ls=%s)\n",
                  static_cast<unsigned>(esp_sleep_get_light_sleep_count()), static_cast<int>(cause), percent, freq,
                  gArmSettled ? (gLightSleepWanted ? "armed" : "off(ab)") : "off");
  } else {
    Serial.printf("[PM] light_sleep_count=n/a(stock SDK) wakeup_cause=%d duty~%d%% (estimate, cpu=%dMHz, ls=%s)\n",
                  static_cast<int>(cause), percent, freq,
                  gArmSettled ? (gLightSleepWanted ? "armed" : "off(ab)") : "off");
  }
  if (gPmOk && gArmSettled && gLightSleepWanted && !gLocksDumped) {
    Serial.println("[PM] lock dump:");
    esp_pm_dump_locks(stdout);
    gLocksDumped = true;
  }
  gStats.resetWindow(xthal_get_ccount(), esp_timer_get_time());
  sLastLogMs = now;
}

#else  // native builds

void pmBegin() {}
bool pmSupported() {
  return false;
}
void pmEnableLightSleep() {}
bool pmLightSleepArmed() {
  return false;
}
void pmEggPerformanceOn() {}
void pmEggPerformanceOff() {}
void pmSetupButtonWakeup(int, int) {}
void pmTick(uint32_t) {}

#endif

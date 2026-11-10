#include "pm_controller.h"

#include <math.h>

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
#include "sdkconfig.h"
#if defined(RLCD_REQUIRE_PM_SDK) && (!defined(CONFIG_PM_ENABLE) || !defined(CONFIG_FREERTOS_USE_TICKLESS_IDLE))
#error "waveshare_rlcd_pm requires the PM-enabled ESP32-S3 SDK (CONFIG_PM_ENABLE + CONFIG_FREERTOS_USE_TICKLESS_IDLE). Run firmware/agent_pet_display/tools/pm-libs/pio-pm.sh instead of bare pio."
#endif
#include "driver/gpio.h"
#include "esp_freertos_hooks.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "xtensa/hal.h"

// Counted inside esp_light_sleep_start by the round-3 SDK patch (see
// tools/pm-libs/). Weak so the same firmware also links against the stock
// SDK, where the counter simply reports as unavailable.
extern "C" uint32_t esp_sleep_get_light_sleep_count(void) __attribute__((weak));

namespace {

#ifdef RLCD_PM_DISABLE_LIGHT_SLEEP
constexpr bool kLightSleepCompileEnabled = false;
#else
constexpr bool kLightSleepCompileEnabled = true;
#endif

constexpr int kIdleFreqMhz = 80;
constexpr int kIdleMinFreqMhz = 20;  // XTAL/2 DFS floor while awake; Wi-Fi PM locks lift it to 80 on demand
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
TaskHandle_t gAppTaskHandle = nullptr;
int gBootWakePin = -1;
int gKeyWakePin = -1;
bool gIdleHookRegistered = false;
volatile bool gWaitingForButtonWake = false;
AwakeStats gStats;

bool onIdleCheckButtonWake() {
  if (gWaitingForButtonWake && gAppTaskHandle != nullptr &&
      ((gBootWakePin >= 0 && gpio_get_level(static_cast<gpio_num_t>(gBootWakePin)) == 0) ||
       (gKeyWakePin >= 0 && gpio_get_level(static_cast<gpio_num_t>(gKeyWakePin)) == 0))) {
    gWaitingForButtonWake = false;
    xTaskNotifyGive(gAppTaskHandle);
    return false;  // yield immediately to woken loopTask instead of re-entering waiti
  }
  return true;  // allow esp_pm_impl_waiti / tickless light sleep
}

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
  gPmOk = applyPmConfig(kIdleFreqMhz, kIdleMinFreqMhz, false) == ESP_OK;
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
  const esp_err_t err = applyPmConfig(kIdleFreqMhz, kIdleMinFreqMhz, kLightSleepCompileEnabled);
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
    const esp_err_t err = applyPmConfig(kIdleFreqMhz, kIdleMinFreqMhz, gLightSleepWanted);
    gArmSettled = (err == ESP_OK);  // wanted state restored (or still off in A/B build)
    Serial.printf("[PM] egg end: 80 MHz, light sleep %s -> %s\n", gLightSleepWanted ? "on" : "off",
                  err == ESP_OK ? "ok" : "FAILED");
  } else {
    const bool ok = setCpuFrequencyMhz(kIdleFreqMhz);
    Serial.printf("[PM] egg end: setCpuFrequencyMhz(80) -> %s\n", ok ? "ok" : "FAILED");
  }
}

void pmSetupButtonWakeup(int bootPin, int keyPin) {
  // IDF 4.4 light-sleep GPIO wake, and nothing else on pin.int_type:
  // attachInterrupt would rewrite pin.int_type and clobber GPIO_INTR_LOW_LEVEL.
  // Instead, onIdleCheckButtonWake() runs on Core 1's FreeRTOS idle loop right
  // after esp_light_sleep_start() returns and wakes loopTask via xTaskNotifyGive.
  gBootWakePin = bootPin;
  gKeyWakePin = keyPin;
  const esp_err_t b1 = gpio_wakeup_enable(static_cast<gpio_num_t>(bootPin), GPIO_INTR_LOW_LEVEL);
  const esp_err_t b2 = gpio_wakeup_enable(static_cast<gpio_num_t>(keyPin), GPIO_INTR_LOW_LEVEL);
  const esp_err_t g = esp_sleep_enable_gpio_wakeup();
  esp_err_t hookErr = ESP_OK;
  if (!gIdleHookRegistered) {
    hookErr = esp_register_freertos_idle_hook_for_cpu(onIdleCheckButtonWake, 1);
    gIdleHookRegistered = (hookErr == ESP_OK);
  }
  Serial.printf("[PM] button wake: boot=0x%x key=0x%x group=0x%x hook=0x%x\n", b1, b2, g, hookErr);
}

void pmRegisterAppTaskWakeup() {
  gAppTaskHandle = xTaskGetCurrentTaskHandle();
}

void pmNotifyAppTask() {
  if (gAppTaskHandle != nullptr) {
    xTaskNotifyGive(gAppTaskHandle);
  }
}

void pmWaitForAppWake(uint32_t sleepMs) {
  if (sleepMs == 0) {
    return;
  }
  gWaitingForButtonWake = true;
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(sleepMs));
  gWaitingForButtonWake = false;
}

uint64_t pmMonotonicMs() {
  const int64_t us = esp_timer_get_time();
  return us > 0 ? static_cast<uint64_t>(us / 1000LL) : 0ULL;
}

// Before a deliberate deep sleep (low-battery protection): auto light sleep
// off so the idle hook cannot start one between the caller's last draws and
// esp_deep_sleep_start. DFS bounds stay; the clock does not matter anymore.
void pmPrepareDeepSleep() {
  gWaitingForButtonWake = false;
  if (gPmOk) {
    applyPmConfig(kIdleFreqMhz, kIdleMinFreqMhz, false);
  }
  gLightSleepWanted = false;
  gArmSettled = false;
}

void pmTick(uint32_t) {
  static uint64_t sLastLogMs = 0;
  static bool sInitialized = false;
  const uint64_t now64 = pmMonotonicMs();
  gStats.sample(xthal_get_ccount(), esp_timer_get_time());
  if (!sInitialized) {
    sInitialized = true;
    sLastLogMs = now64;
    return;
  }
  if (now64 - sLastLogMs < kStatsWindowMs) {
    return;
  }
  const int freq = getCpuFrequencyMhz();
  const int percent = gStats.awakePercent(freq);
  gLastWindowAwakePercent = static_cast<float>(percent);
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
  sLastLogMs = now64;
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
void pmRegisterAppTaskWakeup() {}
void pmNotifyAppTask() {}
void pmWaitForAppWake(uint32_t) {}
uint64_t pmMonotonicMs() {
  return 0ULL;
}
void pmPrepareDeepSleep() {}
void pmTick(uint32_t) {}

#endif

#include "battery_protection.h"

#include "low_battery_guard.h"

#ifdef ARDUINO

#include <Arduino.h>

#include "driver/gpio.h"
#include "esp_sleep.h"

#include "app_config.h"
#include "battery_monitor.h"
#include "models.h"
#include "pm_controller.h"

namespace {

// "LBP1": marks the RTC-data protection slot as written by this build. The
// inverted copy catches garbage after a power cycle (RTC memory is undefined
// then), where the flag must read as absent so the board boots normally.
constexpr uint32_t kProtectMagic = 0x4C425031U;

RTC_DATA_ATTR uint32_t gProtectMagic = 0;
RTC_DATA_ATTR uint32_t gProtectMagicInv = 0;
RTC_DATA_ATTR uint32_t gProtectWakeCount = 0;

bool protectionFlagValid() {
  return gProtectMagic == kProtectMagic && gProtectMagicInv == ~kProtectMagic;
}

void setProtectionFlag() {
  gProtectMagic = kProtectMagic;
  gProtectMagicInv = ~kProtectMagic;
}

void clearProtectionFlag() {
  gProtectMagic = 0;
  gProtectMagicInv = 0;
  gProtectWakeCount = 0;
}

// LCD control pads (schematic: CS=GPIO40, RESET=GPIO41; SCK/MOSI/DC from the
// board wiring). During deep sleep digital pads default to high-impedance
// unless held (see driver/gpio.h): a floating RESET could reset the panel and
// a floating CS could clock in garbage while VCC3V3 still powers the LCD.
// gpio_hold_en alone does NOT hold digital pads through deep sleep on the
// S3; gpio_deep_sleep_hold_en() extends the hold of every gpio_hold_en() pad
// into deep sleep. Whether the panel actually keeps its image also depends
// on the board's 3.3 V rail surviving deep sleep - unverifiable from code.
void holdLcdPads(bool hold) {
  const int pads[] = {kLcdSckPin, kLcdMosiPin, kLcdDcPin, kLcdCsPin, kLcdRstPin};
  for (int pin : pads) {
    if (hold) {
      gpio_hold_en(static_cast<gpio_num_t>(pin));
    } else {
      gpio_hold_dis(static_cast<gpio_num_t>(pin));
    }
  }
  if (hold) {
    gpio_deep_sleep_hold_en();
  } else {
    gpio_deep_sleep_hold_dis();
  }
}

// One protection cycle: arm only the RTC timer, hold the LCD pads, sleep.
// esp_deep_sleep_start() resets the chip on wake (full boot, no resume).
void enterProtectionSleep() {
  ++gProtectWakeCount;
  setProtectionFlag();
  pmPrepareDeepSleep();  // auto light sleep off: nothing may race the entry
  // Drop the light-sleep button wake config; in protection only the RTC
  // timer may wake the board (a button press must not spin up Wi-Fi).
  // IDF 4.4 spells the wildcard ESP_SLEEP_WAKEUP_ALL.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_timer_wakeup(kProtectionSleepUs);
  holdLcdPads(true);
  Serial.printf("[BAT-PROT] deep sleep %lus (wake #%lu will re-measure)\n",
                static_cast<unsigned long>(kBatteryProtectionSleepS),
                static_cast<unsigned long>(gProtectWakeCount));
  Serial.flush();
  esp_deep_sleep_start();  // does not return
}

}  // namespace

bool batteryProtectionWasActive() {
  return protectionFlagValid();
}

uint32_t batteryProtectionWakeCount() {
  return protectionFlagValid() ? gProtectWakeCount : 0;
}

void batteryProtectionEarlyCheck() {
  if (!protectionFlagValid()) {
    return;
  }
  const esp_sleep_source_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("[BAT-PROT] protection boot: wake cause=%d cycles=%lu\n", static_cast<int>(cause),
                static_cast<unsigned long>(gProtectWakeCount));
  // Measure before Wi-Fi/screen/app. The first conversion after a deep-sleep
  // wake can be stale, so read once and discard. Recovery then needs TWO
  // fresh consecutive valid readings at/above 3.75 V (ProtectionWakeConfirm,
  // unit-tested). The 1 s gap before the second reading is paid ONLY when
  // the first reading qualifies: an invalid or sub-threshold first reading
  // goes straight back to deep sleep - staying awake a second to re-confirm
  // a reading that already failed would just drain the cell it is protecting.
  samplePowerState(false);  // warm-up read, discarded
  delay(120);
  ProtectionWakeConfirm confirm;
  const PowerState first = samplePowerState(false);
  // protectionWakeStep() holds this path's decision logic verbatim so the
  // native tests execute the real call path: after the first reading,
  // SleepNow means "did not qualify" (confirmedReadings()==0) and skips the
  // 1 s wait entirely.
  if (protectionWakeStep(confirm, first.sampleOk, first.voltageMv, false) ==
      ProtectionWakeStep::SleepNow) {
    Serial.printf("[BAT-PROT] first reading unconfirmed (%s, %dmV; recovery %dmV): staying protected\n",
                  first.sampleOk ? "low" : "invalid", first.voltageMv,
                  ProtectionWakeConfirm::kThresholdMv);
    enterProtectionSleep();  // does not return
  }
  delay(1000);  // the deliberate ~1 s between confirmation readings
  const PowerState second = samplePowerState(false);
  if (protectionWakeStep(confirm, second.sampleOk, second.voltageMv, true) ==
      ProtectionWakeStep::ResumeNormal) {
    Serial.printf("[BAT-PROT] 2x valid >= %dmV confirmed: resuming normal boot\n",
                  ProtectionWakeConfirm::kThresholdMv);
    // Release the pads the deep-sleep hold kept latched, so the LCD/SPI
    // re-init in the normal boot path can actually drive them again.
    holdLcdPads(false);
    clearProtectionFlag();
    return;
  }
  // Still low (or invalid: cannot prove recovery) -> another cycle. Entering
  // from the wake path skips the notice redraw on purpose: the panel may or
  // may not have kept the entry notice, but re-initialising it every 600 s
  // defeats the point of the protection.
  Serial.printf("[BAT-PROT] second reading unconfirmed (%s, %dmV): staying protected\n",
                second.sampleOk ? "low" : "invalid", second.voltageMv);
  enterProtectionSleep();  // does not return
}

void batteryProtectionEnterDeepSleep() {
  enterProtectionSleep();  // does not return
}

#else  // native builds

bool batteryProtectionWasActive() {
  return false;
}

uint32_t batteryProtectionWakeCount() {
  return 0;
}

void batteryProtectionEarlyCheck() {}

void batteryProtectionEnterDeepSleep() {}

#endif

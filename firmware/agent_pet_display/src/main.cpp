#include <Arduino.h>

#include "app_controller.h"
#include "battery_protection.h"
#include "pm_controller.h"

namespace {

AppController gApp;

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(250);
  // Low-battery protection wake: re-measure on the ADC alone and decide
  // before Wi-Fi, the screen or the app start. May deep-sleep again without
  // returning; falls through (flag cleared) on a recovered battery, and runs
  // as a no-op on every normal boot.
  batteryProtectionEarlyCheck();
  // Power policy owner: esp_pm (80 MHz + auto light sleep after the boot
  // window) when the SDK supports it, otherwise the fixed 80 MHz fallback.
  pmBegin();
  gApp.begin();
}

void loop() {
  gApp.tick();
}

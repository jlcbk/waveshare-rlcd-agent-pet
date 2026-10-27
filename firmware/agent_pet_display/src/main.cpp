#include <Arduino.h>

#include "app_controller.h"
#include "pm_controller.h"

namespace {

AppController gApp;

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(250);
  // Power policy owner: esp_pm (80 MHz + auto light sleep after the boot
  // window) when the SDK supports it, otherwise the fixed 80 MHz fallback.
  pmBegin();
  gApp.begin();
}

void loop() {
  gApp.tick();
}

#include <Arduino.h>

#include "app_controller.h"

namespace {

AppController gApp;

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(250);
  // Idle display duty: 80 MHz is Wi-Fi's minimum and plenty for rendering;
  // egg playback temporarily boosts to 240 MHz (app_controller.cpp).
  if (setCpuFrequencyMhz(80)) {
    Serial.println("[PWR] cpu=80MHz");
  } else {
    Serial.println("[PWR] cpu=80MHz FAILED");
  }
  gApp.begin();
}

void loop() {
  gApp.tick();
}

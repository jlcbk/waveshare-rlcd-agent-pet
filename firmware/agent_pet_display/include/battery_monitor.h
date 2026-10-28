#pragma once

#include <stdint.h>

#include <string>

#include "models.h"

int mapBatteryPercent(int32_t voltageMv);
PowerState derivePowerState(int32_t voltageMv, bool charging);
PowerState samplePowerState(bool charging);

// Display formatting only: "4.09V" from millivolts (truncated to 0.01 V),
// "-- V" for a non-positive reading. Protection decisions must use the raw
// millivolt value and the sampleOk flag, never this rounded string.
std::string formatBatteryVoltage(int voltageMv);

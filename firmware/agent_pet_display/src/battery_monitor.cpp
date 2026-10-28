#include "battery_monitor.h"

#include <stdio.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <esp_adc_cal.h>
#include <driver/adc.h>
#endif

#include "app_config.h"

namespace {

#ifdef ARDUINO
esp_adc_cal_characteristics_t gBatteryChars{};
#endif
bool gBatteryReady = false;

// Nonlinear voltage->percent lookup for the display. Source and method:
//   University of Bayreuth, "Cell-to-Cell Variations of a Panasonic
//   NCR18650B", Zenodo record 8369275 (92 new cells, 25 C, 0.05C discharge,
//   https://zenodo.org/records/8369275). From its pocv_soc.csv (shared SOC
//   axis) and pocv_dc.csv (per-cell discharge pseudo-OCV), take the median
//   voltage across the 92 cells at each SOC point, then normalize SOC
//   between the curve's values at 3.60 V (SOC 42.78%) and 4.10 V (SOC
//   96.65%) and sample every 50 mV. tools/battery_curve/derive_points.py
//   reproduces these points from the two CSVs.
// The 3.60-4.10 V window is a user-chosen usable-voltage display window,
// NOT a claim that 4.10 V is the cell's physical full-charge voltage, and it
// has no effect on charging (nothing here controls the charger). This is a
// representative approximation for one 18650 chemistry, not a universal
// curve. Protection logic (low_battery_guard) uses raw millivolts and never
// this percentage.
struct VoltagePercentPoint {
  int mv;
  int pct;
};
constexpr VoltagePercentPoint kVoltagePercentCurve[] = {
    {3600, 0},  {3650, 15}, {3700, 24}, {3750, 32}, {3800, 42}, {3850, 53},
    {3900, 64}, {3950, 73}, {4000, 82}, {4050, 90}, {4100, 100},
};
constexpr int kVoltagePercentPoints = sizeof(kVoltagePercentCurve) / sizeof(kVoltagePercentCurve[0]);

}  // namespace

void initBatterySampler() {
#ifdef ARDUINO
  if (gBatteryReady) {
    return;
  }

  if (adc1_config_width(ADC_WIDTH_BIT_12) != ESP_OK) {
    return;
  }
  if (adc1_config_channel_atten(ADC1_CHANNEL_3, ADC_ATTEN_DB_12) != ESP_OK) {
    return;
  }

  const esp_adc_cal_value_t calibration =
      esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 0, &gBatteryChars);
  // Only an eFuse-backed characterization makes raw_to_voltage trustworthy.
  // ESP_ADC_CAL_VAL_DEFAULT_VREF (no eFuse data: a generic default-vref
  // guess) and ESP_ADC_CAL_VAL_NOT_SUPPORTED can be off by more than the
  // protection margin around 3.60 V, so uncalibrated millivolts must never
  // count as a valid voltage: the sampler stays not-ready and every sample
  // reports invalid (percent-only display, protection streak reset).
  if (calibration == ESP_ADC_CAL_VAL_DEFAULT_VREF || calibration == ESP_ADC_CAL_VAL_NOT_SUPPORTED) {
    Serial.printf("[BAT] ADC calibration unavailable (0x%x): voltage readings disabled\n",
                  static_cast<int>(calibration));
    return;
  }
  gBatteryReady = true;
#endif
}

bool sampleChargingState() {
#ifdef ARDUINO
  if (kChargeSensePin < 0) {
    return false;
  }

  pinMode(kChargeSensePin, INPUT);
  return digitalRead(kChargeSensePin) == HIGH;
#else
  return false;
#endif
}

int mapBatteryPercent(int32_t voltageMv) {
  const int mv = static_cast<int>(voltageMv);
  // Clamps: below the window floor (and every invalid 0/negative reading)
  // maps to 0, above the window ceiling to 100.
  if (mv <= kVoltagePercentCurve[0].mv) {
    return 0;
  }
  if (mv >= kVoltagePercentCurve[kVoltagePercentPoints - 1].mv) {
    return 100;
  }
  // Integer piecewise-linear interpolation between the 50 mV points; the
  // products stay far below int32 range (<= 4500 * 100).
  for (int i = 1; i < kVoltagePercentPoints; ++i) {
    const VoltagePercentPoint& lo = kVoltagePercentCurve[i - 1];
    const VoltagePercentPoint& hi = kVoltagePercentCurve[i];
    if (mv <= hi.mv) {
      return lo.pct + (mv - lo.mv) * (hi.pct - lo.pct) / (hi.mv - lo.mv);
    }
  }
  return 100;
}

std::string formatBatteryVoltage(int voltageMv) {
  if (voltageMv <= 0) {
    return "-- V";
  }
  // Truncate (not round) to 0.01 V: 4095 mV -> "4.09V". Two decimals is a
  // display choice, not a claim of ±5 mV accuracy.
  char buffer[12];
  snprintf(buffer, sizeof(buffer), "%d.%02dV", voltageMv / 1000, (voltageMv % 1000) / 10);
  return std::string(buffer);
}

PowerState derivePowerState(int32_t voltageMv, bool charging) {
  PowerState power{};
  power.voltageMv = static_cast<int>(voltageMv);
  power.percent = mapBatteryPercent(voltageMv);
  power.charging = charging;
  power.lowBattery = voltageMv <= 3450;
  power.sampleOk = voltageMv > 0;
  return power;
}

PowerState samplePowerState(bool charging) {
#ifdef ARDUINO
  if (!gBatteryReady) {
    initBatterySampler();
  }
  if (!gBatteryReady) {
    return derivePowerState(0, charging);
  }

  int raw = 0;
  int mv = 0;
  raw = adc1_get_raw(ADC1_CHANNEL_3);
  if (raw < 0) {
    return derivePowerState(0, charging);
  }
  mv = static_cast<int>(esp_adc_cal_raw_to_voltage(static_cast<uint32_t>(raw), &gBatteryChars));

  const int32_t batteryMv = static_cast<int32_t>(mv) * 3;
  return derivePowerState(batteryMv, charging || sampleChargingState());
#else
  return derivePowerState(0, charging);
#endif
}

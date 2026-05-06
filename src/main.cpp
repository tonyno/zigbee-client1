// Water-tank distance sensor (Zigbee end device).
// See docs/superpowers/specs/2026-05-06-water-tank-zigbee-design.md.

#ifndef ZIGBEE_MODE_ED
#error "Zigbee end device mode is not selected (set -DZIGBEE_MODE_ED=1)"
#endif

#include "Zigbee.h"

// ---- Identity (Basic cluster strings) ----
constexpr uint8_t  EP_DISTANCE = 10;
constexpr uint8_t  EP_LEVEL    = 11;
constexpr char     MFR[]       = "czechit";
constexpr char     MODEL[]     = "water-tank-sensor";

// ---- Tank geometry (placeholders; tune for real tank) ----
constexpr float    kEmptyDistanceCm = 200.0f;
constexpr float    kFullDistanceCm  =  20.0f;

// ---- Fake distance generator ----
constexpr uint32_t kWavePeriodMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kReportTickMs = 1000;

// ---- Reporting config ----
constexpr uint16_t kRptMin       = 10;     // s — min between reports
constexpr uint16_t kRptMax       = 60;     // s — heartbeat
constexpr float    kRptDeltaCm   = 1.0f;
constexpr float    kRptDeltaPct  = 1.0f;

// ---- Battery sense (DFR1075 built-in 2:1 divider on GPIO0) ----
constexpr uint8_t  kBatteryAdcPin = 0;
constexpr float    kVbatDividerK  = 2.0f;

// Triangle wave between kFullDistanceCm and kEmptyDistanceCm.
// Period: kWavePeriodMs. Used as a stand-in for the real ToF reading.
float fakeDistanceCm(uint32_t nowMs) {
  uint32_t t = nowMs % kWavePeriodMs;
  float phase = float(t) / float(kWavePeriodMs);   // 0..1
  // 0..0.5 ramps from full to empty, 0.5..1 ramps back.
  float tri = (phase < 0.5f) ? (phase * 2.0f) : (2.0f - phase * 2.0f);
  return kFullDistanceCm + tri * (kEmptyDistanceCm - kFullDistanceCm);
}

// Maps distance to fill level. 0% when distance >= empty, 100% when distance <= full.
float computeLevelPct(float distanceCm) {
  float span = kEmptyDistanceCm - kFullDistanceCm;
  if (span <= 0.0f) return 0.0f;       // misconfigured — fail safe
  float pct = (kEmptyDistanceCm - distanceCm) / span * 100.0f;
  if (pct < 0.0f)   pct = 0.0f;
  if (pct > 100.0f) pct = 100.0f;
  return pct;
}

// Reads VBAT through the on-board 2:1 divider on GPIO0. Averages 16 samples.
float readBatteryVoltage() {
  constexpr int N = 16;
  uint32_t mvSum = 0;
  for (int i = 0; i < N; ++i) mvSum += analogReadMilliVolts(kBatteryAdcPin);
  float adcMv = mvSum / float(N);
  return (adcMv * kVbatDividerK) / 1000.0f;
}

// Linear-with-clamp Li-Po SoC mapping. Refine to a multi-point curve later.
uint8_t batteryPercent(float vbat) {
  constexpr float kFullV  = 4.20f;
  constexpr float kEmptyV = 3.30f;
  if (vbat >= kFullV)  return 100;
  if (vbat <= kEmptyV) return 0;
  return uint8_t((vbat - kEmptyV) * 100.0f / (kFullV - kEmptyV));
}

ZigbeeAnalog zbDistance(EP_DISTANCE);
ZigbeeAnalog zbLevel(EP_LEVEL);

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(2000);
  Serial.println("boot");

  analogReadResolution(12);
  pinMode(kBatteryAdcPin, INPUT);

  // EP 10: Basic + Power Config + Analog Input (distance in cm)
  zbDistance.setManufacturerAndModel(MFR, MODEL);
  zbDistance.setPowerSource(ZB_POWER_SOURCE_BATTERY, /*pct=*/100);
  zbDistance.addAnalogInput();
  zbDistance.setAnalogInputDescription("distance_cm");
  zbDistance.setAnalogInputMinMax(0.0f, 500.0f);
  zbDistance.setAnalogInputResolution(0.1f);

  // EP 11: Analog Input (tank level %)
  zbLevel.addAnalogInput();
  zbLevel.setAnalogInputDescription("level_pct");
  zbLevel.setAnalogInputMinMax(0.0f, 100.0f);
  zbLevel.setAnalogInputResolution(1.0f);

  Serial.println("Adding endpoints");
  Zigbee.addEndpoint(&zbDistance);
  Zigbee.addEndpoint(&zbLevel);

  if (!Zigbee.begin()) {
    Serial.println("Zigbee failed to start! Rebooting...");
    delay(1000);
    ESP.restart();
  }

  Serial.print("Connecting to network");
  while (!Zigbee.connected()) {
    Serial.print(".");
    delay(100);
  }
  Serial.println();

  zbDistance.setAnalogInputReporting(kRptMin, kRptMax, kRptDeltaCm);
  zbLevel.setAnalogInputReporting(kRptMin, kRptMax, kRptDeltaPct);
  Serial.println("Reporting configured");
}

void loop() {
  delay(1000);
}

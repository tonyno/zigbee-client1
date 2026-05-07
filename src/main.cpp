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

// ---- Sleep cycle ----
// Test cadence — bump to 3600 (1 h) for production once overnight battery
// drain has been measured and looks acceptable.
constexpr uint32_t kSleepSeconds = 60;
constexpr uint32_t kRadioFlushMs = 500;   // post-report settle before sleep

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

// 3-second BOOT-button hold triggers Zigbee.factoryReset() which clears
// NVS-stored network credentials and reboots; the device must then be
// re-paired in zigbee2mqtt.
void handleFactoryResetButton() {
  if (digitalRead(BOOT_PIN) != LOW) return;

  delay(50);                                  // debounce
  if (digitalRead(BOOT_PIN) != LOW) return;

  uint32_t pressStart = millis();
  while (digitalRead(BOOT_PIN) == LOW) {
    delay(50);
    if (millis() - pressStart > 3000) {
      Serial.println("Factory reset triggered. Rebooting in 1s.");
      delay(1000);
      Zigbee.factoryReset();                  // does not return
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(5000);
  Serial.println("boot");

  analogReadResolution(12);
  pinMode(kBatteryAdcPin, INPUT);
  pinMode(BOOT_PIN, INPUT_PULLUP);
  // Factory reset has to be polled here — once the device enters its
  // wake/sleep cycle, loop() is unreachable. To trigger it: press EN
  // (reset) on the FireBeetle, then hold BOOT for 3 s during the brief
  // window before Zigbee.begin().
  handleFactoryResetButton();

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

  // Sleepy end device: tell the coordinator we won't keep our radio on
  // listening between polls. This is the actual power-saving switch —
  // deep sleep alone isn't enough if the coordinator thinks we're rx-on.
  Zigbee.setRxOnWhenIdle(false);
  Zigbee.setTimeout(30000);   // 10 s join timeout (default 30 s, longer = more battery)

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
  Serial.println("CONNECTED");

  // Measure → report → sleep. Runs once per cold boot or timer wake.
  uint32_t now = millis();
  float d   = fakeDistanceCm(now);
  float pct = computeLevelPct(d);

  zbDistance.setAnalogInput(d);
  zbLevel.setAnalogInput(pct);
  zbDistance.reportAnalogInput();      // explicit push (no min/max/delta binding)
  zbLevel.reportAnalogInput();

  float vbat   = readBatteryVoltage();
  uint8_t bpct = batteryPercent(vbat);
  zbDistance.setBatteryPercentage(bpct);
  zbDistance.setBatteryVoltage(uint8_t(vbat * 10.0f));   // attribute is 100-mV units
  zbDistance.reportBatteryPercentage();

  Serial.println("reported: distance=" + String(d, 1) + "cm level=" + String(pct, 1) + "% vbat=" + String(vbat, 2) + "V (" + String(bpct) + "%)");

  delay(kRadioFlushMs);                // let the radio drain queued frames

  Serial.println("Sleeping for " + String(kSleepSeconds) + " s");
  Serial.flush();                       // make sure the line lands before USB-CDC dies
  esp_sleep_enable_timer_wakeup(uint64_t(kSleepSeconds) * 1000000ULL);
  esp_deep_sleep_start();               // does not return
}

void loop() {
  // Empty — every wake runs setup() to completion, then deep-sleeps.
  // loop() is only reached if esp_deep_sleep_start() ever returns, which
  // it doesn't.
}

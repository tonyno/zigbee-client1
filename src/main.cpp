// Water-tank distance sensor (Zigbee end device).
// See docs/superpowers/specs/2026-05-06-water-tank-zigbee-design.md
// and docs/superpowers/specs/2026-05-07-deep-sleep-design.md.
//
// Architecture follows the upstream Zigbee_Temp_Hum_Sensor_Sleepy example
// (inspiration/Zigbee_Temp_Hum_Sensor_Sleepy.ino):
//   * setup() joins, then spawns a FreeRTOS measure-and-sleep task
//   * the task pushes reports, waits for per-attribute ZCL acks via
//     Zigbee.onGlobalDefaultResponse, retries on FAIL/timeout, then deep
//     sleeps — giving us actual delivery confirmation instead of "report
//     and pray with a 500 ms delay"
//   * loop() polls BOOT, so factory-reset works during any wake window
//     (including the long first-pair interview window)

#ifndef ZIGBEE_MODE_ED
#error "Zigbee end device mode is not selected (set -DZIGBEE_MODE_ED=1)"
#endif

#include "Zigbee.h"
#include <Preferences.h>

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
constexpr uint32_t kSleepSeconds       = 60;
constexpr uint32_t kReportTimeoutMs    = 1000;   // ack wait per attempt
constexpr uint8_t  kReportMaxRetries   = 3;

// ---- First-pair window ----
// Stay awake long enough after the very first join for z2m to complete its
// interview AND its converter's configure() callback (Bind +
// ConfigureReporting commands). Without this, the device sleeps mid-walk
// and ends up half-paired in z2m — interview shows ✓ but Bind/Reporting
// tabs stay empty and Distance/Level read N/A. NVS flag flips after one
// successful window so subsequent boots skip straight to the steady-state
// sleep cycle.
constexpr uint32_t kInterviewWindowMs = 120000;   // 2 min
constexpr uint32_t kInterviewTickMs   = 5000;     // periodic report cadence
constexpr char     kPrefsNamespace[]  = "watertank";
constexpr char     kPrefsPairedKey[]  = "paired";

// ---- Battery sense (DFR1075 built-in 2:1 divider on GPIO0) ----
constexpr uint8_t  kBatteryAdcPin = 0;
constexpr float    kVbatDividerK  = 2.0f;

// ---- BOOT button (factory reset) ----
constexpr uint32_t kBootHoldFactoryResetMs = 3000;   // hold this long for a reset
constexpr uint32_t kColdBootBootWindowMs   = 5000;   // poll BOOT this long at cold boot

// ZigbeeAnalog endpoints. Constructed at module scope so they're visible
// to setup(), the measure-task, and the global ack callback.
ZigbeeAnalog zbDistance(EP_DISTANCE);
ZigbeeAnalog zbLevel(EP_LEVEL);

// Per-cycle ack tracking. The global default-response callback decrements
// g_pendingAcks on each successful ZB_CMD_REPORT_ATTRIBUTE response and
// sets g_resendNeeded on FAIL. Both are reset before each report attempt
// by the measure task. Only the two Analog Input reports are tracked —
// battery is fire-and-forget (the Power Config cluster's report path may
// or may not feed the same callback path; if we tracked it and it didn't
// callback, we'd never get to sleep).
volatile uint8_t g_pendingAcks   = 0;
volatile bool    g_resendNeeded  = false;

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

// Fires for every ZCL command response from the coordinator. We only care
// about ZB_CMD_REPORT_ATTRIBUTE responses for the two Analog Input
// endpoints — those are the ones the measure task waits for.
void onGlobalResponse(zb_cmd_type_t command, esp_zb_zcl_status_t status,
                      uint8_t endpoint, uint16_t cluster) {
  Serial.printf("zb resp: cmd=%d status=%s ep=%u cluster=0x%04x pending=%u\n",
                command, esp_zb_zcl_status_to_name(status), endpoint, cluster,
                g_pendingAcks);
  if (command != ZB_CMD_REPORT_ATTRIBUTE) return;
  if (endpoint != EP_DISTANCE && endpoint != EP_LEVEL) return;
  switch (status) {
    case ESP_ZB_ZCL_STATUS_SUCCESS:
      if (g_pendingAcks > 0) g_pendingAcks--;
      break;
    case ESP_ZB_ZCL_STATUS_FAIL:
      g_resendNeeded = true;
      break;
    default:
      break;
  }
}

// Push current sensor values + battery state to the coordinator. Called
// from the measure task and the interview-window loop. Resets the ack
// tracker to 2 (one per Analog Input) before sending so the caller can
// wait on g_pendingAcks reaching 0.
void reportSensors() {
  uint32_t now = millis();
  float d   = fakeDistanceCm(now);
  float pct = computeLevelPct(d);

  g_pendingAcks  = 2;
  g_resendNeeded = false;

  zbDistance.setAnalogInput(d);
  zbLevel.setAnalogInput(pct);
  zbDistance.reportAnalogInput();
  zbLevel.reportAnalogInput();

  float vbat   = readBatteryVoltage();
  uint8_t bpct = batteryPercent(vbat);
  zbDistance.setBatteryPercentage(bpct);
  zbDistance.setBatteryVoltage(uint8_t(vbat * 10.0f));   // attribute is 100-mV units
  zbDistance.reportBatteryPercentage();                  // fire-and-forget

  Serial.println("reported: distance=" + String(d, 1) + "cm level=" + String(pct, 1) + "% vbat=" + String(vbat, 2) + "V (" + String(bpct) + "%)");
}

// Wait until the two Analog Input reports are ack'd by the coordinator,
// or kReportMaxRetries timeouts have happened. Resends on FAIL/timeout.
// Pattern adapted from the upstream sleepy temp/hum example.
void waitForReportAcks() {
  uint32_t startTime = millis();
  uint8_t  tries     = 0;
  Serial.print("waiting for ack ");
  while (g_pendingAcks != 0 && tries < kReportMaxRetries) {
    if (g_resendNeeded) {
      Serial.println("\nresend on FAIL");
      reportSensors();   // resets pending counter back to 2
      startTime = millis();
    }
    if (millis() - startTime >= kReportTimeoutMs) {
      Serial.println("\nack timeout, resend");
      reportSensors();
      startTime = millis();
      tries++;
    }
    Serial.print(".");
    delay(50);
  }
  Serial.println(g_pendingAcks == 0 ? " ok" : " gave up");
}

// First-pair window: keep the device awake for kInterviewWindowMs after a
// cold-boot join, sending periodic reports so z2m has uninterrupted
// access to walk the Basic cluster and run the converter's configure()
// callback (Bind + ConfigureReporting). Reports during the window do NOT
// wait for acks — z2m may answer slowly or out of order while
// interviewing, and we don't want to block the cycle.
void runInterviewWindow() {
  Serial.println("First pair — staying awake " + String(kInterviewWindowMs / 1000) + " s for z2m interview");
  uint32_t start = millis();
  uint32_t lastTick = 0;
  while (millis() - start < kInterviewWindowMs) {
    if (millis() - lastTick >= kInterviewTickMs || lastTick == 0) {
      reportSensors();
      uint32_t remainS = (kInterviewWindowMs - (millis() - start)) / 1000;
      Serial.println("interview window: " + String(remainS) + " s remaining");
      lastTick = millis();
    }
    delay(50);
  }
  Serial.println("Interview window done");
  Preferences w;
  w.begin(kPrefsNamespace, /*readOnly=*/false);
  w.putBool(kPrefsPairedKey, true);
  w.end();
  Serial.println("Marked as paired in NVS.");
}

// FreeRTOS task: report → wait for ack (with retries) → deep-sleep.
// On the very first cold-boot pair, runs the 2-minute interview window
// before the final report. Created from setup() once Zigbee is connected.
void measureAndSleepTask(void* arg) {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/true);
  bool firstPair = !prefs.isKey(kPrefsPairedKey);
  prefs.end();

  if (firstPair) runInterviewWindow();

  reportSensors();
  waitForReportAcks();

  Serial.println("Sleeping for " + String(kSleepSeconds) + " s");
  Serial.flush();
  esp_sleep_enable_timer_wakeup(uint64_t(kSleepSeconds) * 1000000ULL);
  esp_deep_sleep_start();   // does not return
}

// Polls BOOT for up to windowMs first — gives the user time to start
// pressing the button after the EN release. We CANNOT detect BOOT held
// during the EN press itself: GPIO9 is the chip's strap pin and is
// sampled at reset to choose normal vs UART-download boot mode. Holding
// BOOT across EN puts the chip into download mode, where no application
// code runs (and the serial monitor sees nothing). So the procedure is
// "press EN, then within windowMs start holding BOOT".
//
// Used by setup() at cold boot (windowMs = 5 s) and by loop() during
// every wake (windowMs = 0, no wait — the user is already holding when
// loop() polls).
void handleFactoryResetButton(uint32_t windowMs) {
  uint32_t pollStart = millis();
  while (digitalRead(BOOT_PIN) != LOW) {
    if (millis() - pollStart >= windowMs) return;
    delay(20);
  }

  delay(50);                                  // debounce
  if (digitalRead(BOOT_PIN) != LOW) return;

  uint32_t pressStart = millis();
  while (digitalRead(BOOT_PIN) == LOW) {
    delay(50);
    if (millis() - pressStart > kBootHoldFactoryResetMs) {
      Serial.println("Factory reset triggered. Clearing NVS and rebooting in 1 s.");
      Preferences w;
      w.begin(kPrefsNamespace, /*readOnly=*/false);
      w.clear();
      w.end();
      delay(1000);
      Zigbee.factoryReset();                  // does not return
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);

  pinMode(BOOT_PIN, INPUT_PULLUP);

  // Cold boot only: 5 s window for factory-reset BOOT-hold + USB-CDC
  // settle. Timer wakes from deep sleep skip both — no human pressing
  // buttons mid-cycle, and battery operation shouldn't pay for the wait.
  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  if (wakeCause == ESP_SLEEP_WAKEUP_UNDEFINED) {
    handleFactoryResetButton(/*windowMs=*/kColdBootBootWindowMs);
  }

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

  // Global ack callback BEFORE Zigbee.begin so it catches every report
  // response from the moment the network is up.
  Zigbee.onGlobalDefaultResponse(onGlobalResponse);

  // Custom ED config: keep_alive = 10 s data-poll interval. Matches the
  // upstream sleepy temp/hum example's pattern. We deliberately do NOT
  // call Zigbee.setRxOnWhenIdle(false) — announcing as a true sleepy
  // device makes z2m route Bind/ConfigureReporting through the parent's
  // indirect buffer, which times out, and seems to make the coordinator
  // drop us across deep-sleep gaps. Deep sleep already gives full battery
  // savings (the radio is physically off when the chip is asleep); we
  // just don't advertise sleepy capability over the air.
  esp_zb_cfg_t zigbeeConfig = ZIGBEE_DEFAULT_ED_CONFIG();
  zigbeeConfig.nwk_cfg.zed_cfg.keep_alive = 10000;
  Zigbee.setTimeout(30000);

  if (!Zigbee.begin(&zigbeeConfig, /*erase_nvs=*/false)) {
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

  // Hand off to the measure-and-sleep task. setup() returns; loop() then
  // polls BOOT until the task calls esp_deep_sleep_start.
  xTaskCreate(measureAndSleepTask, "measure_and_sleep", 4096, nullptr, 10, nullptr);
}

void loop() {
  // Factory-reset path is reachable during any wake (including the long
  // first-pair interview window) because this loop runs in parallel with
  // the measure-and-sleep FreeRTOS task. Hold BOOT for 3 s to trigger.
  handleFactoryResetButton(/*windowMs=*/0);
  delay(100);
}

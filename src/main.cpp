// Water-tank distance sensor (Zigbee end device).
// See docs/superpowers/specs/2026-05-06-water-tank-zigbee-design.md
// and docs/superpowers/specs/2026-05-07-deep-sleep-design.md.
//
// Reporting model: we set up *device-side* auto-reporting via
// esp_zb_zcl_update_reporting_info (helper: setAnalogInputReporting).
// That gives the local stack a `dst.short_addr = 0` reporting record,
// so reports go straight to the coordinator without depending on a
// binding-table entry installed by the coordinator. ZigbeeAnalog's
// reportAnalogInput() / ZigbeeEP's reportBatteryPercentage() are the
// *binding-only* path (esp_zb_zcl_report_attr_cmd_req with address
// mode 0 routes via the bind table) — they silently no-op when the
// bind table is empty. We hit that exact failure mode after switching
// to deep sleep: z2m's converter `configure()` was not running, the
// bind table stayed empty, every wake's report was dropped at APS,
// and the only visible symptom was "ack timeout" loops. The auto-
// reporting path bypasses the whole bind dance.
//
// Architecture follows the upstream Zigbee_Temp_Hum_Sensor_Sleepy
// example (inspiration/Zigbee_Temp_Hum_Sensor_Sleepy.ino):
//   * setup() joins, configures auto-reporting, then spawns a FreeRTOS
//     measure-and-sleep task
//   * the task updates the cached attribute values (which fires the
//     stack's auto-report), waits for per-attribute ZCL Default
//     Responses via Zigbee.onGlobalDefaultResponse, then deep-sleeps
//   * loop() polls BOOT, so factory-reset works during any wake window
//     (including the long first-pair interview window)

#ifndef ZIGBEE_MODE_ED
#error "Zigbee end device mode is not selected (set -DZIGBEE_MODE_ED=1)"
#endif

#include "Zigbee.h"
#include "nwk/esp_zigbee_nwk.h"               // esp_zb_get_short_address / pan id / long addr
#include "aps/esp_zigbee_aps.h"               // APS data indication handler (RX traffic log)
#include "zcl/esp_zigbee_zcl_power_config.h"  // ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID
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
constexpr uint32_t kSleepSeconds       = 5;
constexpr uint32_t kReportTimeoutMs    = 8000;   // total ack wait per cycle (no retry — see waitForReportAcks)
constexpr uint32_t kPostJoinSettleMs   = 1000;   // let the parent link stabilize before the first report on warm rejoin

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

// Per-cycle ack tracking. The global default-response callback
// decrements g_pendingAcks on each successful ZB_CMD_REPORT_ATTRIBUTE
// response from the coordinator. Three reports per wake: distance
// (EP10/genAnalogInput), level (EP11/genAnalogInput), battery
// (EP10/genPowerCfg). Reset to 3 before each cycle by reportSensors().
volatile uint8_t g_pendingAcks   = 0;

// Diagnostic counters. Reset on cold boot, accumulate across the
// device's awake life. apsRxCount goes up on every incoming APS frame,
// regardless of profile/cluster — Default Responses to our reports
// look like ZCL frames on profile 0x0104, ZDO Bind requests come in on
// profile 0x0000 cluster 0x0021, etc. apsTxOk / apsTxFail count APS
// data confirms (success vs failure status) for *our* outgoing frames.
// apsTxOk should grow whenever we respond to reads or auto-fire a
// report — if it stays flat while reads come in, we're not answering
// at all.
volatile uint32_t g_apsRxCount   = 0;
volatile uint32_t g_apsTxOk      = 0;
volatile uint32_t g_apsTxFail    = 0;

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

// Fires for every ZCL Default Response *to* one of our outgoing
// commands. Decrement the pending-ack counter for any successful
// ZB_CMD_REPORT_ATTRIBUTE on EP10 (distance + battery) or EP11
// (level). Anything else — including FAIL/INVALID statuses — is
// logged but not treated as a deliverable: if a report fails at
// the coordinator we'd rather sleep than spin retrying.
void onGlobalResponse(zb_cmd_type_t command, esp_zb_zcl_status_t status,
                      uint8_t endpoint, uint16_t cluster) {
  Serial.printf("zb resp: cmd=%d status=%s ep=%u cluster=0x%04x pending=%u\n",
                command, esp_zb_zcl_status_to_name(status), endpoint, cluster,
                g_pendingAcks);
  if (command != ZB_CMD_REPORT_ATTRIBUTE) return;
  if (endpoint != EP_DISTANCE && endpoint != EP_LEVEL) return;
  if (status == ESP_ZB_ZCL_STATUS_SUCCESS && g_pendingAcks > 0) {
    g_pendingAcks = g_pendingAcks - 1;   // -- on volatile is deprecated
  }
}

// APS data indication = every incoming application-layer frame.
// Useful for proving (a) we have a working RX path with the parent
// and (b) the coordinator is actually sending us things — Bind
// requests during pairing (profile=0x0000 cluster=0x0021), ZCL
// Default Responses to our reports (profile=0x0104), reads, etc.
// Returning false hands the frame back to the stack for normal
// processing (returning true would *suppress* it — don't do that).
bool onApsdeDataInd(esp_zb_apsde_data_ind_t ind) {
  g_apsRxCount = g_apsRxCount + 1;   // ++ on volatile is deprecated
  Serial.printf("aps rx: src=0x%04x ep=%u→%u prof=0x%04x cluster=0x%04x lqi=%d len=%lu\n",
                ind.src_short_addr, ind.src_endpoint, ind.dst_endpoint,
                ind.profile_id, ind.cluster_id, ind.lqi,
                (unsigned long)ind.asdu_length);
  return false;
}

// APS data confirm = L2 result for each of our outgoing frames. We
// log every confirm (success and fail) so the per-cycle line tells us
// not just whether anything failed, but whether the device sent
// anything at all. No Tx Ok lines while reads are coming in = the
// device is silently ignoring polls. A burst of Tx Ok after a
// `set:` line = auto-reporting is firing. Non-zero status = parent
// never acked at L2 (separate from "delivered but no app ack").
void onApsdeDataConfirm(esp_zb_apsde_data_confirm_t confirm) {
  if (confirm.status == 0) {
    g_apsTxOk = g_apsTxOk + 1;
    Serial.printf("aps tx ok:   dst_mode=%u dst_short=0x%04x ep=%u→%u\n",
                  confirm.dst_addr_mode, confirm.dst_addr.addr_short,
                  confirm.src_endpoint, confirm.dst_endpoint);
  } else {
    g_apsTxFail = g_apsTxFail + 1;
    Serial.printf("aps tx FAIL: status=0x%02x dst_mode=%u dst_short=0x%04x ep=%u→%u\n",
                  confirm.status, confirm.dst_addr_mode,
                  confirm.dst_addr.addr_short,
                  confirm.src_endpoint, confirm.dst_endpoint);
  }
}

// Logs short address, PAN id, IEEE address. Called right after
// Zigbee.connected() so we can confirm we actually joined and where
// we sit in the network. short_addr=0xFFFF is "not joined yet".
void logNetworkInfo(const char *tag) {
  uint16_t shortAddr = esp_zb_get_short_address();
  uint16_t panId     = esp_zb_get_pan_id();
  esp_zb_ieee_addr_t ieee;
  esp_zb_get_long_address(ieee);
  Serial.printf("[%s] short=0x%04x pan=0x%04x ieee=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                tag, shortAddr, panId,
                ieee[7], ieee[6], ieee[5], ieee[4],
                ieee[3], ieee[2], ieee[1], ieee[0]);
}

// Set up firmware-side auto-reporting for the three reportable
// attributes (distance, level, battery%). Distance + level go via
// ZigbeeAnalog::setAnalogInputReporting which memsets dst to zero —
// dst.short_addr=0 is the coordinator, so reports flow there even
// without a bind-table entry. Battery has no equivalent helper, so
// we set up esp_zb_zcl_reporting_info_t directly with the same
// dst-zeroing pattern.
//
// Intervals: min=0 / max=60 / delta=1.0 — matches the OLD working
// always-on firmware. Some esp-zigbee-sdk builds appear to interpret
// delta=0 as "never fire on value change, only on max_interval"
// rather than "any change reports", which keeps reports from firing
// on per-wake attribute updates. delta=1.0 is small enough that any
// real distance/level change crosses it and a fresh report goes out.
//
// IMPORTANT: must be called *after* Zigbee.connected(). Calling it
// before begin() doesn't reach the configured ZCL stack; calling it
// before connect() may run before the network is up enough to accept
// the registration.
void configureAutoReporting() {
  if (!zbDistance.setAnalogInputReporting(0, 60, 1.0f)) {
    Serial.println("WARN: distance auto-report config failed");
  } else {
    Serial.println("ok: distance auto-report (ep10 0x000C, max 60s, delta 1.0)");
  }
  if (!zbLevel.setAnalogInputReporting(0, 60, 1.0f)) {
    Serial.println("WARN: level auto-report config failed");
  } else {
    Serial.println("ok: level auto-report (ep11 0x000C, max 60s, delta 1.0)");
  }

  esp_zb_zcl_reporting_info_t bat = {};
  bat.direction    = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
  bat.ep           = EP_DISTANCE;
  bat.cluster_id   = ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG;
  bat.cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE;
  bat.attr_id      = ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID;
  bat.u.send_info.min_interval     = 0;
  bat.u.send_info.max_interval     = 60;
  bat.u.send_info.def_min_interval = 0;
  bat.u.send_info.def_max_interval = 60;
  bat.u.send_info.delta.u8         = 1;   // 1% — same delta=0 footgun as the analog inputs
  bat.dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
  bat.manuf_code     = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
  esp_zb_lock_acquire(portMAX_DELAY);
  esp_err_t ret = esp_zb_zcl_update_reporting_info(&bat);
  esp_zb_lock_release();
  if (ret != ESP_OK) {
    Serial.printf("WARN: battery auto-report config failed: 0x%x %s\n",
                  ret, esp_err_to_name(ret));
  } else {
    Serial.println("ok: battery auto-report (ep10 0x0001, max 60s)");
  }
}

// Push current sensor values + battery state to the coordinator. We
// only update the cached attribute values — the stack's auto-report
// path (set up in configureAutoReporting) fires the actual ZCL
// Report Attribute frames. Three reports per cycle (distance, level,
// battery%) are tracked via the global default-response callback.
// We deliberately don't call ZigbeeAnalog::reportAnalogInput() or
// ZigbeeEP::reportBatteryPercentage() — those are the binding-only
// path (esp_zb_zcl_report_attr_cmd_req with addr-mode 0) and silently
// no-op when the bind table is empty. See header comment for the full
// post-mortem on that footgun.
void reportSensors() {
  uint32_t now = millis();
  float d   = fakeDistanceCm(now);
  float pct = computeLevelPct(d);

  g_pendingAcks  = 3;   // distance, level, battery

  zbDistance.setAnalogInput(d);                          // ep10 0x000C → auto-report
  zbLevel.setAnalogInput(pct);                           // ep11 0x000C → auto-report

  float vbat   = readBatteryVoltage();
  uint8_t bpct = batteryPercent(vbat);
  zbDistance.setBatteryPercentage(bpct);                 // ep10 0x0001 → auto-report
  zbDistance.setBatteryVoltage(uint8_t(vbat * 10.0f));   // attribute is 100-mV units, no auto-report

  Serial.printf("set: distance=%.1fcm level=%.1f%% vbat=%.2fV (%u%%) [pending=%u apsRx=%lu]\n",
                d, pct, vbat, bpct, g_pendingAcks,
                (unsigned long)g_apsRxCount);

  // Belt-and-suspenders: also fire a manual one-shot report. This
  // uses the binding table — once z2m's configure() has installed
  // binds (Bind tab populated in z2m UI), these reach the
  // coordinator. The auto-reporting path (set up in
  // configureAutoReporting) is a separate way to get the same data
  // out via dst.short_addr=0; we send both so whichever path z2m's
  // wired up that pairing actually delivers. Each successful report
  // produces one zb resp: cmd=10 line below.
  Serial.println("manual report → distance ep10 cluster=0x000C");
  zbDistance.reportAnalogInput();
  Serial.println("manual report → level ep11 cluster=0x000C");
  zbLevel.reportAnalogInput();
  Serial.println("manual report → battery ep10 cluster=0x0001");
  zbDistance.reportBatteryPercentage();
}

// Wait until all three reports (distance/level/battery) have been
// ack'd by the coordinator, or the timeout fires. We do NOT retry on
// timeout: with auto-reporting the only way to "re-send" is to mutate
// the attribute again, and the wake cycle naturally retries on the
// next boot anyway. The summary line at the end says how many
// out of 3 reports actually landed — anything < 3 means the radio
// path was flaky for this wake and we'll retry on the next.
void waitForReportAcks() {
  const uint8_t kExpectedAcks = 3;
  uint32_t startTime = millis();
  Serial.print("waiting for acks ");
  while (g_pendingAcks > 0 && millis() - startTime < kReportTimeoutMs) {
    Serial.print(".");
    delay(50);
  }
  uint8_t got = (g_pendingAcks <= kExpectedAcks) ? (kExpectedAcks - g_pendingAcks) : 0;
  Serial.printf("\nacks: %u/%u after %lu ms (%s)\n",
                got, kExpectedAcks, (unsigned long)(millis() - startTime),
                g_pendingAcks == 0 ? "ok" : "timeout");
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

// FreeRTOS task: report → wait for ack → deep-sleep. On the very
// first cold-boot pair, runs the 2-minute interview window before the
// final report. Created from setup() once Zigbee is connected.
//
// Steady-state wakes: a small post-rejoin settling delay before the
// first report. Without it the parent's APS path back to the
// coordinator isn't always ready in the few ms after Zigbee.connected()
// returns and the first one or two reports get lost. 1 s is enough on
// our network; bump if your parent is further away.
void measureAndSleepTask(void* arg) {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/true);
  bool firstPair = !prefs.isKey(kPrefsPairedKey);
  prefs.end();

  if (firstPair) {
    runInterviewWindow();
  } else {
    Serial.printf("post-rejoin settle: delay(%lu ms)\n",
                  (unsigned long)kPostJoinSettleMs);
    delay(kPostJoinSettleMs);
  }

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
// Returns true if BOOT was held for kBootHoldFactoryResetMs. The caller
// decides what to do — cold boot plumbs this into Zigbee.begin(erase_nvs)
// (Zigbee.factoryReset() crashes if the stack hasn't been initialized,
// since it relies on partition handles opened by begin()), while runtime
// callers from loop() can call Zigbee.factoryReset() directly.
//
// Used by setup() at cold boot (windowMs = 5 s — also serves as USB-CDC
// settle delay) and by loop() during every wake (windowMs = 0, no wait).
bool pollFactoryResetHold(uint32_t windowMs) {
  uint32_t pollStart = millis();
  while (digitalRead(BOOT_PIN) != LOW) {
    if (millis() - pollStart >= windowMs) return false;
    delay(20);
  }

  delay(50);                                  // debounce
  if (digitalRead(BOOT_PIN) != LOW) return false;

  uint32_t pressStart = millis();
  while (digitalRead(BOOT_PIN) == LOW) {
    delay(50);
    if (millis() - pressStart > kBootHoldFactoryResetMs) {
      // Drain the press so we don't double-trigger if the user is slow
      // to release.
      while (digitalRead(BOOT_PIN) == LOW) delay(50);
      return true;
    }
  }
  return false;
}

// Wipes our Preferences namespace (paired flag etc). Safe to call any
// time — doesn't depend on Zigbee being initialized.
void clearLocalPrefs() {
  Preferences w;
  w.begin(kPrefsNamespace, /*readOnly=*/false);
  w.clear();
  w.end();
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);

  pinMode(BOOT_PIN, INPUT_PULLUP);

  // Cold boot only: 5 s window for factory-reset BOOT-hold + USB-CDC
  // settle. Timer wakes from deep sleep skip both — no human pressing
  // buttons mid-cycle, and battery operation shouldn't pay for the wait.
  // If the user holds BOOT for 3 s here, we set eraseNvsOnBegin so
  // Zigbee.begin() wipes its NVS partitions during normal init. Calling
  // Zigbee.factoryReset() here would crash — partition handles aren't
  // open until begin() runs.
  bool eraseNvsOnBegin = false;
  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  if (wakeCause == ESP_SLEEP_WAKEUP_UNDEFINED) {
    if (pollFactoryResetHold(/*windowMs=*/kColdBootBootWindowMs)) {
      Serial.println("Factory reset (cold boot) — clearing prefs; Zigbee NVS will be erased in begin().");
      clearLocalPrefs();
      eraseNvsOnBegin = true;
    }
  }

  Serial.printf("boot (wakeCause=%d %s)\n", (int)wakeCause,
                wakeCause == ESP_SLEEP_WAKEUP_TIMER     ? "TIMER" :
                wakeCause == ESP_SLEEP_WAKEUP_UNDEFINED ? "COLD"  :
                                                          "OTHER");

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
  // response from the moment the network is up. The APS RX/TX hooks
  // are registered AFTER begin() further below, because Zigbee.begin()
  // installs its own indication handler at line 196 of ZigbeeCore.cpp
  // (used for bind/unbind tracking). Registering ours before would
  // get silently overwritten.
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

  if (!Zigbee.begin(&zigbeeConfig, /*erase_nvs=*/eraseNvsOnBegin)) {
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
  logNetworkInfo("post-connect");

  // APS RX/TX hooks must be registered AFTER Zigbee.begin() — the
  // library's begin() installs its own indication handler and would
  // overwrite ours. Register here, AFTER the stack has booted and the
  // library's setup is done. We lose the library's
  // searchBindings() helper for cluster 0x21/0x22 events but we don't
  // depend on it (we don't call Zigbee.bound()/Zigbee.bound_devices).
  esp_zb_aps_data_indication_handler_register(onApsdeDataInd);
  esp_zb_aps_data_confirm_handler_register(onApsdeDataConfirm);

  // Set up firmware-side auto-reporting BEFORE the measure task runs
  // its first reportSensors(), so the very first cold-boot report
  // already has a configured destination.
  configureAutoReporting();

  // Hand off to the measure-and-sleep task. setup() returns; loop() then
  // polls BOOT until the task calls esp_deep_sleep_start.
  xTaskCreate(measureAndSleepTask, "measure_and_sleep", 4096, nullptr, 10, nullptr);
}

void loop() {
  // Factory-reset path is reachable during any wake (including the long
  // first-pair interview window) because this loop runs in parallel with
  // the measure-and-sleep FreeRTOS task. Hold BOOT for 3 s to trigger.
  // Safe to call Zigbee.factoryReset() here — Zigbee.begin() has run by
  // the time setup() returns and loop() starts.
  if (pollFactoryResetHold(/*windowMs=*/0)) {
    Serial.println("Factory reset (runtime). Clearing prefs and rebooting in 1 s.");
    clearLocalPrefs();
    delay(1000);
    Zigbee.factoryReset();   // does not return
  } 
  delay(100);
}

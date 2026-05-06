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

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(2000);
  Serial.println("boot");
}

void loop() {
  delay(1000);
}

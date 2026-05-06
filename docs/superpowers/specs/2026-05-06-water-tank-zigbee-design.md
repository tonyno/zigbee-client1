---
title: Water-tank distance sensor — Zigbee integration
date: 2026-05-06
status: approved
---

# Water-tank distance sensor — Zigbee integration

## Goal

Replace the Espressif on/off light-bulb boilerplate in `src/main.cpp` with a
Zigbee end-device firmware that identifies as a water-tank distance sensor
and exposes three values to a Zigbee coordinator (Home Assistant via
zigbee2mqtt):

1. **Distance** to the water surface, in centimetres.
2. **Tank fill level** as a percentage (0–100 %).
3. **Battery percentage**, read from the on-board Li-Po sense divider.

This iteration is intentionally scoped to the **Zigbee model and its
discovery in zigbee2mqtt**. The actual VL53L1X distance measurement is
out of scope; a triangle-wave fake distance generator stands in for the
real sensor so the data path can be exercised end-to-end. Battery
measurement, however, *is* in scope, because the FireBeetle 2 ESP32-C6
exposes the cell voltage natively on GPIO0.

Deep sleep is also out of scope this iteration — the firmware runs always-on
to keep dev iteration tight. The eventual sleepy-ZED model is preserved
in CLAUDE.md as a follow-up task.

## Non-goals

- Real distance sensing (VL53L1X wiring, I²C bring-up).
- Deep-sleep duty cycling and the multi-year battery-life target.
- A zigbee2mqtt external converter for pretty `distance` / `level` labels
  (the device will pair as a generic Zigbee device with auto-recognised
  Analog Input + Power Configuration clusters; relabelling is a follow-up).
- Any HA-side automations or template sensors.
- Coulomb-counting or any non-voltage-based battery state-of-charge.

## Architecture

```
┌─────────────────── ESP32-C6 (Arduino + Zigbee 3.3.8) ───────────────────┐
│                                                                         │
│  Endpoint 10 — ZigbeeAnalog                                             │
│     ├─ Basic cluster:   Manufacturer="czechit", Model="water-tank-sensor"│
│     ├─ Power Config:    Battery source, battery % attribute (reportable)│
│     └─ Analog Input:    distance to water surface (cm)                  │
│                                                                         │
│  Endpoint 11 — ZigbeeAnalog                                             │
│     └─ Analog Input:    tank fill level (0–100 %)                       │
│                                                                         │
│  Lifecycle: setup() joins Zigbee + configures reporting; loop() drives  │
│  a triangle-wave fake distance generator and a real battery reader      │
│  that update the endpoint values; the library decides when reports go  │
│  out based on the configured min/max/delta.                             │
└─────────────────────────────────────────────────────────────────────────┘
                                │  IEEE 802.15.4
                                ▼
                       Zigbee coordinator (HA)
                                │
                                ▼
                  zigbee2mqtt — auto-detected as a generic
                  device with two Analog Inputs + battery.
                  No external converter required for this iteration.
```

### Why two endpoints

The Arduino `ZigbeeAnalog` class allows only one Analog Input cluster per
endpoint instance. The two reportable values (`distance`, `level`) therefore
need two endpoint instances. Battery is on the base `ZigbeeEP` and lives on
endpoint 10 alongside the Basic cluster.

### Why no external converter (for now)

zigbee2mqtt has built-in handlers for unknown devices that auto-expose any
Analog Input cluster and the standard Power Configuration battery
attribute. The values reach Home Assistant correctly; the labels are just
generic (`analog_input` instead of `distance`). Adding a converter is a
~30-line follow-up and is tracked in CLAUDE.md.

## Identity & Zigbee parameters

| Field                         | Value                              |
| ----------------------------- | ---------------------------------- |
| Manufacturer name (Basic)     | `czechit`                          |
| Model identifier (Basic)      | `water-tank-sensor`                |
| Power source (Basic)          | `Battery`                          |
| Endpoint IDs                  | 10 (distance), 11 (level)          |
| Distance Analog Input range   | min 0 cm, max 500 cm, res 0.1 cm   |
| Distance Analog Input desc.   | `distance_cm`                      |
| Level Analog Input range      | min 0 %, max 100 %, res 1 %        |
| Level Analog Input desc.      | `level_pct`                        |
| Reporting (distance)          | min 10 s, max 60 s, delta 1.0 cm   |
| Reporting (level)             | min 10 s, max 60 s, delta 1.0 %    |
| Battery % reporting           | explicit `reportBatteryPercentage()` per loop tick |

These strings (manufacturer, model) are durable identifiers — once devices
are paired in z2m they cannot be changed without re-pairing. They are
chosen now to support a future external converter that matches on these
exact values.

## Tank geometry

Compile-time placeholder constants in `src/main.cpp`:

```cpp
constexpr float kEmptyDistanceCm = 200.0f;  // sensor reads this when empty
constexpr float kFullDistanceCm  =  20.0f;  // sensor reads this when full
```

`level_pct = clamp((kEmptyDistanceCm - distance) / (kEmptyDistanceCm - kFullDistanceCm), 0, 1) * 100`.

These are placeholders, not final tank dimensions. They are intentionally
hardcoded for this iteration; promoting them to NVS-stored or
remotely-writable Zigbee attributes is deliberately deferred.

## Fake distance generator

A triangle wave between `kFullDistanceCm` and `kEmptyDistanceCm` with a
period of 5 minutes. Picked because:

- Exercises the full reporting path (a constant value would suppress
  delta-based reports almost entirely after the first send).
- Produces a visually-obvious moving graph in HA dashboards, making it
  trivial to confirm the integration is alive.
- Triangle (not sine) avoids any float-precision pitfalls with `sin()`.

## Battery measurement

DFRobot DFR1075 wiring (per the FireBeetle 2 C6 wiki and the on-board
charger schematic):

- Battery voltage is exposed via a built-in **2:1 voltage divider** to
  **GPIO0** (`ADC1_CH0`).
- ADC reads `VBAT/2`; effective `VBAT = ADC_mV × 2`.
- ESP32-C6 ADC: 12-bit, 11 dB attenuation → ~3.3 V full-scale on the ADC
  pin → ~6.6 V on the cell side, comfortably covering the 3.0–4.2 V
  Li-Po range.

Implementation:

```cpp
constexpr uint8_t  kBatteryAdcPin = 0;
constexpr float    kVbatDividerK  = 2.0f;   // calibrate against multimeter later

float readBatteryVoltage() {
  constexpr int N = 16;
  uint32_t mvSum = 0;
  for (int i = 0; i < N; ++i) mvSum += analogReadMilliVolts(kBatteryAdcPin);
  return (mvSum / float(N)) * kVbatDividerK / 1000.0f;
}

uint8_t batteryPercent(float vbat) {
  constexpr float kFullV  = 4.20f;
  constexpr float kEmptyV = 3.30f;
  if (vbat >= kFullV)  return 100;
  if (vbat <= kEmptyV) return 0;
  return uint8_t((vbat - kEmptyV) * 100.0f / (kFullV - kEmptyV));
}
```

Mapping is linear-with-clamp; refining to a multi-point Li-Ion discharge
curve is a follow-up.

GPIO0 does not collide with `BOOT_PIN` — on ESP32-C6 the BOOT button is
on GPIO9.

When the board is USB-only (no cell connected), GPIO0 reads near zero and
`batteryPercent()` returns 0 %, which is semantically correct.

## File layout

```
src/main.cpp        ← rewritten: water-tank sensor (replaces light bulb)
partitions.csv      ← unchanged
platformio.ini      ← unchanged
CLAUDE.md           ← updated: device profile, status, follow-up TODOs
docs/superpowers/specs/
└─ 2026-05-06-water-tank-zigbee-design.md   ← this document
```

## `main.cpp` skeleton

```cpp
#include "Zigbee.h"

constexpr uint8_t  EP_DISTANCE = 10;
constexpr uint8_t  EP_LEVEL    = 11;
constexpr char     MFR[]       = "czechit";
constexpr char     MODEL[]     = "water-tank-sensor";

constexpr float    kEmptyDistanceCm = 200.0f;
constexpr float    kFullDistanceCm  =  20.0f;

constexpr uint32_t kWavePeriodMs = 5 * 60 * 1000;
constexpr uint32_t kReportTickMs = 1000;
constexpr uint16_t kRptMin       = 10;
constexpr uint16_t kRptMax       = 60;
constexpr float    kRptDeltaCm   = 1.0f;
constexpr float    kRptDeltaPct  = 1.0f;

constexpr uint8_t  kBatteryAdcPin = 0;
constexpr float    kVbatDividerK  = 2.0f;

ZigbeeAnalog zbDistance(EP_DISTANCE);
ZigbeeAnalog zbLevel(EP_LEVEL);

float fakeDistanceCm(uint32_t nowMs);   // triangle wave
float computeLevelPct(float distanceCm);
float readBatteryVoltage();
uint8_t batteryPercent(float vbat);
void   handleFactoryResetButton();
void   blinkLedOnReport();

void setup() {
  Serial.begin(115200); delay(2000);
  pinMode(BOOT_PIN, INPUT_PULLUP);
  pinMode(RGB_BUILTIN, OUTPUT);
  analogReadResolution(12);
  pinMode(kBatteryAdcPin, INPUT);

  // EP 10: Basic + Power Config + Analog Input (distance)
  zbDistance.setManufacturerAndModel(MFR, MODEL);
  zbDistance.setPowerSource(ZB_POWER_SOURCE_BATTERY, /*pct=*/100);
  zbDistance.addAnalogInput();
  zbDistance.setAnalogInputDescription("distance_cm");
  zbDistance.setAnalogInputMinMax(0.0f, 500.0f);
  zbDistance.setAnalogInputResolution(0.1f);

  // EP 11: Analog Input (level %)
  zbLevel.addAnalogInput();
  zbLevel.setAnalogInputDescription("level_pct");
  zbLevel.setAnalogInputMinMax(0.0f, 100.0f);
  zbLevel.setAnalogInputResolution(1.0f);

  Zigbee.addEndpoint(&zbDistance);
  Zigbee.addEndpoint(&zbLevel);

  if (!Zigbee.begin()) { ESP.restart(); }
  while (!Zigbee.connected()) { /* solid red LED */ delay(100); }

  zbDistance.setAnalogInputReporting(kRptMin, kRptMax, kRptDeltaCm);
  zbLevel.setAnalogInputReporting(kRptMin, kRptMax, kRptDeltaPct);
}

void loop() {
  handleFactoryResetButton();    // 3-sec hold → Zigbee.factoryReset()

  uint32_t now = millis();
  float d   = fakeDistanceCm(now);
  float pct = computeLevelPct(d);

  zbDistance.setAnalogInput(d);
  zbLevel.setAnalogInput(pct);

  float vbat = readBatteryVoltage();
  uint8_t bpct = batteryPercent(vbat);
  zbDistance.setBatteryPercentage(bpct);
  zbDistance.setBatteryVoltage(uint8_t(vbat * 10.0f));   // attribute is in 100-mV units
  zbDistance.reportBatteryPercentage();

  blinkLedOnReport();
  delay(kReportTickMs);
}
```

The library suppresses redundant Analog Input reports based on the
`min/max/delta` configuration; the `setAnalogInput()` calls every tick are
cheap and let the stack make the decision.

## Validation plan

In order of strictness:

1. **Build green.**
   ```
   pio run -t clean && pio run
   ```
2. **Boots and joins.**
   - `boot` printed; "Adding ZigbeeAnalog endpoint" for EP 10 and EP 11.
   - "Zigbee started" → "Connecting to network" → "Connected".
   - LED: solid red while joining → off when joined → blue blink each tick.
3. **Discovered in zigbee2mqtt.**
   - Visible in z2m "Devices".
   - `manufacturerName = "czechit"`, `modelID = "water-tank-sensor"`.
   - "Supported: No (interview successful)" — expected for generic auto-handling.
   - Endpoints 10 and 11 each show an Analog Input cluster.
   - Endpoint 10 also shows Power Configuration with battery percentage.
4. **Values flow.**
   - `analog_input` on EP 10 oscillates between ~20 cm and ~200 cm over 5 min.
   - `analog_input` on EP 11 oscillates 0–100.
   - `battery` reflects the actual cell state (verify with multimeter).
   - `linkquality` is non-zero.
5. **Reports arrive without polling.** With z2m at debug log level,
   confirm attribute-report frames originate from the device, not z2m
   read-attribute polls.
6. **Factory reset works.** Hold BOOT 3 s → device leaves the network →
   re-pairs cleanly on next boot.

## Future work (also tracked in CLAUDE.md)

1. Wire VL53L1X ToF sensor on I²C and replace `fakeDistanceCm()` with real
   measurements. Confirm DFR1075 default I²C pin assignment.
2. Refine battery SoC mapping. Replace the linear `batteryPercent()` with
   a multi-point Li-Po discharge curve once the linear version's drift is
   measured. Recalibrate `kVbatDividerK` against a multimeter reading.
3. Deep sleep cycle. Restructure `loop()` → `setup()`-only flow with a
   single report and `esp_deep_sleep_start(3600 × 1e6)`. Drop the LED
   indicator (battery cost).
4. Tune reporting cadence for battery life. Once on real hardware,
   measure wake duration and adjust the deep-sleep interval to hit the
   multi-year battery target.
5. Optional: zigbee2mqtt external converter for pretty `distance` /
   `level` exposes instead of generic `analog_input`. ~30 lines of JS in
   z2m's `data/external_converters/`.
6. Optional: switch to a single-`factory` partition layout once OTA
   flexibility is no longer wanted. Wipes NVS — re-pair required.

# Water-tank Zigbee firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Espressif on/off light-bulb boilerplate in `src/main.cpp` with a water-tank distance-sensor firmware that pairs with zigbee2mqtt as `czechit / water-tank-sensor`, exposing distance (cm), tank fill level (%), and real battery percentage.

**Architecture:** Single Arduino sketch. Two `ZigbeeAnalog` endpoints (10 = distance, 11 = level), Basic + Power Configuration clusters on EP 10, attribute reporting via the library's `setAnalogInputReporting(min, max, delta)` mechanism. Distance is faked with a 5-minute triangle wave; battery is read for real on GPIO0 through the FireBeetle 2 C6 built-in 2:1 divider. Always-on loop — deep sleep is deferred.

**Tech Stack:** PlatformIO + pioarduino + Arduino-ESP32 3.3.8 + Espressif `Zigbee.h` library + ESP-IDF 5.5.4 underneath. No new dependencies introduced.

**Spec:** `docs/superpowers/specs/2026-05-06-water-tank-zigbee-design.md`

**Verification model (read first):**
This is bare-metal firmware that talks to a radio. There's no unit-test
seam for `Zigbee.h` calls without heavy mocking, so each code-touching
task ends with `pio run` (must build cleanly) + a `git commit`. End-to-end
behaviour is verified once on real hardware in the final task by walking
the spec's validation plan (boots → joins → appears in z2m → values flow
→ factory reset works).

If `pio run` ever produces warnings about unused functions or missing
declarations, fix them before committing — don't wait for the final task.

---

## Task 1: Strip the light-bulb code, leave a minimal buildable skeleton

**Files:**
- Modify: `src/main.cpp` (full rewrite)

- [ ] **Step 1: Replace `src/main.cpp` with a minimal Zigbee end-device skeleton.**

The skeleton initialises Serial, sets up nothing else, and idles. It must
still include `Zigbee.h` and the `ZIGBEE_MODE_ED` guard so the build
flags stay meaningful.

```cpp
// Water-tank distance sensor (Zigbee end device).
// See docs/superpowers/specs/2026-05-06-water-tank-zigbee-design.md.

#ifndef ZIGBEE_MODE_ED
#error "Zigbee end device mode is not selected (set -DZIGBEE_MODE_ED=1)"
#endif

#include "Zigbee.h"

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(2000);
  Serial.println("boot");
}

void loop() {
  delay(1000);
}
```

- [ ] **Step 2: Build and verify it compiles cleanly.**

Run: `pio run`
Expected: `SUCCESS`. RAM/Flash percentages will drop versus the previous
build because `ZigbeeLight` is gone.

- [ ] **Step 3: Commit.**

```bash
git add src/main.cpp
git commit -m "refactor: strip light-bulb code, leave minimal Zigbee skeleton"
```

---

## Task 2: Define identity, geometry, ADC, and reporting constants

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Add the constants block immediately after the `#include "Zigbee.h"` line.**

These are all `constexpr` so they live in flash and don't cost RAM.

```cpp
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
```

- [ ] **Step 2: Build and verify.**

Run: `pio run`
Expected: `SUCCESS`. Constants are unused so the compiler may not error
even if a name is wrong, but a typo in `EP_DISTANCE` etc. will surface
in later tasks. That's fine — we'll catch it then.

- [ ] **Step 3: Commit.**

```bash
git add src/main.cpp
git commit -m "feat: add identity, geometry, and reporting constants"
```

---

## Task 3: Add pure logic helpers (level %, battery %, fake distance)

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Add the helper functions after the constants block.**

These are deliberately pure (no Zigbee/Arduino state) so a human can sanity-check them by reading.

```cpp
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
```

- [ ] **Step 2: Build and verify.**

Run: `pio run`
Expected: `SUCCESS`. The compiler will likely warn that all four helpers
are unused — that's expected, they're used in Task 5.

- [ ] **Step 3: Commit.**

```bash
git add src/main.cpp
git commit -m "feat: add fake distance, level math, and battery helpers"
```

---

## Task 4: Wire up two ZigbeeAnalog endpoints and join the network

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Declare the endpoint instances above `setup()`.**

Insert after the helper functions:

```cpp
ZigbeeAnalog zbDistance(EP_DISTANCE);
ZigbeeAnalog zbLevel(EP_LEVEL);
```

- [ ] **Step 2: Replace `setup()` with the full version: configure both endpoints, join Zigbee, configure reporting.**

```cpp
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
```

- [ ] **Step 3: Build and verify.**

Run: `pio run`
Expected: `SUCCESS`. Flash usage will jump (Zigbee + ESP-IDF link in).
Roughly comparable to the original light-bulb example (~600 KB).

- [ ] **Step 4: Commit.**

```bash
git add src/main.cpp
git commit -m "feat: configure two ZigbeeAnalog endpoints and join network"
```

---

## Task 5: Drive values from `loop()` (distance, level, battery)

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Replace `loop()` with the full version that updates values every tick.**

```cpp
void loop() {
  uint32_t now = millis();

  float d   = fakeDistanceCm(now);
  float pct = computeLevelPct(d);
  zbDistance.setAnalogInput(d);
  zbLevel.setAnalogInput(pct);

  float vbat   = readBatteryVoltage();
  uint8_t bpct = batteryPercent(vbat);
  zbDistance.setBatteryPercentage(bpct);
  zbDistance.setBatteryVoltage(uint8_t(vbat * 10.0f));   // attribute is in 100-mV units
  zbDistance.reportBatteryPercentage();

  delay(kReportTickMs);
}
```

The Zigbee library decides whether the analog-input value change
warrants a real on-the-wire report based on the
`setAnalogInputReporting(min, max, delta)` config. The
`setAnalogInput()` calls every tick are cheap and let the stack make
the decision. Battery has no min/max/delta API — we explicitly call
`reportBatteryPercentage()` each tick (always-on iteration; revisit
when adding deep sleep).

- [ ] **Step 2: Build and verify.**

Run: `pio run`
Expected: `SUCCESS`. The four helper functions from Task 3 should now
be referenced; compiler warnings about unused functions go away.

- [ ] **Step 3: Commit.**

```bash
git add src/main.cpp
git commit -m "feat: drive distance, level, and battery values from loop"
```

---

## Task 6: Add LED status indicator

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Add LED helpers near the top of the file (after the helper functions, before the endpoint declarations).**

`RGB_BUILTIN` is the on-board RGB LED defined by the
`esp32-c6-devkitc-1` board profile; `rgbLedWrite(pin, r, g, b)` writes
24-bit RGB.

```cpp
void ledOff()    { rgbLedWrite(RGB_BUILTIN, 0, 0, 0); }
void ledRed()    { rgbLedWrite(RGB_BUILTIN, 32, 0, 0); }      // dim — 32/255 brightness
void ledBlue()   { rgbLedWrite(RGB_BUILTIN, 0, 0, 32); }

void blinkLedOnReport() {
  ledBlue();
  delay(20);
  ledOff();
}
```

- [ ] **Step 2: Show solid red while joining.**

In `setup()`, replace:

```cpp
  Serial.print("Connecting to network");
  while (!Zigbee.connected()) {
    Serial.print(".");
    delay(100);
  }
```

with:

```cpp
  Serial.print("Connecting to network");
  ledRed();
  while (!Zigbee.connected()) {
    Serial.print(".");
    delay(100);
  }
  ledOff();
```

- [ ] **Step 3: Blink blue per loop tick.**

In `loop()`, immediately before the final `delay(kReportTickMs);`:

```cpp
  blinkLedOnReport();
```

- [ ] **Step 4: Build and verify.**

Run: `pio run`
Expected: `SUCCESS`.

- [ ] **Step 5: Commit.**

```bash
git add src/main.cpp
git commit -m "feat: add LED status (red while joining, blue blink per tick)"
```

---

## Task 7: Add 3-second BOOT-button factory reset

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Add the button helper above `loop()`.**

`BOOT_PIN` (GPIO9 on ESP32-C6) is defined by the `esp32-c6-devkitc-1`
board profile. Active-low (pressed = LOW). 3-second hold triggers
`Zigbee.factoryReset()` which clears NVS-stored Zigbee credentials and
reboots; the device will need to be re-paired in z2m.

```cpp
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
```

- [ ] **Step 2: Configure the pin and call the handler each tick.**

In `setup()`, near the other `pinMode` calls:

```cpp
  pinMode(BOOT_PIN, INPUT_PULLUP);
```

In `loop()`, as the very first line of the body:

```cpp
  handleFactoryResetButton();
```

- [ ] **Step 3: Build and verify.**

Run: `pio run`
Expected: `SUCCESS`.

- [ ] **Step 4: Commit.**

```bash
git add src/main.cpp
git commit -m "feat: add 3-second BOOT-button factory reset"
```

---

## Task 8: Hardware verification (manual — requires the device)

This task can only be done with the FireBeetle 2 C6 connected via USB-C
and a Zigbee coordinator running zigbee2mqtt with `permit_join: true`.

**Files:** None (verification only).

- [ ] **Step 1: Flash the firmware using the BOOT-button dance.**

Per CLAUDE.md "Flashing gotcha":

```
1. Unplug USB-C.
2. Hold the BOOT button on the FireBeetle.
3. While holding BOOT, plug USB-C back in.
4. Run: pio run -t upload
5. Release BOOT once esptool reports "Connected".
```

Expected: esptool succeeds; the device reboots.

- [ ] **Step 2: Open the serial monitor and observe the boot sequence.**

Run: `pio device monitor`

Expected log lines, in order:

```
boot
Adding endpoints
Connecting to network........
Reporting configured
```

The dot count varies; "Reporting configured" must appear within ~30 s
of "Adding endpoints" if a permit-joining coordinator is in range.

LED behaviour:
- Solid dim red while "Connecting to network..." is printing.
- Off after "Reporting configured".
- Brief blue blink once per second thereafter.

- [ ] **Step 3: Confirm the device appears in zigbee2mqtt.**

In the z2m web UI → Devices, look for a new device with:
- `manufacturerName` = `czechit`
- `modelID` = `water-tank-sensor`
- "Supported: No (interview successful)" badge — expected for generic auto-handling.

Endpoints panel for the device should show:
- Endpoint 10: clusters including `genBasic`, `genPowerCfg`, `genAnalogInput`.
- Endpoint 11: clusters including `genAnalogInput`.

- [ ] **Step 4: Confirm values flow.**

In z2m's Exposes view (or watch the MQTT topic
`zigbee2mqtt/<friendly-name>` with any MQTT client):

- `analog_input` on EP 10 oscillates between ~20 cm and ~200 cm over
  ~5 minutes (triangle wave). Reports arrive every 10–60 s.
- `analog_input` on EP 11 oscillates between 0 and 100.
- `battery` matches the actual cell voltage (verify by sticking a
  multimeter on the JST-PH connector and applying the curve: 4.20 V →
  100 %, 3.30 V → 0 %, linear in between).
- `linkquality` is non-zero.

- [ ] **Step 5: Confirm reports are unsolicited (not z2m polling).**

Set z2m log level to `debug` (UI → Settings → Log → `level: debug`,
restart z2m). In the live log:
- Look for `attributeReport` frames originating from the device.
- Ensure they are not preceded by `read` requests from z2m within the
  same second (a `read` followed by a `readResponse` is z2m polling,
  not the device pushing).

- [ ] **Step 6: Confirm factory reset works.**

Hold the BOOT button for 3 s. Expected serial output:

```
Factory reset triggered. Rebooting in 1s.
```

Then the device reboots and re-joins (provided the coordinator still
has `permit_join: true`); z2m should show it leaving and re-joining.
After re-pairing, repeat Step 4 to confirm values still flow.

- [ ] **Step 7: Tag the working firmware.**

If steps 1–6 all pass, snapshot the milestone:

```bash
git tag -a v0.1.0-zigbee-integration -m "Water-tank Zigbee integration verified end-to-end in z2m"
```

(No `git push --tags` — that's the user's call.)

---

## Self-review

- **Spec coverage:**
  - Identity (`czechit / water-tank-sensor`) — Task 4.
  - Two endpoints (10 distance, 11 level) — Task 4.
  - Reporting config (10/60/1.0) — Task 4.
  - Tank geometry constants — Task 2.
  - Triangle-wave fake distance — Tasks 2, 3.
  - Battery sense on GPIO0 with 2:1 divider, linear-clamp SoC — Task 3.
  - Battery reported each tick — Task 5.
  - LED status (red joining, blue blink) — Task 6.
  - 3-sec BOOT factory reset — Task 7.
  - Validation plan walked end-to-end — Task 8.
  - File layout (only `src/main.cpp`) — every task touches just `src/main.cpp`.
- **Placeholders:** none — every step shows complete code or a concrete command + expected output.
- **Type/name consistency:** `EP_DISTANCE`/`EP_LEVEL`, `MFR`/`MODEL`, `kEmptyDistanceCm`/`kFullDistanceCm`, `kRptMin`/`kRptMax`/`kRptDeltaCm`/`kRptDeltaPct`, `kBatteryAdcPin`/`kVbatDividerK` consistent across Tasks 2, 3, 4, 5. `zbDistance`/`zbLevel` consistent in Tasks 4, 5, 6. `handleFactoryResetButton` defined and called in Task 7. `blinkLedOnReport` defined in Task 6 Step 1 and called in Task 6 Step 3.
- **Verification model:** every code task ends in `pio run` + commit; behavioural verification is consolidated in Task 8 because it requires hardware.

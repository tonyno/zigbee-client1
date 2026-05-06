# Deep-sleep duty cycle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert `src/main.cpp` from always-on to a sleepy Zigbee end device that wakes every 60 s, reports distance/level/battery, and deep-sleeps. Remove all LED code. The 60-second cadence is a measurement value — production target is 3600 s, set via a single constant change.

**Architecture:** All work moves into `setup()`; `loop()` becomes empty. Each wake runs: factory-reset check → `Zigbee.setRxOnWhenIdle(false)` → `Zigbee.begin()` (rejoins from NVS in ~3 s on warm wakes) → read sensors → `setAnalogInput` + explicit `reportAnalogInput` per endpoint → `reportBatteryPercentage` → 500 ms radio-flush delay → `esp_deep_sleep_start(60 s)`. NVS persistence of Zigbee credentials makes the cold-boot and timer-wake paths identical.

**Tech Stack:** PlatformIO + pioarduino + Arduino-ESP32 3.3.8 + Espressif `Zigbee.h` library + ESP-IDF 5.5.4. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-05-07-deep-sleep-design.md`

**Verification model (read first):**
This is bare-metal firmware on a Zigbee radio. No unit-test seam without heavy mocking, so each code-touching task ends with `pio run` (must build cleanly, no new warnings) plus a commit. End-to-end behavior is verified on real hardware in the final task — flash, watch the serial monitor through one or two wake/sleep cycles, confirm reports land in zigbee2mqtt, then leave on battery overnight to collect the actual drain number this iteration is built to measure.

If `pio run` warns about unused functions or missing declarations, fix it before committing — don't wait for the final task.

**API references (verified against the installed library):**
- `void ZigbeeCore::setRxOnWhenIdle(bool)` — `ZigbeeCore.h:171`
- `void ZigbeeCore::setTimeout(uint32_t)` — `ZigbeeCore.h:177`
- `bool ZigbeeAnalog::reportAnalogInput()` — `ZigbeeAnalog.h:77`
- `bool ZigbeeEP::reportBatteryPercentage()` — `ZigbeeEP.h:110` (already used)
- `esp_sleep_enable_timer_wakeup(uint64_t us)` and `esp_deep_sleep_start()` — ESP-IDF, included via `Arduino.h`

---

## Task 1: Remove all LED code

**Files:**
- Modify: `src/main.cpp`

This task only deletes code. Behavior changes: no white boot flash, no red join-breath, no color-cycling heartbeat. Zigbee data path is unaffected.

- [ ] **Step 1: Delete the LED constant.**

In `src/main.cpp`, remove this block (currently around line 74):

```cpp
// On-board RGB LED helpers. Brightness 64/255 — visible across the room
// without being obnoxious. Pure off helper avoids leaving stray colours on.
constexpr uint8_t kLedBri = 64;
```

- [ ] **Step 2: Delete the LED helper functions.**

Remove `ledOff()`, `ledWhite()`, `ledJoinBreath()`, `blinkLedOnReport()` and their leading comment blocks. That's the entire run from `void ledOff()` through the closing `}` of `blinkLedOnReport()`.

- [ ] **Step 3: Delete the white boot-flash from `setup()`.**

Remove this block:

```cpp
  // Brief white flash so it's obvious from across the desk that the chip
  // just (re)started — useful when iterating without watching the serial.
  ledWhite();
  delay(80);
  ledOff();
```

- [ ] **Step 4: Replace the join-breath in the connect-loop.**

Find this block in `setup()`:

```cpp
  Serial.print("Connecting to network");
  while (!Zigbee.connected()) {
    Serial.print(".");
    ledJoinBreath();   // ~640 ms per breath — replaces the previous delay(100)
  }
  ledOff();
  Serial.println();
```

Replace with:

```cpp
  Serial.print("Connecting to network");
  while (!Zigbee.connected()) {
    Serial.print(".");
    delay(100);
  }
  Serial.println();
```

- [ ] **Step 5: Remove the heartbeat blink from `loop()`.**

Delete the `blinkLedOnReport();` line in `loop()`.

- [ ] **Step 6: Build clean.**

Run: `pio run`
Expected: `SUCCESS`. Binary should be a hair smaller than before — no LED code. No new warnings.

- [ ] **Step 7: Commit.**

```bash
git add src/main.cpp
git commit -m "refactor: remove RGB LED code (preparing for sleepy ZED)"
```

---

## Task 2: Move factory-reset polling to early in `setup()`

**Files:**
- Modify: `src/main.cpp`

Once deep sleep is in place, `loop()` becomes unreachable and BOOT polling there is dead code. Doing this in a separate task keeps the diff small and lets the device still factory-reset cleanly between tasks.

- [ ] **Step 1: Move the `handleFactoryResetButton()` call.**

In `loop()`, delete this line:

```cpp
  handleFactoryResetButton();
```

In `setup()`, immediately after the existing `pinMode(BOOT_PIN, INPUT_PULLUP);` line, add:

```cpp
  // Factory reset has to be polled here — once the device enters its
  // wake/sleep cycle, loop() is unreachable. To trigger it: press EN
  // (reset) on the FireBeetle, then hold BOOT for 3 s during the brief
  // window before Zigbee.begin().
  handleFactoryResetButton();
```

- [ ] **Step 2: Build clean.**

Run: `pio run`
Expected: `SUCCESS`, no warnings.

- [ ] **Step 3: Commit.**

```bash
git add src/main.cpp
git commit -m "refactor: poll factory-reset in setup() before Zigbee.begin()"
```

---

## Task 3: Convert to sleepy ZED with deep-sleep cycle

**Files:**
- Modify: `src/main.cpp`

This is the substantive change. After this task, `loop()` is empty and the device wakes/sleeps on a 60-second cycle.

- [ ] **Step 1: Replace the reporting constants block with sleep constants.**

Find and delete this block (the four `kRpt*` plus `kReportTickMs`):

```cpp
// ---- Fake distance generator ----
constexpr uint32_t kWavePeriodMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kReportTickMs = 1000;

// ---- Reporting config ----
constexpr uint16_t kRptMin       = 10;     // s — min between reports
constexpr uint16_t kRptMax       = 60;     // s — heartbeat
constexpr float    kRptDeltaCm   = 1.0f;
constexpr float    kRptDeltaPct  = 1.0f;
```

Replace with:

```cpp
// ---- Fake distance generator ----
constexpr uint32_t kWavePeriodMs = 5UL * 60UL * 1000UL;

// ---- Sleep cycle ----
// Test cadence — bump to 3600 (1 h) for production once overnight battery
// drain has been measured and looks acceptable.
constexpr uint32_t kSleepSeconds = 60;
constexpr uint32_t kRadioFlushMs = 500;   // post-report settle before sleep
```

- [ ] **Step 2: Reduce the boot delay.**

Find this in `setup()`:

```cpp
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(2000);
  Serial.println("boot");
```

Replace with:

```cpp
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(50);
  Serial.println("boot");
```

(2 s was for USB-CDC enumeration when watching the serial monitor — pure energy waste on battery, ~30–45 µAh wasted per wake. The serial console catches up after the 50 ms delay anyway.)

- [ ] **Step 3: Add sleepy-mode flags before `Zigbee.begin()`.**

Find this block in `setup()`:

```cpp
  Serial.println("Adding endpoints");
  Zigbee.addEndpoint(&zbDistance);
  Zigbee.addEndpoint(&zbLevel);

  if (!Zigbee.begin()) {
    Serial.println("Zigbee failed to start! Rebooting...");
    delay(1000);
    ESP.restart();
  }
```

Replace with:

```cpp
  Serial.println("Adding endpoints");
  Zigbee.addEndpoint(&zbDistance);
  Zigbee.addEndpoint(&zbLevel);

  // Sleepy end device: tell the coordinator we won't keep our radio on
  // listening between polls. This is the actual power-saving switch —
  // deep sleep alone isn't enough if the coordinator thinks we're rx-on.
  Zigbee.setRxOnWhenIdle(false);
  Zigbee.setTimeout(10000);   // 10 s join timeout (default 30 s, longer = more battery)

  if (!Zigbee.begin()) {
    Serial.println("Zigbee failed to start! Rebooting...");
    delay(1000);
    ESP.restart();
  }
```

- [ ] **Step 4: Remove the reporting-binding configuration.**

Find and delete this block at the end of `setup()`:

```cpp
  zbDistance.setAnalogInputReporting(kRptMin, kRptMax, kRptDeltaCm);
  zbLevel.setAnalogInputReporting(kRptMin, kRptMax, kRptDeltaPct);
  Serial.println("Reporting configured");
```

Don't replace it with anything — the next steps add explicit per-wake reports instead.

- [ ] **Step 5: Append the measure → report → sleep block to `setup()`.**

After the (now-removed) reporting-config block, add the per-wake work that used to live in `loop()`. Add this at the end of `setup()`, just before the closing `}`:

```cpp
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
```

- [ ] **Step 6: Empty out `loop()`.**

Find the existing `loop()` body (now down to a few lines after Tasks 1 and 2):

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

  Serial.println("tick: distance=" + String(d, 1) + "cm level=" + String(pct, 1) + "% vbat=" + String(vbat, 2) + "V (" + String(bpct) + "%)");
}
```

Replace the entire `loop()` function with:

```cpp
void loop() {
  // Empty — every wake runs setup() to completion, then deep-sleeps.
  // loop() is only reached if esp_deep_sleep_start() ever returns, which
  // it doesn't.
}
```

- [ ] **Step 7: Build clean.**

Run: `pio run`
Expected: `SUCCESS`. The binary may shrink slightly (reporting config code paths gone). No new warnings — in particular, none about unused functions, unused variables, or implicit declarations of `esp_sleep_*`.

If you see a warning that `esp_sleep_enable_timer_wakeup` is implicit, add `#include "esp_sleep.h"` near the top of the file (after `#include "Zigbee.h"`).

- [ ] **Step 8: Commit.**

```bash
git add src/main.cpp
git commit -m "feat: convert to sleepy ZED with 60s deep-sleep cycle"
```

---

## Task 4: Update CLAUDE.md to reflect the new operational mode

**Files:**
- Modify: `CLAUDE.md`

The "Operational pattern", "Current status", and "TODO" sections are now stale.

- [ ] **Step 1: Update the Operational pattern section.**

Find this in `CLAUDE.md`:

```markdown
## Operational pattern

**Current iteration (always-on):** the firmware runs continuously, reports
on Zigbee min/max/delta intervals (10–60 s), and uses a fake distance
generator so the Zigbee data path can be exercised without the ToF sensor.
Battery is measured for real on GPIO0.

**Final target (sleepy ZED, deferred — see TODO list):**

- Boot → join/rejoin Zigbee network → take measurement → report attribute → wait for ZCL ack → `esp_deep_sleep_start()`.
- Deep sleep duration: ~3600 seconds.
- Network credentials persist in NVS across deep sleep, so `Zigbee.begin()` rejoins automatically (~3 s reconnect time observed in similar projects).
- Light sleep / instant command response is NOT supported in the Arduino framework. Don't try to add it. If that's needed later, the project must move to ESP-IDF + esp-zigbee-sdk directly.
```

Replace with:

```markdown
## Operational pattern

**Current behavior (sleepy ZED):**

- All work happens in `setup()`. `loop()` is empty.
- Each wake: factory-reset check → `Zigbee.setRxOnWhenIdle(false)` → `Zigbee.begin()` (rejoins from NVS) → measure → `setAnalogInput` + `reportAnalogInput` per endpoint → `reportBatteryPercentage` → 500 ms radio-flush delay → `esp_deep_sleep_start(kSleepSeconds × 1e6)`.
- `kSleepSeconds = 60` is the current test cadence (chosen for measuring overnight battery drain). Production target is `3600` (one hour) once drain numbers look acceptable — a single-constant change.
- Network credentials persist in NVS across deep sleep, so warm-wake rejoins are fast (~3 s observed in similar ESP32-C6 projects).
- Light sleep / instant command response is NOT supported in the Arduino framework. Don't try to add it. If that's needed later, the project must move to ESP-IDF + esp-zigbee-sdk directly.
- Factory reset works only during the cold-boot window: press EN, then hold BOOT for 3 s before `Zigbee.begin()` is called. There's no LED indication.
```

- [ ] **Step 2: Update the Current status section.**

Find this in `CLAUDE.md`:

```markdown
## Current status

- ✅ Toolchain validated: pioarduino + Arduino + Zigbee + ESP32-C6 building, flashing, and running on the DFR1075.
- ✅ Default Zigbee on/off light bulb example flashed successfully (619 KB firmware, 46% of 1.25 MB app slot used, 33 KB RAM).
- ✅ Zigbee device-profile design approved (see spec linked above).
- ✅ Firmware written: always-on water-tank-sensor with fake triangle-wave distance, real GPIO0 battery sense, two ZigbeeAnalog endpoints, identity strings, factory-reset button, color-cycling RGB LED heartbeat. Builds cleanly (`pio run`).
- ✅ Verified on hardware: device pairs in z2m as `czechit / water-tank-sensor`, manual refresh returns correct values (distance/level/battery), serial monitor shows triangle-wave running.
- ✅ z2m external converter shipped (`z2m/external_converters/czechit-water-tank-sensor.js`) so unsolicited reports flow without manual binding.
- ⏳ Now: copy converter into z2m, restart, re-pair, confirm reports flow without manual refresh.
```

Replace with:

```markdown
## Current status

- ✅ Toolchain validated: pioarduino + Arduino + Zigbee + ESP32-C6 building, flashing, and running on the DFR1075.
- ✅ Zigbee device-profile design approved (see spec linked above).
- ✅ Always-on firmware verified on hardware: device pairs in z2m as `czechit / water-tank-sensor`, distance/level/battery reportable.
- ✅ z2m external converter shipped (`z2m/external_converters/czechit-water-tank-sensor.js`).
- ✅ Sleepy ZED with 60-second deep-sleep cycle implemented; LEDs removed; factory reset moved to early `setup()`.
- ⏳ Now: flash, re-pair if needed, leave on a fully-charged 18650 overnight, record battery % delta in z2m the next morning. That number tells us whether the multi-year-on-battery target is achievable at this cadence.
```

- [ ] **Step 3: Update the TODO list.**

Find this section in `CLAUDE.md`:

```markdown
## TODO (deferred follow-ups)

In rough priority order:

1. **Wire the VL53L1X ToF sensor** on I²C and replace the fake distance
   generator with real measurements. Confirm DFR1075 default I²C pins
   from the wiki.
2. **Refine battery SoC mapping.** Replace the linear `batteryPercent()`
   with a multi-point Li-Po discharge curve once drift is observed.
   Recalibrate the divider ratio against a multimeter (wiki says 2.0;
   community reports ~2.1218).
3. **Add deep-sleep cycle.** Restructure `loop()` → `setup()`-only flow:
   single report and `esp_deep_sleep_start(3600 × 1e6)`. Drop the LED
   indicator (battery cost). Tune cadence for multi-year target.
4. **Single-`factory` partition layout.** Switch from the dual-OTA layout
   once OTA flexibility is no longer wanted (frees ~1.4 MB; wipes NVS,
   forcing a re-pair).
```

Replace with:

```markdown
## TODO (deferred follow-ups)

In rough priority order:

1. **Wire the VL53L1X ToF sensor** on I²C and replace the fake distance
   generator with real measurements. Confirm DFR1075 default I²C pins
   from the wiki.
2. **Refine battery SoC mapping.** Replace the linear `batteryPercent()`
   with a multi-point Li-Po discharge curve once drift is observed.
   Recalibrate the divider ratio against a multimeter (wiki says 2.0;
   community reports ~2.1218).
3. **Tune `kSleepSeconds` to the production cadence.** After the overnight
   drain measurement, raise the constant from 60 to 3600 (or whatever
   number the math says hits the multi-year battery target). Single-
   constant change in `src/main.cpp`.
4. **Wait for ZCL report ack instead of fixed `delay(kRadioFlushMs)`.**
   Use `Zigbee.onGlobalDefaultResponse(...)` to sleep as soon as the
   coordinator confirms delivery, rather than waiting a fixed 500 ms.
   Saves a small but non-trivial amount of active time per wake. The
   pattern is in the upstream `Zigbee_Temp_Hum_Sensor_Sleepy` example.
5. **Single-`factory` partition layout.** Switch from the dual-OTA layout
   once OTA flexibility is no longer wanted (frees ~1.4 MB; wipes NVS,
   forcing a re-pair).
```

- [ ] **Step 4: Commit.**

```bash
git add CLAUDE.md
git commit -m "docs: update CLAUDE.md for sleepy ZED operational mode"
```

---

## Task 5: Hardware validation

**Files:** none — this task is fully on-hardware.

The previous tasks ensure the firmware builds and the spec/docs reflect the new mode. This task confirms the device actually behaves correctly on hardware and kicks off the overnight measurement that motivated the iteration.

- [ ] **Step 1: Flash the firmware.**

The device is asleep most of the time, so USB-CDC is unresponsive at any given moment. Use the BOOT-button flash procedure:

1. Unplug USB-C from the FireBeetle.
2. Hold the BOOT button on the board.
3. While holding BOOT, plug USB-C back in.
4. Run: `pio run -t upload`
5. Release BOOT once esptool reports "Connected".

Expected: upload succeeds, esptool resets the chip.

- [ ] **Step 2: Watch one cold-boot wake/sleep cycle on the serial monitor.**

Run: `pio device monitor`

Expected serial output sequence on cold boot (or first plug-in after flashing):

```
boot
Adding endpoints
Connecting to network........
reported: distance=... level=... vbat=... V (...%)
Sleeping for 60 s
```

After the "Sleeping for 60 s" line, the monitor goes silent. After ~60 s the chip wakes; you'll see another `boot` line, a much shorter `Connecting to network` (because rejoin from NVS is fast — a couple of dots), another `reported:` line, then `Sleeping for 60 s` again.

If the device keeps cold-booting (long join sequence every wake instead of fast rejoin), NVS isn't preserving credentials. Stop and investigate before continuing to step 3 — that would invalidate the overnight measurement.

- [ ] **Step 3: Confirm the device pairs (or re-pairs) in zigbee2mqtt.**

Open the z2m UI. The device should appear as `czechit / water-tank-sensor` (Endpoint 10 has Analog Input + Power Configuration; Endpoint 11 has Analog Input). With z2m at debug log level, you should see attribute-report frames originating from the device every ~60 s — the `last_seen` timestamp updates each wake.

If you previously paired this device and it shows up as offline / interview-failed, factory-reset it (EN button + hold BOOT for 3 s during the cold-boot window) and re-pair.

- [ ] **Step 4: Confirm the values are sane.**

In z2m:

- `analog_input` on EP 10 ramps between ~20 cm and ~200 cm over 5 minutes (triangle wave).
- `analog_input` on EP 11 ramps between ~0 % and ~100 % in lockstep.
- `battery` reflects the actual cell voltage (cross-check with a multimeter on the JST connector if you've never validated this device's mapping).
- `linkquality` is non-zero.

- [ ] **Step 5: Run the overnight measurement.**

1. Charge the 18650 to 100 % (USB plugged into FireBeetle's charger; status LED goes from red to off when charged — see DFR1075 wiki for the exact indicator).
2. Note the battery percentage shown in z2m at this moment. Record it (and the time).
3. Unplug USB. The device is now on battery only.
4. Leave running overnight. Don't fiddle with z2m settings during the run — keep it on debug log only if you want to count exactly how many reports made it.
5. The next morning, note the battery percentage and time again.

Compute drain:

```
drain_per_hour = (start_pct - end_pct) / hours_elapsed
estimated_total_runtime_hours = start_pct / drain_per_hour
```

Whatever number this produces is the headline result of this iteration. If it projects to 1+ year on a single 18650 even at 60-second cadence, the production cadence (3600 s = one update per hour) will run for many years. If it's days or weeks, deeper power work is needed (move to ESP-IDF + esp-zigbee-sdk, hardware power-rail changes, deep-sleep current investigation) before the multi-year target is realistic.

- [ ] **Step 6: Record the result.**

Append a one-line note to the Current status section of `CLAUDE.md` with the numbers, e.g.:

```
- ✅ Overnight drain measurement: 92 % → 87 %, 5 % over 9 h at 60-second cadence → projects ~750 h (~31 d) on full charge at this cadence; with production 3600-second cadence this scales to ~62× longer (rejoin/active time dominates) → production projection: well over a year.
```

(The "scales to ~62×" arithmetic is rough — at 1-h cadence the active time per cycle is the same but the cycles per hour drop from 60 to 1, so total active time per day drops by ~60×. Sleep current is small compared to active current, so most of the drain is active-time-bound. The real number gets calibrated once production cadence is also measured for a night.)

```bash
git add CLAUDE.md
git commit -m "docs: record overnight battery-drain measurement"
```

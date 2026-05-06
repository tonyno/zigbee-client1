---

title: Water-tank sensor — sleepy ZED with deep-sleep duty cycle  
date: 2026-05-07  
status: approved

---

# Water-tank sensor — sleepy ZED with deep-sleep duty cycle

## Goal

Convert the water-tank distance sensor firmware from always-on to a  
**sleepy Zigbee end device** that wakes on a timer, takes a single  
measurement, reports it to the coordinator, and returns to deep sleep.  
This is the persistent operating mode going forward — the always-on  
iteration was a stepping stone that has done its job (data path validated  
on hardware, pairing in z2m confirmed).

The immediate motivation is to collect overnight battery-drain data on a  
single 18650 cell with a 60-second wake-cycle. That number tells us  
whether the multi-year-on-battery target is achievable at this firmware's  
quiescent draw, or whether deeper power work is needed (move to ESP-IDF +  
esp-zigbee-sdk, hardware power-rail changes, etc.) before raising the  
cadence to the production target of 3600 s.

## Non-goals

*   Real distance sensing — the triangle-wave fake generator stays.  
    Replacing it with VL53L1X is a separate follow-up (CLAUDE.md TODO #1).
*   Multi-point Li-Po discharge curve — the linear-with-clamp mapping  
    stays (TODO #2).
*   Single-`factory` partition layout — separate change, wipes NVS (TODO #4).
*   Light sleep with instant command response — not supported in the  
    Arduino framework. If ever needed, requires migrating to ESP-IDF +  
    esp-zigbee-sdk directly (already documented in CLAUDE.md).
*   LED status indication — removed entirely (see below).

## Operational pattern

```
power-on  ────────────────────────────────────────────────────────────────┐
                                                                          │
   ┌──── cold boot ──────────────────────────────────────────────────┐    │
   │                                                                  │    │
   │  setup():                                                        │    │
   │    1. Serial.begin, brief delay(50)                              │    │
   │    2. check BOOT pin → 3 s hold = factoryReset() & reboot        │    │
   │    3. configure ADC, endpoints                                   │    │
   │    4. Zigbee.setRxOnWhenIdle(false)   ← sleepy mode              │    │
   │    5. Zigbee.begin() → join (cold) or rejoin from NVS (~3 s)     │    │
   │    6. read VBAT, compute distance/level                          │    │
   │    7. setAnalogInput(...) on EP10/EP11; reportBatteryPercentage()│    │
   │    8. delay(500)  — let radio flush queued frames                │    │
   │    9. esp_sleep_enable_timer_wakeup(60 × 1e6)                    │    │
   │   10. esp_deep_sleep_start()  ← does not return                  │    │
   │                                                                  │    │
   │  loop(): empty (never reached)                                   │    │
   └──────────────────────────────┬───────────────────────────────────┘    │
                                  │                                        │
                                  ▼                                        │
                       ESP32-C6 deep sleep (~60 s)                         │
                                  │                                        │
                                  ▼ timer wake                             │
                              re-enter setup() ───────────────────────────┘
```

NVS persists the Zigbee credentials across deep sleep, so the rejoin path  
on every wake after the first pairing is identical to a coordinator-side  
"data poll": fast (~3 s observed in similar ESP32-C6 sleepy projects).

## Sleepy mode

The single most important power knob is `Zigbee.setRxOnWhenIdle(false)`.  
A non-sleepy ZED keeps its radio on listening for parent polls; a sleepy  
ZED tells the coordinator to buffer commands and only listens briefly at  
poll boundaries. Without this flag, deep sleep alone wouldn't help — the  
coordinator would keep retrying transmissions to a "always-on" child.

This must be called **before** `Zigbee.begin()` so the announce frame  
carries the right capability bits.

## Reporting model

Each wake does **one explicit push per attribute**, then sleeps. The  
existing min/max/delta-based reporting bindings  
(`setAnalogInputReporting(10, 60, 1.0)`) only make sense for a device  
that's continuously evaluating deltas — meaningless when asleep most of  
the time.

Implementation uses whichever of these the Arduino-ESP32 Zigbee 3.3.8  
ZigbeeAnalog API exposes (to be confirmed against the header at  
implementation time):

*   **Preferred — explicit report method.** If `ZigbeeAnalog` exposes  
    `reportAnalogInput()` or `report()`, use it. `setAnalogInput(v)`  
    updates the cached attribute, then the explicit method pushes to the  
    coordinator unconditionally. Cleanest separation of "value update"  
    from "send".
*   **Fallback — permissive reporting binding.** If no explicit report  
    method exists, configure  
    `setAnalogInputReporting(min=0, max=1, delta=0.0)` so the stack  
    reports on every `setAnalogInput()` call, then call `setAnalogInput()`  
    once per wake. The min=0/max=1/delta=0 triple effectively means  
    "report on any value, immediately, no rate-limiting" which gives the  
    same observable behavior.

Battery reporting on the base `ZigbeeEP` already has the explicit  
`reportBatteryPercentage()` method (used in the current firmware). That  
stays.

Wake-cycle steps:

```cpp
zbDistance.setAnalogInput(d);                                  // value
zbLevel.setAnalogInput(pct);                                   // value
//   then either reportAnalogInput() per endpoint, or rely on
//   permissive reporting binding (see above)
zbDistance.setBatteryPercentage(bpct);
zbDistance.setBatteryVoltage(uint8_t(vbat * 10.0f));
zbDistance.reportBatteryPercentage();                          // explicit
delay(kRadioFlushMs);                                          // 500 ms
esp_deep_sleep_start();                                        // no return
```

If 500 ms turns out to be too short or too long once measured on the bench,  
this is a one-line tuning change.

## LED removal

Deleted entirely (no `#define` gate):

*   `kLedBri` constant
*   `ledOff()`, `ledWhite()`, `ledJoinBreath()`, `blinkLedOnReport()`
*   White boot flash in `setup()`
*   Red join-breath while waiting for `Zigbee.connected()`
*   Color-cycling heartbeat in `loop()`

Visual feedback is no longer worth the energy cost, and the device has  
been verified on hardware — debugging now happens via z2m logs and the  
USB serial console (when plugged in).

If LED debugging is ever needed again it can be added back from git  
history; we don't keep both code paths around.

## Factory reset

Moved from `loop()` (no longer reached) to early in `setup()`, before  
`Zigbee.begin()`. Procedure:

1.  Press EN (reset) on the FireBeetle to force a cold boot.
2.  Hold BOOT for 3 s during the boot window (the brief period before  
    `Zigbee.begin()` is called).
3.  `Zigbee.factoryReset()` clears NVS-stored credentials and reboots.

This is the same physical pattern as flash mode, just with EN+BOOT held in  
sequence instead of BOOT-during-USB-connect. The 3-second hold guard  
remains (debounce + intent confirmation).

The asleep-most-of-the-time nature of the device means BOOT cannot be  
polled while sleeping. This is acceptable — factory reset is a rare,  
deliberate action, and the EN button gives an instant cold-boot path.

## Configuration constants

Added to `src/main.cpp`:

```cpp
constexpr uint32_t kSleepSeconds = 60;          // wake every minute (test)
constexpr uint32_t kRadioFlushMs = 500;         // post-report settle delay
```

`kSleepSeconds = 60` for the overnight test. Once drain numbers are  
acceptable, bump to `3600` (one hour) for production. No code changes  
needed beyond that one constant — the rest of the firmware is cadence-  
agnostic.

Removed from `src/main.cpp` (no longer relevant):

*   `kReportTickMs` (loop-tick cadence — no loop)
*   `kRptMin`, `kRptMax`, `kRptDeltaCm`, `kRptDeltaPct` (named min/max/  
    delta reporting bindings — replaced by either explicit per-wake  
    report calls, or a single permissive `setAnalogInputReporting(0, 1,  
    0)`, depending on which API path the implementation takes; see  
    Reporting model)
*   `kLedBri` (no LED)

## Startup delay

The current firmware does `delay(2000)` after `Serial.begin()` to give the  
USB host time to enumerate so the "boot" line is visible in the monitor.  
That delay runs on every wake from deep sleep — at typical ESP32-C6  
active-mode draw (~50–80 mA with radio idle), 2 seconds is ~30–45 µAh per  
wake. Over an hour of 60-second cycles that's ~2–3 mAh purely on a delay  
that does nothing when the device is on battery. Small in absolute terms,  
but the whole point of this iteration is shaving down avoidable active  
time, so it goes.

Reduced to `delay(50)`. The serial monitor still picks up later log lines  
(boot banner, join trace, report trace) once it has reconnected. The  
Zigbee `Serial.setTxTimeoutMs(0)` already prevents `Serial.print` from  
blocking when no host is attached.

## Battery measurement

Unchanged from the previous spec: GPIO0 + on-board 2:1 divider, 16-sample  
average, linear-with-clamp `batteryPercent()` between 4.20 V (full) and  
3.30 V (empty). The point of this iteration is precisely to measure how  
this number drops over a night of 60-second wakes.

## Files changed

```
src/main.cpp                                              ← restructured
docs/superpowers/specs/2026-05-07-deep-sleep-design.md    ← this document
CLAUDE.md                                                 ← Operational
                                                            pattern, Current
                                                            status, TODO list
```

The `partitions.csv` and `platformio.ini` files do **not** change. Same  
4 MB dual-OTA layout, same Zigbee end-device build flag.

## CLAUDE.md updates

The "Operational pattern" section needs to flip:

*   Old text: "Current iteration (always-on)" — describe the new sleepy  
    ZED as the **current** behavior, with a `kSleepSeconds = 60` test  
    interval and a note that production target is 3600.
*   "Final target (sleepy ZED, deferred)" subsection: collapse, since the  
    current iteration *is* the sleepy ZED. Keep the bullet about light  
    sleep being unsupported in Arduino framework.

The "Current status" section adds:

*   Sleepy ZED with 60-second wake cycle implemented.
*   LEDs removed.
*   Pending: overnight battery-drain measurement.

The "TODO (deferred follow-ups)" list:

*   TODO #3 (deep sleep) → mark done; replace with "Tune `kSleepSeconds`  
    to production cadence after measuring drain."

## Validation plan

In order:

1.  **Build green.** `pio run` succeeds; binary size sanity-check (should  
    be smaller — LED helpers gone).
2.  **Cold boot joins.** With z2m at debug log level, watch the device  
    pair as `czechit / water-tank-sensor` exactly as in the previous  
    iteration. The capability bits in the announce frame should reflect  
    sleepy mode (`rxOnWhenIdle = false`).
3.  **First report arrives within ~5 s of join**, with sane values  
    (distance, level, battery).
4.  **Device sleeps.** USB power draw drops to deep-sleep idle (a few µA,  
    if measurable; on USB the regulator dominates). Serial console goes  
    quiet for ~60 s.
5.  **Wake/report cycle.** Every 60 s, z2m receives a fresh attribute  
    report frame. `last_seen` in z2m updates. Battery percentage shows  
    the actual cell value.
6.  **Factory reset still works.** EN + 3 s BOOT hold during cold-boot  
    window → device leaves the network → re-pairs cleanly on next boot.
7.  **Overnight battery drain.** Run on a fully-charged 18650 from  
    bedtime to wake-up; record battery % at start and end via z2m. The  
    delta over ~8 hours is the headline number this iteration produces.

Step 7 is the entire point of the iteration. Steps 1–6 just confirm  
nothing regressed.

## Risks and unknowns

*   **`setRxOnWhenIdle(false)` API name.** I'm 90 % sure this exists in  
    Arduino-ESP32 Zigbee 3.3.8 (referenced in the sleepy temp/hum  
    example). If the actual symbol differs, the implementation will  
    discover and adjust — design intent is unchanged.
*   **ZigbeeAnalog explicit report API.** The Reporting model section  
    above has two paths: explicit `reportAnalogInput()`/`report()`, or  
    fallback to a permissive `setAnalogInputReporting(0, 1, 0)` binding.  
    The implementation will pick whichever the 3.3.8 header exposes;  
    observable behavior is identical (one report per wake).
*   **z2m's reaction to the sleepy flag.** The existing converter shipped  
    with this repo (`z2m/external_converters/czechit-water-tank-sensor.js`)  
    binds reporting in `configure()`. With sleepy mode the bindings still  
    install on pairing but periodic reports come from explicit pushes,  
    not a min/max/delta engine — z2m doesn't care which, it just renders  
    whatever frames arrive.
*   **Rejoin time.** "~3 s" is a community number for similar ESP32-C6  
    Zigbee setups. If real-world rejoin runs 10 s+, the duty cycle is  
    much worse than expected and the production cadence may need to be  
    longer (e.g. 3600 s with a 20-second active window is fine; 600 s  
    with 20-second active is a ~3 % duty cycle which is not sleepy at all).
*   **Coordinator child-table.** Some coordinators age out children that  
    don't poll. ZHA/z2m on a typical Sonoff/CC2652 stick handles ~1-hour  
    silence fine; longer cadences may need testing once we get there.

## Open questions

None for this iteration. The 60-second test cadence, LED removal, and  
explicit single-report-per-wake model are all decided. Tuning constants  
(sleep duration, radio flush window) will be adjusted after the overnight  
measurement.

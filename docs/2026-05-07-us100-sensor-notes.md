# US-100 ultrasonic sensor — energy-efficient integration notes

Date: 2026-05-07
Status: design idea, not yet implemented. Replaces the planned VL53L1X ToF
sensor (see CLAUDE.md TODO item 1) if we go this route.

## Why US-100 (vs HC-SR04 or VL53L1X)

| Property              | HC-SR04   | US-100        | VL53L1X      |
| --------------------- | --------- | ------------- | ------------ |
| Supply voltage        | 5 V       | 2.4–5.5 V     | 2.6–3.5 V    |
| Active current        | ~15 mA    | ~2 mA (≤8 mA) | ~20 mA       |
| Quiescent current     | ~2 mA    | ~2 µA         | ~5 µA        |
| Temp compensation     | no        | yes (UART)    | n/a (optical)|
| Beam cone             | ~15°      | ~15°          | very narrow  |
| Min range             | ~2 cm     | ~2 cm         | ~4 cm        |
| Max range             | ~4 m      | ~4.5 m        | ~4 m         |
| Works on water surface| good      | good          | OK, but glare risk |

US-100 is the sweet spot for a battery-powered water-tank sensor:
3.3 V-native (no level shifters), temperature-compensated when used in
UART mode, and order-of-magnitude lower current than HC-SR04.

## Use UART mode, not trigger/echo

Install the jumper on the back of the module so it speaks UART.

Wins over GPIO trigger/echo mode:

- Works at 3.3 V supply directly. Signals are already 3.3 V — no level
  shifter, no charge pump.
- Built-in temperature compensation. You can also query temperature
  separately with `0x50` (returns `temp_C + 45`).
- Lower current (no continuous trigger pulses needed).

Active current ~2 mA, ≤8 mA on the transmit burst. Quiescent ~2 µA —
basically free if you ever leave it powered.

## Wiring — GPIO power-gating, no MOSFET needed

ESP32-C6 GPIO pins source 20–40 mA. The US-100 needs only ~2 mA
(≤8 mA spike), so one GPIO can drive `VCC` directly:

```
GPIO_PWR  ──► US-100 VCC
GND       ──► US-100 GND
GPIO_TX   ──► US-100 RX     (any UART1 TX pin)
GPIO_RX   ──◄ US-100 TX     (any UART1 RX pin)
```

No MOSFET, no level shifter. Pick `GPIO_PWR` as a normal digital pin
(does not need to be RTC-capable since we re-init every wake from
`setup()`).

## Per-wake measurement sequence

Inside `setup()`, before `esp_deep_sleep_start()`:

1. `pinMode(GPIO_PWR, OUTPUT); digitalWrite(GPIO_PWR, HIGH);`
2. Wait **~50 ms** for the module to boot and stabilize.
3. `Serial1.begin(9600, SERIAL_8N1, GPIO_RX, GPIO_TX);`
4. `Serial1.write(0x55);` → read 2 bytes → `distance_mm = (hi << 8) | lo`.
   - 0xFFFF = out-of-range / no echo.
5. (Optional) `Serial1.write(0x50);` → read 1 byte → `temp_C = byte − 45`.
6. `Serial1.end();`
7. `digitalWrite(GPIO_PWR, LOW); pinMode(GPIO_PWR, INPUT);` (high-Z) so
   nothing leaks back through the pin during deep sleep.

Total active time ~50–80 ms.

## Energy budget

- Per wake: ~80 ms × 2 mA ≈ **~3 µAh**.
- Over a 1 h cycle: averages to ~3 µA.
- Versus Zigbee join cost (~30–50 mAs per wake): the sensor is no longer
  the limiting factor on battery life.

## Caveats / things to verify on hardware

- **Beam cone ~15°**: mount the sensor away from tank sidewalls; off-axis
  echoes from the wall can ghost on shallow tanks. Aim straight down,
  centered.
- **Minimum range ~2 cm**: don't mount too close to a full tank.
- **Foam / floating debris** can scatter echoes. UART mode returns a
  median over a few pings internally, which helps.
- **3.3 V vs 5 V accuracy**: spec is given at 5 V. At 3.3 V the module
  still works but max range may drop slightly (~3.5 m instead of ~4.5 m).
  Fine for water tanks under ~3 m deep.
- **Boot delay**: the 50 ms figure is conservative — measure on the
  actual unit and tune down if possible to save energy.

## Open questions before committing

1. Do we still want VL53L1X for any use case, or is US-100 the new
   default? (US-100 has a wider cone, which is actually better for
   liquid surfaces — VL53L1X can be confused by reflective water.)
2. Which UART pins on the FireBeetle 2 C6? Need to pick non-strapping
   pins that survive deep sleep cleanly.
3. Calibrate `kEmptyDistanceCm` / `kFullDistanceCm` against a real tank.

## Next step if we go ahead

Run through `superpowers:brainstorming` to confirm the design change,
then update:

- `docs/superpowers/specs/2026-05-06-water-tank-zigbee-design.md` —
  swap VL53L1X for US-100 in the sensor section.
- `CLAUDE.md` TODO item 1 — replace VL53L1X with US-100 + UART mode.
- Firmware — add `Us100Sensor` driver replacing the fake triangle-wave
  generator, with the power-gating sequence above.

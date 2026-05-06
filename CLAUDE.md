# Project Context for Claude Code

## What this project is

A Zigbee distance sensor running on an ESP32-C6 board. The sensor wakes from
deep sleep approximately once per hour, takes a single distance measurement,
reports it over Zigbee to a coordinator (Home Assistant via ZHA or
Zigbee2MQTT), and goes back to deep sleep. Optimized for multi-year battery
life on an 18650 Li-Ion cell.

Currently the project is a working starter from
https://github.com/technoo10201/esp32-c6-zigbee-example-platformio
(Zigbee on/off light bulb example), used to validate the toolchain. The next
step is to replace the example logic with the sleepy distance sensor pattern.

## Hardware

- **Board:** DFRobot FireBeetle 2 ESP32-C6 (SKU: DFR1075)
  - Wiki: https://wiki.dfrobot.com/SKU_DFR1075_FireBeetle_2_Board_ESP32_C6
  - Bought at: https://botland.cz/desky-kompatibilni-s-arduino-dfrobot/25402-firebeetle-2-esp32-c6-wifi-bluetooth-zigbee-matter-dfrobot-dfr1075-6959420924509.html
- **MCU:** ESP32-C6FH4 (QFN32), revision v0.2, RISC-V 160 MHz, 320 KB SRAM, **4 MB embedded flash**
- **MAC (BASE):** `9c:13:9e:cc:3d:10`
- **Connectivity:** Wi-Fi 6, BLE 5, IEEE 802.15.4 (Zigbee 3.0, Thread 1.3, Matter)
- **Power:** USB-C, 5V DC, or solar; integrated Li-Ion charger and battery monitoring
- **Distance sensor (planned):** VL53L1X ToF over I²C (low power, 3.3V, ~20 mA only during ~30 ms measurement). HC-SR04 considered but rejected — too power-hungry and needs 5V.

## Operational pattern (target)

- Sleepy Zigbee End Device (ZED).
- Boot → join/rejoin Zigbee network → take measurement → report attribute → wait for ZCL ack → `esp_deep_sleep_start()`.
- Deep sleep duration: ~3600 seconds.
- Network credentials persist in NVS across deep sleep, so `Zigbee.begin()` rejoins automatically (~3 s reconnect time observed in similar projects).
- Light sleep / instant command response is NOT supported in the Arduino framework. Don't try to add it. If that's needed later, the project must move to ESP-IDF + esp-zigbee-sdk directly.

## Toolchain

- **IDE:** VS Code + PlatformIO extension (Arduino IDE explicitly not used).
- **Platform:** **pioarduino fork** of platform-espressif32, NOT the upstream PlatformIO Labs platform.
  - Upstream `espressif32` does NOT support the Arduino framework on ESP32-C6 (issue: https://github.com/platformio/platform-espressif32/issues/1694, still open).
  - pioarduino: https://github.com/pioarduino/platform-espressif32
- **Versions in use (verified working):**
  - pioarduino platform: 55.3.38
  - Arduino-ESP32 core: 3.3.8
  - ESP-IDF: 5.5.4
  - Toolchain: riscv32-esp 14.2.0+20260121
  - Zigbee library: 3.3.8 (bundled with arduino-esp32, auto-included)

## platformio.ini (verified working)

```ini
[env:firebeetle2_esp32c6]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
board = esp32-c6-devkitc-1
framework = arduino
monitor_speed = 115200

board_upload.flash_size = 4MB
board_build.flash_size = 4MB
board_build.partitions = partitions.csv

build_flags =
    -DZIGBEE_MODE_ED=1
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1

monitor_filters = esp32_exception_decoder
```

Important configuration notes:
- The board profile `esp32-c6-devkitc-1` defaults to 8 MB flash. The DFR1075 is 4 MB. **Both** `board_upload.flash_size` and `board_build.flash_size` must be set or the bootloader gets baked with wrong settings (boot loops or partition verify fails).
- The HARDWARE summary line in `pio run` output cosmetically reports "8MB Flash" regardless — this is a known quirk and is misleading. The truth is in the verbose esptool invocations. Always verify with:
```bash
  pio run -t clean
  pio run -v 2>&1 | grep -- "--flash-size"
```
  Both lines (bootloader + firmware) must show `--flash-size 4MB`.
- `ZIGBEE_MODE_ED=1` is required to compile in End Device mode. Other modes (`ZIGBEE_MODE_RCP`, `ZIGBEE_MODE_ZCZR` for coordinator) exist but are not relevant here.

## partitions.csv (4 MB layout, current — has OTA)

```csv
# Name,     Type, SubType, Offset,  Size, Flags
nvs,        data, nvs,     0x9000,  0x5000,
otadata,    data, ota,     0xe000,  0x2000,
app0,       app,  ota_0,   0x10000, 0x140000,
app1,       app,  ota_1,   0x150000,0x140000,
spiffs,     data, spiffs,  0x290000,0x15B000,
zb_storage, data, fat,     0x3EB000,0x4000,
zb_fct,     data, fat,     0x3EF000,0x1000,
coredump,   data, coredump,0x3F0000,0x10000,
```

`zb_storage` and `zb_fct` are required for the Zigbee stack — do not remove.

### Note on OTA partitions

The `app0` + `app1` dual-OTA layout consumes 2.5 MB of the 4 MB flash for a feature
that isn't actually usable in this project's framework:

- Wi-Fi HTTP OTA: not applicable, no Wi-Fi.
- Zigbee OTA Upgrade Cluster: supported by esp-zigbee-sdk but **NOT exposed by the Arduino `Zigbee.h` library**. Would also be too slow/power-hungry for a battery sensor.
- USB reflash always works regardless of OTA partitions (just rewrites the active app slot).

A single-`factory` layout would give ~3 MB for app + filesystem and is recommended once the
device is field-deployed:

```csv
# Name,     Type, SubType, Offset,  Size, Flags
nvs,        data, nvs,     0x9000,  0x6000,
phy_init,   data, phy,     0xf000,  0x1000,
factory,    app,  factory, 0x10000, 0x290000,
spiffs,     data, spiffs,  0x2A0000,0x150000,
zb_storage, data, fat,     0x3F0000,0x4000,
zb_fct,     data, fat,     0x3F4000,0x1000,
coredump,   data, coredump,0x3F5000,0xB000,
```

Switching layouts wipes NVS — the device must be re-paired to the Zigbee coordinator afterward.

## Why these stack choices (background, do not re-litigate)

These were evaluated and rejected:

- **ESPHome:** does not support ESP32-C6 as a Zigbee sleepy end device. ESPHome assumes Wi-Fi for transport. Skipped.
- **Arduino IDE:** functional but no Claude Code support, no proper git ergonomics. Rejected.
- **ESP-IDF + esp-zigbee-sdk directly:** more powerful (light sleep with instant command response possible), but unnecessarily complex for a unidirectional once-per-hour sensor. Reserved for future migration only if the requirements change.
- **Official PlatformIO `espressif32` platform:** does NOT support Arduino framework on ESP32-C6. Use the pioarduino fork.

## Workflow

```bash
# Build
pio run

# Verbose build (use to debug toolchain config)
pio run -v

# Clean
pio run -t clean

# Flash (hold BOOT button on board while plugging USB-C, then run)
pio run -t upload

# Serial monitor
pio device monitor
```

The PlatformIO CLI is on PATH via `~/.zshrc`:
```bash
export PATH="$PATH:$HOME/.platformio/penv/bin"
```

### Flashing gotcha

Once the device enters its deep-sleep cycle, USB-CDC is unresponsive most of the time, so normal uploads will fail. **For every reflash:**

1. Unplug USB-C.
2. Hold the BOOT button on the FireBeetle.
3. While holding BOOT, plug USB-C back in.
4. Run `pio run -t upload`.
5. Release BOOT once esptool reports "Connected".

Setting `Erase All Flash Before Sketch Upload` (or equivalent flag) wipes Zigbee credentials — the device must be re-paired in HA after that.

## Validation commands

Before flashing after any platformio.ini change:

```bash
# Verify flash size is 4MB (both lines must show 4MB)
pio run -t clean && pio run -v 2>&1 | grep -- "--flash-size"

# Verify custom partitions are picked up (look for ARDUINO_PARTITION_partitions, not _default)
pio run -v 2>&1 | grep "ARDUINO_PARTITION_" | head -3
```

## Current status

- ✅ Toolchain validated: pioarduino + Arduino + Zigbee + ESP32-C6 building, flashing, and running on the DFR1075.
- ✅ Default Zigbee on/off light bulb example flashed successfully (619 KB firmware, 46% of 1.25 MB app slot used, 33 KB RAM).
- ⏳ Next: replace example with sleepy distance sensor implementation (use `Zigbee_Temp_Hum_Sensor_Sleepy.ino` as starting point; adapt for VL53L1X distance over I²C, 1-hour deep sleep cycle).
- ⏳ Then: pair with Home Assistant Zigbee coordinator, validate end-to-end measurement flow, then characterize battery life.

## References

- DFR1075 wiki: https://wiki.dfrobot.com/SKU_DFR1075_FireBeetle_2_Board_ESP32_C6
- pioarduino platform: https://github.com/pioarduino/platform-espressif32
- Arduino-ESP32 Zigbee docs: https://github.com/espressif/arduino-esp32/tree/master/libraries/Zigbee
- Sleepy sensor example: https://github.com/espressif/arduino-esp32/tree/master/libraries/Zigbee/examples/Zigbee_Temp_Hum_Sensor_Sleepy
- Working PlatformIO Zigbee+C6 reference: https://github.com/technoo10201/esp32-c6-zigbee-example-platformio
- Battery-powered C6 Zigbee sensor tutorial (Feb 2026): https://tutoduino.fr/en/tutorials/esp32c6-zigbee/
- Reference config repo (sigmdel): https://github.com/sigmdel/xiao_esp32c6_sketches
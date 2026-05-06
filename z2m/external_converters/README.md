# zigbee2mqtt external converter — czechit/water-tank-sensor

`czechit-water-tank-sensor.js` teaches zigbee2mqtt about the firmware in
this repo. Without it the device pairs but z2m treats it as generic:
values are exposed as `analog_input` with unit `°C`, and there is no
attribute reporting (values are `N/A` until you click the refresh icon).

With it installed:

- Two endpoints map to friendly properties: `distance` (cm) and `level` (%).
- `battery` (%) is exposed via the standard battery converter.
- On pairing, z2m binds `genAnalogInput` (EP 10 + EP 11) and `genPowerCfg`
  (EP 10) to the coordinator and configures attribute reporting at
  10–60 s for the analog channels and 60 s – 1 h for battery.

## Install

The path depends on your z2m installation:

| Install method                          | Converter directory                                   |
| --------------------------------------- | ----------------------------------------------------- |
| Home Assistant **Zigbee2MQTT add-on**   | `/config/zigbee2mqtt/external_converters/`            |
| Home Assistant **Z2M-Edge add-on**      | `/config/zigbee2mqtt-edge/external_converters/`       |
| Standalone (npm) z2m                    | `<your z2m data dir>/external_converters/`            |
| Docker (Koenkk image)                   | bind-mount over `/app/data/external_converters/`      |

On a Home Assistant install the directory is reachable through the
**File editor** or **Studio Code Server** add-ons (both visible in your
HA sidebar). If `external_converters/` doesn't exist yet, create it.

Steps:

1. Copy `czechit-water-tank-sensor.js` to that directory.
2. Open `/config/zigbee2mqtt/configuration.yaml`. On z2m **older than
   1.34**, add:
   ```yaml
   external_converters:
     - czechit-water-tank-sensor.js
   ```
   On z2m **1.34+** the converter is auto-loaded from the directory; no
   yaml change is needed. (Check your z2m version under Settings → About
   in the z2m UI.)
3. Restart the Zigbee2MQTT add-on.
4. In z2m UI → device card → click the red trash icon, tick **Force
   remove**, confirm. (This wipes z2m's cached interview record so the
   device is re-interviewed against the new converter.)
5. Make sure permit-join is on, then on the device hold BOOT for 3 s to
   factory-reset and rejoin. The device card should now show:
   - Manufacturer = `czechit`, Model = `water-tank-sensor`
   - Exposes: `distance` (cm), `level` (%), `battery` (%), `linkquality`
   - Values populate within ~60 s **without** clicking the refresh icon.

## Updating the converter

The converter lives in this repo so it is version-controlled. After
editing `czechit-water-tank-sensor.js` here, copy it back to the z2m
directory and restart z2m. You do not need to re-pair the device for
converter-internal changes (e.g. tweaks to `fromZigbee` mapping or
exposes); you do need to re-pair if you change `configure()` because
that function only runs on (re-)interview.

## References

- z2m external converter docs: https://www.zigbee2mqtt.io/advanced/more/how_to_support_new_devices.html
- zigbee-herdsman-converters API: https://github.com/Koenkk/zigbee-herdsman-converters

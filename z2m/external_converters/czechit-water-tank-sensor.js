// zigbee2mqtt external converter for the czechit water-tank-sensor.
//
// Matches a custom Espressif ESP32-C6 device (firmware in this repo) that
// reports:
//   - Endpoint 10: Analog Input cluster, presentValue = distance to water
//                  surface in cm. Also hosts Power Configuration with
//                  batteryPercentageRemaining.
//   - Endpoint 11: Analog Input cluster, presentValue = tank fill level
//                  in percent (0..100).
//
// Format note: this is the **z2m 2.x modernExtend converter format**.
// Each `m.numeric(...)` /  `m.battery()` block in the `extend` array
// auto-generates a `configure()` step that sends the right ZDO Bind
// requests + ZCL ConfigureReporting commands during pairing — z2m
// invokes those automatically on every interview, with no `meta.configured`
// hash dance to skip them. The legacy `module.exports = {fromZigbee,
// toZigbee, configure}` form we shipped previously was loaded by z2m
// 2.8.0 (UI showed "Supported: external") but its `configure` was not
// being called on re-pairs, leaving the bind table empty. This format
// fixes that.
//
// Install: copy this file into your z2m external_converters directory
// (typically /share/zigbee2mqtt/external_converters/ on Home Assistant
// installs — check your z2m data path) and restart z2m. See README in
// this repo for full steps.

const m = require('zigbee-herdsman-converters/lib/modernExtend');

const definition = {
    zigbeeModel: ['water-tank-sensor'],
    model: 'water-tank-sensor',
    vendor: 'czechit',
    description: 'DIY water-tank distance sensor (ESP32-C6)',

    extend: [
        // Map endpoint numbers to short names used by m.numeric() below.
        // Without this, z2m's herdsman-converters can't disambiguate
        // genAnalogInput on EP10 vs EP11.
        m.deviceEndpoints({endpoints: {'10': 10, '11': 11}}),

        // Standard battery handling: binds genPowerCfg to the
        // coordinator and configures reporting on
        // batteryPercentageRemaining. z2m's built-in path; expects the
        // attribute on the same endpoint as the power source — EP10
        // for us, which is also the device's "primary" endpoint.
        m.battery(),

        // EP10 / genAnalogInput.presentValue → exposed as `distance`.
        // The reporting block tells z2m to ConfigureReporting on
        // pairing: min 10 s, max 60 s, change 1.0 cm.
        m.numeric({
            name: 'distance',
            cluster: 'genAnalogInput',
            attribute: 'presentValue',
            description: 'Distance from sensor to water surface',
            unit: 'cm',
            endpointNames: ['10'],
            access: 'STATE_GET',
            reporting: {min: 10, max: 60, change: 1},
        }),

        // EP11 / genAnalogInput.presentValue → exposed as `level`.
        m.numeric({
            name: 'level',
            cluster: 'genAnalogInput',
            attribute: 'presentValue',
            description: 'Tank fill level (0–100 %)',
            unit: '%',
            endpointNames: ['11'],
            access: 'STATE_GET',
            reporting: {min: 10, max: 60, change: 1},
        }),
    ],
};

module.exports = definition;

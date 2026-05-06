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
// Without this converter z2m treats the device as generic, exposes the
// values with unit `°C` and labels `analog_input`, and does not configure
// attribute reporting. With this converter installed the device pairs
// with friendly names and reports flow automatically.
//
// Install: copy this file into your z2m external_converters directory
// (typically /config/zigbee2mqtt/external_converters/ on Home Assistant
// installs) and restart z2m. See README in this repo for full steps.

const fz       = require('zigbee-herdsman-converters/converters/fromZigbee');
const exposes  = require('zigbee-herdsman-converters/lib/exposes');
const reporting = require('zigbee-herdsman-converters/lib/reporting');
const e  = exposes.presets;
const ea = exposes.access;

const fzLocal = {
    // Maps the generic genAnalogInput.presentValue to either `distance`
    // (endpoint 10) or `level` (endpoint 11) so each value gets its own
    // top-level property in MQTT instead of colliding under `analog_input`.
    distance_or_level: {
        cluster: 'genAnalogInput',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.data.presentValue === undefined) return undefined;
            const v = parseFloat(msg.data.presentValue);
            if (msg.endpoint.ID === 10) {
                return {distance: Number(v.toFixed(1))};
            }
            if (msg.endpoint.ID === 11) {
                return {level: Number(v.toFixed(0))};
            }
            return undefined;
        },
    },
};

module.exports = {
    zigbeeModel: ['water-tank-sensor'],
    model: 'water-tank-sensor',
    vendor: 'czechit',
    description: 'DIY water-tank distance sensor (ESP32-C6)',

    fromZigbee: [fz.battery, fzLocal.distance_or_level],
    toZigbee: [],

    exposes: [
        e.battery(),
        exposes
            .numeric('distance', ea.STATE)
            .withUnit('cm')
            .withDescription('Distance from sensor to water surface'),
        exposes
            .numeric('level', ea.STATE)
            .withUnit('%')
            .withDescription('Tank fill level (0–100 %)'),
    ],

    configure: async (device, coordinatorEndpoint, logger) => {
        const ep10 = device.getEndpoint(10);
        const ep11 = device.getEndpoint(11);

        // Bind the coordinator to the clusters we want unsolicited reports
        // from. Without these binds the device has no destination to push
        // reports to and z2m sees N/A until each manual read.
        await reporting.bind(ep10, coordinatorEndpoint, ['genPowerCfg', 'genAnalogInput']);
        await reporting.bind(ep11, coordinatorEndpoint, ['genAnalogInput']);

        // Battery: every 1–60 minutes, or any 1% change.
        await reporting.batteryPercentageRemaining(ep10, {min: 60, max: 3600, change: 1});

        // Distance and level both report on the same cadence the firmware
        // is configured for: 10–60 s, 1.0 unit change.
        const analogCfg = [{
            attribute: 'presentValue',
            minimumReportInterval: 10,
            maximumReportInterval: 60,
            reportableChange: 1,
        }];
        await ep10.configureReporting('genAnalogInput', analogCfg);
        await ep11.configureReporting('genAnalogInput', analogCfg);
    },
};

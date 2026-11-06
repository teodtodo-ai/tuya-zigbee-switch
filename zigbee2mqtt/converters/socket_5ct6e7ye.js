const {
    numeric,
    enumLookup,
    deviceEndpoints,
    onOff,
    text,
    binary,
    windowCovering,
    light,
    iasZoneAlarm,
    deviceAddCustomCluster,
} = require("zigbee-herdsman-converters/lib/modernExtend");
const {assertString} = require("zigbee-herdsman-converters/lib/utils");
const e = require("zigbee-herdsman-converters/lib/exposes").presets;
const reporting = require("zigbee-herdsman-converters/lib/reporting");
const constants = require("zigbee-herdsman-converters/lib/constants");
const Zcl = require('zigbee-herdsman').Zcl;

/********************************************************************
  This file (`switch_custom.js`) is generated. 
  
  You can edit it for testing, but for PRs please use:
  - `device_db.yaml`                - add or edit devices
  - `switch_custom.md.jinja`        - update the template
  - `make_z2m_custom_converters.py` - update generation script

  Generate with: `make tools/update_converters`
********************************************************************/

const romasku = {
    switchAction: (name, endpointName) =>
        enumLookup({
            name,
            endpointName,
            lookup: { on_off: 0, off_on: 1, toggle_simple: 2, toggle_smart_sync: 3, toggle_smart_opposite: 4 },
            cluster: "genOnOffSwitchCfg",
            attribute: {ID: 0x0010, type: 0x30, required: true, write: true, min: 0, max: 4}, // Enum8
            description: `Select how switch should work:
            - on_off: When switch physically moved to position 1 it always generates ON command, and when moved to position 2 it generates OFF command
            - off_on: Same as on_off, but positions are swapped
            - toggle_simple: Any press of physical switch will TOGGLE the relay and send TOGGLE command to binds
            - toggle_smart_sync: Any press of physical switch will TOGGLE the relay and send corresponding ON/OFF command to keep binds in sync with relay
            - toggle_smart_opposite: Any press of physical switch: TOGGLE the relay and send corresponding ON/OFF command to keep binds in the state opposite to the relay`,
            entityCategory: "config",
        }),
    switchMode: (name, endpointName) =>
        enumLookup({
            name,
            endpointName,
            lookup: { toggle: 0, momentary: 1, momentary_nc: 2 },
            cluster: "genOnOffSwitchCfg",
            attribute: { ID: 0xff00, type: 0x30 }, // Enum8
            description: "Select the type of switch connected to the device",
            entityCategory: "config",
        }),
    relayMode: (name, endpointName) =>
        enumLookup({
            name,
            endpointName,
            lookup: { detached: 0, press_start: 1, short_press: 3, long_press: 2},
            cluster: "genOnOffSwitchCfg",
            attribute: { ID: 0xff01, type: 0x30 }, // Enum8
            description: "When to turn on/off internal relay",
            entityCategory: "config",
        }),
    relayIndex: (name, endpointName, relay_cnt, light_cnt) =>
        enumLookup({
            name,
            endpointName,
            lookup: Object.fromEntries([
                // The device's outputs in the order the firmware numbers them:
                // relays first, then lights. Lights count from 0 to match their
                // endpoint names, relays from 1 as they always have.
                ...Array.from({ length: relay_cnt }, (_, i) => [`relay_${i + 1}`, i + 1]),
                ...Array.from({ length: light_cnt }, (_, i) => [`light_${i}`, relay_cnt + i + 1]),
                // 0xFF drives every output at once. A mixed set is resolved as
                // a group rather than per output: anything on means everything
                // goes off, otherwise everything goes on - what a master button
                // is expected to do.
                ["all", 255],
            ]),
            cluster: "genOnOffSwitchCfg",
            attribute: { ID: 0xff02, type: 0x20 }, // uint8
            description: "Which internal output it should trigger ('all' switches every output together)",
            entityCategory: "config",
        }),
    bindedMode: (name, endpointName) =>
        enumLookup({
            name,
            endpointName,
            lookup: { press_start: 1, short_press: 3, long_press: 2},
            cluster: "genOnOffSwitchCfg",
            attribute: { ID: 0xff05, type: 0x30 }, // Enum8
            description: "When turn on/off binded device",
            entityCategory: "config",
        }),
    multiPressReset: (name, endpointName) =>
        binary({
            name,
            endpointName,
            valueOn: ["ON", 1],
            valueOff: ["OFF", 0],
            cluster: "genOnOffSwitchCfg",
            attribute: { ID: 0xff06, type: 0x10 }, // Boolean
            description: "Whether repeatedly triggering this input resets the device to factory defaults. Turn off for an input that is not operated by hand - a reed contact reaches the press count on its own",
            access: "ALL",
            entityCategory: "config",
        }),
    switchFlash: (name, endpointName) =>
        binary({
            name,
            endpointName,
            valueOn: ["ON", 1],
            valueOff: ["OFF", 0],
            cluster: "genOnOffSwitchCfg",
            attribute: { ID: 0xff07, type: 0x10 }, // Boolean
            description: "Briefly light the indicator LED to confirm a press",
            access: "ALL",
            entityCategory: "config",
        }),
    switchFlashBrightness: (name, endpointName) =>
        numeric({
            name,
            endpointNames: [endpointName],
            cluster: "genOnOffSwitchCfg",
            attribute: { ID: 0xff08, type: 0x20 }, // uint8
            description: "How bright the confirmation flash is (0-255, e.g. 128 = 50%)",
            valueMin: 0,
            valueMax: 255,
            entityCategory: "config",
        }),
    longPressDuration: (name, endpointName) =>
        numeric({
            name,
            endpointNames: [endpointName],
            cluster: "genOnOffSwitchCfg",
            attribute: { ID: 0xff03, type: 0x21 }, // uint16
            description: "What duration is considerd to be long press",
            valueMin: 0,
            valueMax: 5000,
            entityCategory: "config",
        }),
    levelMoveRate: (name, endpointName) =>
        numeric({
            name,
            endpointNames: [endpointName],
            cluster: "genOnOffSwitchCfg",
            attribute: { ID: 0xff04, type: 0x20 }, // uint8
            description: "Level (dim) move rate in steps per ms",
            valueMin: 1,
            valueMax: 255,
            entityCategory: "config",
        }),
    pressAction: (name, endpointName) =>
        enumLookup({
            name,
            endpointName,
            access: "STATE_GET",
            lookup: { released: 0, press: 1, long_press: 2, position_on: 3, position_off: 4 },
            cluster: "genMultistateInput",
            attribute: "presentValue",
            description: "Action of the switch: 'released' or 'press' or 'long_press'",
            entityCategory: "diagnostic",
        }),
    // Publishes the physical button activity as a Z2M `action`, which Home
    // Assistant turns into an *event* entity (the same way the Hue remotes
    // work). The `*_press_action` sensors above stay untouched, so existing
    // automations keep working and both representations arrive in HA.
    //
    // Two independent sources feed the same action, on purpose: whichever
    // path survives the mesh wins, and neither needs the other.
    //  - genMultistateInput reports (bound in `configure`): what the button
    //    did. The firmware already picks the value by switch mode, so a toggle
    //    switch emits position_on/position_off while a momentary one emits
    //    press/long_press/release - the action follows automatically.
    //  - the commands the switch sends to its bindings: genOnOff, genLevelCtrl
    //    (dimming) and closuresWindowCovering (covers). These arrive because
    //    `configure` binds the coordinator alongside the user's own bindings,
    //    so the action says what the bound devices were actually told to do.
    //
    // Endpoint names differ per gang count (switch_left, switch_right, ...),
    // so the action prefix is deliberately positional and stable instead:
    // switch_0, switch_1, ... and cover_switch_0, cover_switch_1, ...
    actionEvent: ({switches = [], longSwitches = [], coverSwitches = []}) => {
        const switchStates = {
            0: "release", 1: "press", 2: "long_press",
            3: "position_on", 4: "position_off",
        };
        const coverStates = {
            0: "release", 1: "open", 2: "close",
            3: "stop", 4: "long_open", 5: "long_close",
        };
        const onOffCommands = {commandOn: "on", commandOff: "off", commandToggle: "toggle"};
        const coverCommands = {
            commandUpOpen: "cover_open",
            commandDownClose: "cover_close",
            commandStop: "cover_stop",
        };
        const levelSuffixes = ["brightness_move_up", "brightness_move_down", "brightness_stop"];

        // Each button also gets an `action` of its own, tied to its endpoint, so
        // Home Assistant creates one event entity per button instead of a single
        // device-wide one whose type carries the button in its name. The
        // combined `action` stays exactly as it was - automations built on it
        // keep working, and it remains the only place a 2EP long press and its
        // parent button can be told apart by name alone.
        const byEndpoint = (list) => Object.fromEntries(list.map((s) => [s.endpoint, s.prefix]));
        const namesByEndpoint = Object.fromEntries(
            [...switches, ...longSwitches, ...coverSwitches]
                .filter((s) => s.name)
                .map((s) => [s.endpoint, s.name]),
        );
        // A 2EP companion endpoint has no name of its own in deviceEndpoints; its
        // events land on the parent button's entity, marked `long_`.
        const suffixPrefixByEndpoint = Object.fromEntries(
            [...switches, ...longSwitches, ...coverSwitches].map(
                (s) => [s.endpoint, s.suffixPrefix || ""],
            ),
        );
        const multistatePrefixes = byEndpoint([...switches, ...coverSwitches]);
        const multistateStates = Object.fromEntries([
            ...switches.map((s) => [s.endpoint, switchStates]),
            ...coverSwitches.map((s) => [s.endpoint, coverStates]),
        ]);
        // A long-press companion endpoint (2EP) only ever toggles its own
        // bindings, so it has no multistate of its own - just the command.
        const onOffPrefixes = byEndpoint([...switches, ...longSwitches]);
        const levelPrefixes = byEndpoint(switches);
        const coverPrefixes = byEndpoint(coverSwitches);

        const actions = [];
        // Per endpoint name, the event types that button alone can produce.
        const perButton = {};
        const add = (s, suffixes) => {
            const suffixPrefix = s.suffixPrefix || "";
            actions.push(...suffixes.map((x) => `${s.prefix}_${x}`));
            if (!s.name) return;
            perButton[s.name] = perButton[s.name] || [];
            perButton[s.name].push(...suffixes.map((x) => `${suffixPrefix}${x}`));
        };
        for (const s of switches) {
            add(s, Object.values(switchStates));
            add(s, Object.values(onOffCommands));
            add(s, levelSuffixes);
        }
        for (const s of longSwitches) {
            add(s, Object.values(onOffCommands));
        }
        for (const s of coverSwitches) {
            add(s, Object.values(coverStates));
            add(s, Object.values(coverCommands));
        }

        const lookupAction = (prefixes, msg, suffix) => {
            const prefix = prefixes[msg.endpoint.ID];
            if (prefix === undefined || suffix === undefined) return;
            const result = {action: `${prefix}_${suffix}`};
            const name = namesByEndpoint[msg.endpoint.ID];
            if (name !== undefined) {
                const suffixPrefix = suffixPrefixByEndpoint[msg.endpoint.ID] || "";
                result[`action_${name}`] = `${suffixPrefix}${suffix}`;
            }
            return result;
        };

        return {
            isModernExtend: true,
            exposes: [
                e.action(actions),
                ...Object.entries(perButton).map(
                    ([name, values]) => e.action([...new Set(values)]).withEndpoint(name),
                ),
            ],
            fromZigbee: [
                {
                    cluster: "genMultistateInput",
                    type: ["attributeReport", "readResponse"],
                    convert: (model, msg) => {
                        const states = multistateStates[msg.endpoint.ID];
                        if (states === undefined) return;
                        return lookupAction(multistatePrefixes, msg, states[msg.data.presentValue]);
                    },
                },
                {
                    cluster: "genOnOff",
                    type: Object.keys(onOffCommands),
                    convert: (model, msg) => lookupAction(onOffPrefixes, msg, onOffCommands[msg.type]),
                },
                {
                    cluster: "genLevelCtrl",
                    type: ["commandMove", "commandMoveWithOnOff", "commandStop", "commandStopWithOnOff"],
                    convert: (model, msg) => {
                        const stop = msg.type === "commandStop" || msg.type === "commandStopWithOnOff";
                        // movemode 0 = up, 1 = down (the firmware alternates it
                        // so each long press dims the other way).
                        const suffix = stop
                            ? "brightness_stop"
                            : `brightness_move_${msg.data.movemode === 0 ? "up" : "down"}`;
                        return lookupAction(levelPrefixes, msg, suffix);
                    },
                },
                {
                    cluster: "closuresWindowCovering",
                    type: Object.keys(coverCommands),
                    convert: (model, msg) => lookupAction(coverPrefixes, msg, coverCommands[msg.type]),
                },
            ],
        };
    },
    // "Restore the previous colour temperature" is 0xFFFF on the wire, but
    // zigbee-herdsman 10.6.1 caps startUpColorTemperature at 0xFEFF and never
    // consults the sentinel it defines for exactly this: a write of 65535 is
    // refused with INVALID_VALUE before it leaves the coordinator, and a read
    // of 65535 comes back as NaN. Both are fixed upstream, but only later.
    //
    // 0 is sent instead. The firmware reads it as "previous" as well - 0 mireds
    // is not a colour any light can show, so nothing else can mean it - and it
    // passes the check. The published state stays 65535 so the UI keeps showing
    // its "previous" preset as the selected one.
    //
    // Split in two because converter order decides who wins: the *first*
    // matching toZigbee is used, so the write has to come before light(), while
    // fromZigbee results are merged in order, so the read has to come after it.
    colorTempStartupPreviousWrite: () => ({
        isModernExtend: true,
        toZigbee: [{
            key: ["color_temp_startup"],
            convertSet: async (entity, key, value, meta) => {
                const previous = value === "previous" || Number(value) === 65535;
                await entity.write("lightingColorCtrl", {
                    startUpColorTemperature: previous ? 0 : Number(value),
                });
                return {state: {color_temp_startup: previous ? 65535 : Number(value)}};
            },
            convertGet: async (entity) => {
                await entity.read("lightingColorCtrl", ["startUpColorTemperature"]);
            },
        }],
    }),
    colorTempStartupPreviousRead: (endpointNamesById) => ({
        isModernExtend: true,
        fromZigbee: [{
            cluster: "lightingColorCtrl",
            type: ["attributeReport", "readResponse"],
            convert: (model, msg) => {
                if (msg.data.startUpColorTemperature !== 0) return;
                const name = endpointNamesById[msg.endpoint.ID];
                if (name === undefined) return;
                return {[`color_temp_startup_${name}`]: 65535};
            },
        }],
    }),
    // One fade time per light rather than per channel: a tunable white has to
    // fade cold and warm together, and an RGB light must not end up with three
    // separate transitions.
    lightTransition: (name, endpointName) =>
        numeric({
            name,
            endpointNames: [endpointName],
            cluster: "genLevelCtrl",
            attribute: { ID: 0xff00, type: 0x21 }, // uint16
            description: "Fade time for on/off, brightness and colour changes",
            valueMin: 0,
            valueMax: 65535,
            unit: "ms",
            entityCategory: "config",
        }),
    relayIndicatorMode: (name, endpointName) =>
        enumLookup({
            name,
            endpointName,
            lookup: { same: 0, opposite: 1, manual: 2 },
            cluster: "genOnOff",
            attribute: { ID: 0xff01, type: 0x30 }, // Enum8
            description: "Mode for the relay indicator LED",
            entityCategory: "config",
        }),
    relayIndicator: (name, endpointName) =>
        binary({
            name,
            endpointName,
            valueOn: ["ON", 1],
            valueOff: ["OFF", 0],
            cluster: "genOnOff",
            attribute: {ID: 0xff02, type: 0x10},  // Boolean
            description: "State of the relay indicator LED",
            access: "ALL",
            entityCategory: "config",
        }),
    ledBrightness: (name, endpointName) =>
        numeric({
            name,
            endpointNames: [endpointName],
            cluster: "genOnOff",
            attribute: { ID: 0xff03, type: 0x20 }, // uint8
            description: "Indicator LED brightness when on (0-255, e.g. 128 = 50%)",
            valueMin: 0,
            valueMax: 255,
            entityCategory: "config",
        }),
    ledTransition: (name, endpointName) =>
        numeric({
            name,
            endpointNames: [endpointName],
            cluster: "genOnOff",
            attribute: { ID: 0xff04, type: 0x21 }, // uint16
            description: "Indicator LED fade time in milliseconds (0 = instant)",
            valueMin: 0,
            valueMax: 65535,
            unit: "ms",
            entityCategory: "config",
        }),
    batteryPercentage: () => {
        const result = numeric({
            name: "battery",
            cluster: "genPowerCfg",
            attribute: "batteryPercentageRemaining",
            description: "Remaining battery in %",
            valueMin: 0,
            valueMax: 100,
            unit: "%",
            access: "STATE_GET",
            entityCategory: "diagnostic",
        });
        // Patch fromZigbee to convert ZCL 0-200 to 0-100%
        const origConvert = result.fromZigbee[0].convert;
        result.fromZigbee[0].convert = (model, msg, publish, options, meta) => {
            const r = origConvert(model, msg, publish, options, meta);
            if (r && r.battery !== undefined) {
                r.battery = Math.round(r.battery / 2);
            }
            return r;
        };
        return result;
    },
    // numeric() that divides the raw firmware value by `divisor` for display
    // (firmware sends integers; we patch fromZigbee so scaling always applies).
    // `precision` sets the default number of decimals; Z2M still lets the user
    // override it (0-3) via the "<name> precision" device setting.
    scaledMeasurement: ({name, cluster, attribute, unit, divisor, precision, endpointName}) => {
        const result = numeric({
            name,
            cluster,
            attribute,
            unit,
            precision,
            access: "STATE",
            endpointName,
        });
        const origConvert = result.fromZigbee[0].convert;
        result.fromZigbee[0].convert = (model, msg, publish, options, meta) => {
            const r = origConvert(model, msg, publish, options, meta);
            if (r && r[name] !== undefined && r[name] !== null) {
                r[name] = r[name] / divisor;
            }
            return r;
        };
        return result;
    },
    networkIndicator: (name, endpointName) =>
        binary({
            name,
            endpointName,
            valueOn: ["ON", 1],
            valueOff: ["OFF", 0],
            cluster: "genBasic",
            attribute: {ID: 0xff01, type: 0x10},  // Boolean
            description: "State of the network indicator LED",
            access: "ALL",
            entityCategory: "config",
        }),
    networkLedBrightness: (name, endpointName) =>
        numeric({
            name,
            endpointNames: [endpointName],
            cluster: "genBasic",
            attribute: { ID: 0xff05, type: 0x20 }, // uint8
            description: "Network/status LED brightness when on (0-255, e.g. 128 = 50%)",
            valueMin: 0,
            valueMax: 255,
            entityCategory: "config",
        }),
    networkLedTransition: (name, endpointName) =>
        numeric({
            name,
            endpointNames: [endpointName],
            cluster: "genBasic",
            attribute: { ID: 0xff06, type: 0x21 }, // uint16
            description: "Network/status LED fade time in milliseconds (0 = instant)",
            valueMin: 0,
            valueMax: 65535,
            unit: "ms",
            entityCategory: "config",
        }),
    // On-device calibration trigger: the user enters the real measured value
    // (in display units, e.g. Volts), we scale it to the firmware's raw integer
    // unit (multiplier) and write it. The firmware then derives and persists the
    // multiplier so the channel reads that value, and clears the field.
    calibrationValues: (name, endpointName) =>
        text({
            name,
            endpointName,
            access: "ALL",
            cluster: "haElectricalMeasurement",
            attribute: {ID: 0xFF20, type: 0x42}, // char str
            description: "Active calibration multipliers as V<v>A<a>W<w>. Read it from a calibrated device and write it to others of the same type to copy the calibration (a missing or 0 channel keeps its current value)",
            entityCategory: "config",
            validate: (value) => {
                assertString(value);
                if (!/^([VAW]\d{1,10}){0,3}$/.test(value)) {
                    throw new Error("Expected format like V154672A118646W13939 (V/A/W each followed by digits)");
                }
            },
        }),
    overloadSetting: ({name, attribute, unit, scale, valueMin, valueMax, valueStep, description, endpointName}) => {
        const result = numeric({
            name,
            cluster: "haElectricalMeasurement",
            attribute,
            unit,
            description,
            access: "ALL",
            valueMin,
            valueMax,
            valueStep,
            entityCategory: "config",
            endpointName,
        });
        // Firmware stores current in mA and voltage in cV; present A/V and scale.
        if (scale && scale !== 1) {
            const origSet = result.toZigbee[0].convertSet;
            result.toZigbee[0].convertSet = async (entity, key, value, meta) =>
                await origSet(entity, key, Math.round(Number(value) * scale), meta);
            const origConvert = result.fromZigbee[0].convert;
            result.fromZigbee[0].convert = (model, msg, publish, options, meta) => {
                const r = origConvert(model, msg, publish, options, meta);
                if (r && r[name] !== undefined && r[name] !== null) {
                    r[name] = r[name] / scale;
                }
                return r;
            };
        }
        return result;
    },
    overloadAlarm: (name, endpointName) =>
        enumLookup({
            name,
            cluster: "haElectricalMeasurement",
            attribute: {ID: 0xFF36, type: 0x30}, // enum8
            lookup: {none: 0, power: 1, current: 2, peak: 3, voltage_high: 4, voltage_low: 5, locked_out: 6},
            description: "Overload protection status: what tripped the relay or is being warned about (locked_out = tripped too many times, switch on manually to re-arm)",
            // Readable, not only reported: a device that tripped while Z2M was
            // down would otherwise keep showing the state from before.
            access: "STATE_GET",
            entityCategory: "diagnostic",
            endpointName,
        }),
    calibrate: ({name, attribute, unit, multiplier, valueMax, valueStep, description, endpointName}) => {
        const result = numeric({
            name,
            cluster: "haElectricalMeasurement",
            attribute,
            unit,
            description,
            access: "ALL",
            valueMin: 0,
            valueMax,
            valueStep,
            entityCategory: "config",
            endpointName,
        });
        const origConvertSet = result.toZigbee[0].convertSet;
        result.toZigbee[0].convertSet = async (entity, key, value, meta) => {
            const raw = Math.round(Number(value) * multiplier);
            return await origConvertSet(entity, key, raw, meta);
        };
        return result;
    },
    multiPressResetCount: (name, endpointName) =>
        numeric({
            name,
            endpointNames: [endpointName],
            cluster: "genBasic",
            attribute: { ID: 0xff02, type: 0x20 }, // uint8
            description: "Number of consecutive presses to trigger factory reset (0 = disabled)",
            valueMin: 0,
            valueMax: 255,
            entityCategory: "config",
        }),
    deviceConfig: (name, endpointName) =>
        text({
            name,
            endpointName,
            access: "ALL",
            cluster: "genBasic",
            attribute:  { ID: 0xff00, type: 0x44 }, // long str
            description: "Current configuration of the device",
            zigbeeCommandOptions: {timeout: 30_000},
            validate: (value) => {
                assertString(value);
                
                const validatePin = (pin) => {
                    const validPins = [
                        "A0", "A1", "A2", "A3", "A4", "A5", "A6","A7",
                        "B0", "B1", "B2", "B3", "B4", "B5", "B6","B7",
                        "C0", "C1", "C2", "C3", "C4", "C5", "C6","C7",
                        "D0", "D1", "D2", "D3", "D4", "D5", "D6","D7",
                    ];
                    if (!validPins.includes(pin)) throw new Error(`Pin ${pin} is invalid`);
                }

                if (value.length > 256) throw new Error('Length of config is greater than 256');
                if (!value.endsWith(';')) throw new Error('Should end with ;');
                const parts = value.slice(0, -1).split(';');  // Drop last ;
                if (parts.length < 2) throw new Error("Model and/or manufacturer missing");

                // The firmware holds its peripherals in fixed-size tables. Asking
                // for more than fits used to overflow them and corrupt the
                // device's cluster tables for good - it stayed on the network but
                // answered every read with UNSUPPORTED_ATTRIBUTE, so not even the
                // config could be written back, leaving only a re-flash by wire.
                // Newer firmware resets itself to defaults instead, but the write
                // still costs a rejoin, so refuse it here.
                const capacity = {
                    S: [4, 'switches'], R: [6, 'relays'],
                    X: [3, 'cover switches'], C: [3, 'covers'],
                    W: [5, 'lights'], T: [5, 'lights'],
                };
                const counts = {};
                for (const part of parts.slice(2)) {
                    // Only the bare single-letter tokens declare peripherals;
                    // BT/SLP and friends are options that happen to share a letter.
                    if (part === 'SLP' || part.startsWith('BT')) continue;
                    if (capacity[part[0]]) counts[part[0]] = (counts[part[0]] || 0) + 1;
                }
                for (const [token, [max, name]] of Object.entries(capacity)) {
                    if ((counts[token] || 0) > max) {
                        throw new Error(`Config declares ${counts[token]} ${name}, the firmware supports at most ${max}`);
                    }
                }
                // Switches, relays, cover switches and covers each take one
                // endpoint, plus one more per switch when 2EP is set.
                if ((counts.W || 0) + (counts.T || 0) > 5) {
                    throw new Error(`Config declares ${(counts.W || 0) + (counts.T || 0)} lights, the firmware supports at most 5`);
                }
                const endpoints = (counts.S || 0) + (counts.R || 0) + (counts.X || 0) +
                    (counts.C || 0) + (counts.W || 0) + (counts.T || 0) +
                    (parts.includes('2EP') ? (counts.S || 0) : 0);
                if (endpoints > 12) {
                    throw new Error(`Config needs ${endpoints} endpoints, the firmware supports at most 12`);
                }
                for (const part of parts.slice(2)) {
                    if (part == 'SLP') {
                        continue;
                    } else if (part == '2EP') {
                        continue;
                    } else if (part.startsWith('OL')) {
                        // Overload limits: OL[C<soft_mA>][P<peak_mA>]
                        if (!/^OL(C\d+)?(P\d+)?$/.test(part)) {
                            throw new Error(`Overload option ${part} is invalid. Use OLC<soft_mA>P<peak_mA>, e.g. OLC16000P20000`);
                        }
                    } else if (part[0] == 'D') {
                        if (!/^D\d+$/.test(part)) {
                            throw new Error(`Debounce option ${part} is invalid. Use D<N>, e.g. D100 or D0`);
                        }
                    } else if (part.startsWith('BT')) {
                        validatePin(part.slice(2,4));
                    } else if (part[0] == 'B' || part[0] == 'S') {
                        validatePin(part.slice(1,3));
                        if (!["u", "U", "d", "f"].includes(part[3])) {
                            throw new Error(`Pull up down ${part[3]} is invalid. Valid options are u, U, d, f`);
                        } 
                    } else if (part[0] == 'X') {
                        validatePin(part.slice(1,3));
                        validatePin(part.slice(3,5));
                        if (!["u", "U", "d", "f"].includes(part[5])) {
                            throw new Error(`Pull up down ${part[5]} is invalid. Valid options are u, U, d, f`);
                        }
                    } else if (part[0] == 'C') {
                        validatePin(part.slice(1,3));
                        validatePin(part.slice(3,5));
                    } else if (part[0] == 'L' || part[0] == 'R' || part[0] == 'I') {
                        validatePin(part.slice(1,3));
                    } else if(part[0] == 'M') {
                        ;
                    } else if(part[0] == 'i') {
                        ; // TODO: write validation
                    } else if(part.startsWith('EP')) {
                        validatePin(part.slice(2,4));
                        validatePin(part.slice(4,6));
                        validatePin(part.slice(6,8));
                    } else if (part[0] == 'W') {
                        // W<pin> - one dimmable channel
                        validatePin(part.slice(1,3));
                    } else if (part[0] == 'T') {
                        // T<cold><warm> - tunable white
                        validatePin(part.slice(1,3));
                        validatePin(part.slice(3,5));
                    } else if (part[0] == 'Y') {
                        // Y<r><g><b> - three-colour status LED
                        validatePin(part.slice(1,3));
                        validatePin(part.slice(3,5));
                        validatePin(part.slice(5,7));
                    } else if(part.startsWith('EB')) {
                        // BL0942 UART metering: EB<TX><RX>[S<baud>][V..][A..][W..]
                        validatePin(part.slice(2,4));
                        validatePin(part.slice(4,6));
                    } else {
                        throw new Error(`Invalid entry ${part}. Should start with one of B, BT, C, D, EB, EP, I, L, M, OL, R, S, SLP, T, W, X, Y, i, 2EP`);
                    }
                }
            },
            entityCategory: "config",
        }),
    coverSwitchPressAction: (name, endpointName) =>
        enumLookup({
            name,
            endpointName,
            access: "STATE_GET",
            lookup: { 
                released: 0, 
                open: 1, 
                close: 2,
                stop: 3,
                long_open: 4,
                long_close: 5
            },
            cluster: "genMultistateInput",
            attribute: "presentValue",
            description: "Cover switch button press action",
            entityCategory: "diagnostic"
        }),
    coverSwitchType: (name, endpointName) =>
        enumLookup({
            name,
            endpointName,
            lookup: { toggle: 0, momentary: 1 },
            cluster: "manuSpecificTuyaCoverSwitchConfig",
            attribute: "switchType",
            description: "Type of cover switch: toggle (rocker) or momentary (push button)",
            entityCategory: "config",
        }),
    coverSwitchCoverIndex: (name, endpointName, output_cnt) =>
        enumLookup({
            name,
            endpointName,
            lookup: Object.fromEntries([
                ['detached', 0],
                ...Array.from({ length: output_cnt || 2 }, (_, i) => [`cover_${i + 1}`, i + 1])
            ]),
            cluster: "manuSpecificTuyaCoverSwitchConfig",
            attribute: "coverIndex",
            description: "Which cover to control locally (detached = no local control)",
            entityCategory: "config",
        }),
    coverSwitchInvert: (name, endpointName) =>
        binary({
            name,
            endpointName,
            valueOn: ["ON", 1],
            valueOff: ["OFF", 0],
            cluster: "manuSpecificTuyaCoverSwitchConfig",
            attribute: "reversal",
            description: "Inverts UP/DOWN direction for inputs",
            access: "ALL",
            entityCategory: "config",
        }),
    coverSwitchLocalMode: (name, endpointName) =>
        enumLookup({
            name,
            endpointName,
            lookup: { immediate: 0, short_press: 1, long_press: 2, hybrid: 3 },
            cluster: "manuSpecificTuyaCoverSwitchConfig",
            attribute: "localMode",
            description: "When to trigger local cover: immediate (start/stop on press), short_press (trigger on release), long_press (trigger after long press duration), hybrid (trigger on release or continuous movement while held). Only affects momentary switches",
            entityCategory: "config",
        }),
    coverSwitchBindedMode: (name, endpointName) =>
        enumLookup({
            name,
            endpointName,
            lookup: { immediate: 0, short_press: 1, long_press: 2, hybrid: 3 },
            cluster: "manuSpecificTuyaCoverSwitchConfig",
            attribute: "bindedMode",
            description: "When to send commands to bound devices: immediate (start/stop on press), short_press (trigger on release), long_press (trigger after long press duration), hybrid (trigger on release or continuous movement while held). Only affects momentary switches",
            entityCategory: "config",
        }),
    coverSwitchLongPressDuration: (name, endpointName) =>
        numeric({
            name,
            endpointNames: [endpointName],
            cluster: "manuSpecificTuyaCoverSwitchConfig",
            attribute: "longPressDuration",
            description: "Threshold in milliseconds to distinguish short press from long press",
            valueMin: 0,
            valueMax: 5000,
            entityCategory: "config",
        }),
    coverMoving: (name, endpointName) =>
        enumLookup({
            name,
            endpointName,
            access: "STATE_GET",
            lookup: {
                stopped: 0,
                opening: 1,
                closing: 2
            },
            cluster: "closuresWindowCovering",
            attribute: "moving",
            description: "Cover movement status",
            entityCategory: "diagnostic",
        }),
    coverMotorReversal: (name, endpointName) =>
        binary({
            name,
            endpointName,
            valueOn: [true, 1],
            valueOff: [false, 0],
            cluster: "closuresWindowCovering",
            attribute: "motorReversal",
            description: "Reverse motor direction (swap OPEN/CLOSE relays)",
            entityCategory: "config",
        }),
};

const definitions = [
    {
        zigbeeModel: [
            "TS011F-BS-PM-5",
        ],
        model: "TS011F_plug_1_2",
        vendor: "Tuya-custom",
        description: "Custom switch (https://github.com/romasku/tuya-zigbee-switch)",
        extend: [
            deviceEndpoints({ endpoints: {"relay": 1, } }),
            romasku.deviceConfig("device_config", "relay"),
            romasku.multiPressResetCount("multi_press_reset_count", "relay"),
            romasku.networkIndicator("network_led", "relay"),
            romasku.networkLedBrightness("network_led_brightness", "relay"),
            romasku.networkLedTransition("network_led_transition", "relay"),
            onOff({ endpointNames: ["relay"] }),
            romasku.scaledMeasurement({
                name: "voltage",
                cluster: "haElectricalMeasurement",
                attribute: "rmsVoltage",
                unit: "V",
                divisor: 100, // firmware reports centivolts
                precision: 2,
                endpointName: "relay",
            }),
            romasku.scaledMeasurement({
                name: "current",
                cluster: "haElectricalMeasurement",
                attribute: "rmsCurrent",
                unit: "A",
                divisor: 1000, // firmware reports milliamps
                precision: 3,
                endpointName: "relay",
            }),
            numeric({
                name: "power",
                cluster: "haElectricalMeasurement",
                attribute: "activePower",
                description: "Instantaneous measured active power",
                unit: "W",
                access: "STATE",
                endpointName: "relay",
            }),
            numeric({
                name: "apparent_power",
                cluster: "haElectricalMeasurement",
                attribute: "apparentPower",
                description: "Apparent power S = Vrms x Irms",
                unit: "VA",
                access: "STATE",
                endpointName: "relay",
            }),
            numeric({
                name: "reactive_power",
                cluster: "haElectricalMeasurement",
                attribute: "reactivePower",
                description: "Reactive power Q = sqrt(S^2 - P^2), magnitude only",
                unit: "var",
                access: "STATE",
                endpointName: "relay",
            }),
            numeric({
                name: "power_factor",
                cluster: "haElectricalMeasurement",
                attribute: "powerFactor",
                description: "Total power factor P/S in percent (equals cos φ only for linear loads; the BL0942 gives no phase, so pure cos φ is not available)",
                unit: "%",
                access: "STATE",
                endpointName: "relay",
            }),
            romasku.scaledMeasurement({
                name: "energy",
                cluster: "seMetering",
                attribute: "currentSummDelivered",
                unit: "kWh",
                divisor: 1000, // firmware reports watt-hours
                precision: 3,
                endpointName: "relay",
            }),
            binary({
                name: "reset_energy",
                cluster: "seMetering",
                attribute: {ID: 0xF000, type: 0x20}, // uint8
                valueOn: ["RESET", 1],
                valueOff: ["OFF", 0],
                description: "Set to RESET to zero the accumulated energy counter",
                access: "ALL",
                entityCategory: "config",
                endpointName: "relay",
            }),
            romasku.calibrate({
                name: "calibrate_voltage",
                attribute: {ID: 0xFF10, type: 0x21}, // uint16
                unit: "V",
                multiplier: 100, // firmware wants centivolts
                valueMax: 655,
                valueStep: 0.01, // allow e.g. 236.54 V
                description: "Measure the real voltage and enter it here to calibrate; the device computes and stores the multiplier",
                endpointName: "relay",
            }),
            romasku.calibrate({
                name: "calibrate_current",
                attribute: {ID: 0xFF11, type: 0x21}, // uint16
                unit: "A",
                multiplier: 1000, // firmware wants milliamps
                valueMax: 65,
                valueStep: 0.001, // allow e.g. 0.523 A
                description: "Measure the real current (under a steady load) and enter it here to calibrate",
                endpointName: "relay",
            }),
            romasku.calibrate({
                name: "calibrate_power",
                attribute: {ID: 0xFF12, type: 0x21}, // uint16
                unit: "W",
                multiplier: 1, // firmware wants whole watts
                valueMax: 65535,
                valueStep: 1,
                description: "Measure the real power (under a steady load) and enter it here to calibrate",
                endpointName: "relay",
            }),
            romasku.calibrationValues("calibration_values", "relay"),
            // Overload protection (always active; the hard 3680 W / 16 A peak
            // cannot be disabled or raised above the manufacturer maximum).
            romasku.overloadSetting({
                name: "overload_power_limit", attribute: {ID: 0xFF30, type: 0x21}, unit: "W", scale: 1,
                valueMin: 100, valueMax: 3680, valueStep: 10,
                description: "Soft power limit: over this for longer than the trip delay switches the relay off",
                endpointName: "relay",
            }),
            romasku.overloadSetting({
                name: "overload_current_limit", attribute: {ID: 0xFF31, type: 0x21}, unit: "A", scale: 1000,
                valueMin: 1, valueMax: 16, valueStep: 0.5,
                description: "Soft current limit: over this for longer than the trip delay switches the relay off",
                endpointName: "relay",
            }),
            romasku.overloadSetting({
                name: "overload_trip_delay", attribute: {ID: 0xFF32, type: 0x21}, unit: "s", scale: 1,
                valueMin: 0, valueMax: 3600, valueStep: 1,
                description: "How long the load may stay above the soft limit before the relay is switched off (the hard peak trips instantly regardless)",
                endpointName: "relay",
            }),
            romasku.overloadSetting({
                name: "overvoltage_warn", attribute: {ID: 0xFF33, type: 0x21}, unit: "V", scale: 100,
                valueMin: 230, valueMax: 280, valueStep: 1,
                description: "Overvoltage warning threshold (raises the alarm only, does not switch off)",
                endpointName: "relay",
            }),
            romasku.overloadSetting({
                name: "undervoltage_warn", attribute: {ID: 0xFF34, type: 0x21}, unit: "V", scale: 100,
                valueMin: 150, valueMax: 240, valueStep: 1,
                description: "Undervoltage warning threshold (raises the alarm only, does not switch off)",
                endpointName: "relay",
            }),
            romasku.overloadSetting({
                name: "overload_reconnect_delay", attribute: {ID: 0xFF35, type: 0x21}, unit: "s", scale: 1,
                valueMin: 5, valueMax: 3600, valueStep: 1,
                description: "After a trip, if the relay's power-on behavior is On or Previous, it auto-reconnects after this delay (up to 5 times, then locks out)",
                endpointName: "relay",
            }),
            romasku.overloadAlarm("overload_alarm", "relay"),
            romasku.relayIndicatorMode("relay_indicator_mode", "relay"),
            romasku.relayIndicator("relay_indicator", "relay"),
            romasku.ledBrightness("relay_led_brightness", "relay"),
            romasku.ledTransition("relay_led_transition", "relay"),
        ],
        meta: { multiEndpoint: true },
        configure: async (device, coordinatorEndpoint, logger) => {
            const endpoint1 = device.getEndpoint(1);
            await reporting.bind(endpoint1, coordinatorEndpoint, ["genOnOff"]);
            await endpoint1.configureReporting("genOnOff", [
                {attribute: "onOff", minimumReportInterval: 0, maximumReportInterval: 300, reportableChange: 0},
            ]);

            const emEndpoint = device.getEndpoint(1);
            await reporting.bind(emEndpoint, coordinatorEndpoint, ["haElectricalMeasurement", "seMetering"]);
            await emEndpoint.configureReporting("haElectricalMeasurement", [
                // reportableChange is in the attribute's raw units: voltage in
                // centivolts (500 = 5 V), current in mA (50 = 0.05 A), power in W.
                // Long max intervals (10 h) keep idle devices quiet on the mesh;
                // changes still report within the min interval.
                {attribute: "rmsVoltage", minimumReportInterval: 10, maximumReportInterval: 36000, reportableChange: 500},
                {attribute: "rmsCurrent", minimumReportInterval: 5, maximumReportInterval: 36000, reportableChange: 50},
                {attribute: "activePower", minimumReportInterval: 5, maximumReportInterval: 36000, reportableChange: 5},
                {attribute: "apparentPower", minimumReportInterval: 5, maximumReportInterval: 36000, reportableChange: 5},
                {attribute: "reactivePower", minimumReportInterval: 5, maximumReportInterval: 36000, reportableChange: 5},
                {attribute: "powerFactor", minimumReportInterval: 10, maximumReportInterval: 36000, reportableChange: 5},
                {attribute: {ID: 0xFF36, type: 0x30}, minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0},
            ]);
            // Seed the current value, so the entity is populated before the
            // first trip rather than showing "unknown" until one happens.
            await emEndpoint.read("haElectricalMeasurement", [0xFF36]);
            await emEndpoint.configureReporting("seMetering", [
                {attribute: "currentSummDelivered", minimumReportInterval: 0, maximumReportInterval: 36000, reportableChange: 10},
            ]);



        },
        ota: true,
    },
];

module.exports = definitions;

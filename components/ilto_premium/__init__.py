# SPDX-License-Identifier: 0BSD
# Copyright (C) 2026 quasd

import esphome.codegen as cg
from esphome import pins
import esphome.config_validation as cv
from esphome.components import binary_sensor, select, sensor, switch, uart
from esphome.const import (
    CONF_ID,
    CONF_RESTORE_MODE,
    DEVICE_CLASS_CARBON_DIOXIDE,
    DEVICE_CLASS_HUMIDITY,
    DEVICE_CLASS_PROBLEM,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_MINUTE,
    UNIT_PARTS_PER_MILLION,
    UNIT_PERCENT,
    UNIT_REVOLUTIONS_PER_MINUTE,
)

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "binary_sensor", "select", "switch"]

ilto_premium_ns = cg.esphome_ns.namespace("ilto_premium")
IltoPremium = ilto_premium_ns.class_("IltoPremium", cg.Component, uart.UARTDevice)
IltoSwitch = ilto_premium_ns.class_("IltoSwitch", switch.Switch, cg.Parented.template(IltoPremium))
IltoSwitchKind = IltoSwitch.enum("Kind")
IltoModeSelect = ilto_premium_ns.class_(
    "IltoModeSelect", select.Select, cg.Parented.template(IltoPremium)
)
# The order is the C++ side's mode numbers: Off = 0, speeds 1-5, Fireplace = 6.
MODE_OPTIONS = ["Off", "1", "2", "3", "4", "5", "Fireplace"]
IltoStepSelect = ilto_premium_ns.class_(
    "IltoStepSelect", select.Select, cg.Parented.template(IltoPremium)
)
# The panel's 3-step choices on the CASA bus layout, in the C++ side's order (STEP_CODE / STEP_MINUTES).
STEP_OPTIONS = ["Away", "Home", "Boost 30 min", "Boost 60 min", "Boost 120 min", "Boost"]


def _no_restore(config):
    # A restored "off" would switch the unit off on every ESP restart.
    if str(config[CONF_RESTORE_MODE]) != "DISABLED":
        raise cv.Invalid("restore_mode must stay DISABLED: the switch mirrors the unit's own state")
    return config


# The unit's power and fireplace mode, sending the panel's commands.
SWITCHES = {
    "ventilation": ("POWER", "mdi:hvac"),
    "fireplace_mode": ("FIREPLACE", "mdi:fireplace"),
}

# The panel's sensor 1..8 screens, in that order. openHAB calls the first ones
# outdoor/supply/extract, but on an ILTO 440 econo sensor 1 reads 26 C in winter, so which
# sensor is which has to be worked out per unit.
TEMPERATURES = [f"temperature_{i}" for i in range(1, 9)]
FAN_RPMS = ["supply_fan_rpm", "extract_fan_rpm"]
FAN_POWERS = ["supply_fan_power", "extract_fan_power"]
# Raw codes whose meaning is not documented yet; kept as diagnostics to learn them.
RAW_CODES = ["operating_mode", "unit_state"]
STATES = ["fireplace", "running", "bypass", "boost", "defrosting", "freeze_protection"]
PROBLEMS = ["filter_alarm", "service_reminder", "fan_failure", "emergency_stop"]

SCHEMA = {
    cv.GenerateID(): cv.declare_id(IltoPremium),
    # The transceiver's DE and RE, tied together: high while sending.
    cv.Optional("flow_control_pin"): pins.gpio_output_pin_schema,
    # Send on this UART instead of the receiving one.
    cv.Optional("tx_uart_id"): cv.use_id(uart.UARTComponent),
    cv.Optional("efficiency"): sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
        icon="mdi:heat-wave",
    ),
    # Raw bytes as openHAB reads them; the scale is unchecked (0 on units without the option).
    cv.Optional("co2"): sensor.sensor_schema(
        unit_of_measurement=UNIT_PARTS_PER_MILLION,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_CARBON_DIOXIDE,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional("humidity"): sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_HUMIDITY,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    # The speed selected on the panel, 1-5.
    cv.Optional("fan_speed"): sensor.sensor_schema(
        accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT, icon="mdi:fan"
    ),
}
# One control for off / speed 1-5 / fireplace.
SCHEMA[cv.Optional("mode")] = select.select_schema(IltoModeSelect, icon="mdi:fan")
# Away / Home / Boost, as the panel's 3-step screen (CASA bus layout only).
SCHEMA[cv.Optional("step_mode")] = select.select_schema(IltoStepSelect, icon="mdi:home-switch")
# Minutes left of a timed boost (CASA bus layout), counted by the ESP from the boost's start; 0 otherwise.
SCHEMA[cv.Optional("boost_time")] = sensor.sensor_schema(
    unit_of_measurement=UNIT_MINUTE,
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:timer-outline",
)
for key, (_, icon) in SWITCHES.items():
    SCHEMA[cv.Optional(key)] = cv.All(
        switch.switch_schema(IltoSwitch, icon=icon, default_restore_mode="DISABLED"), _no_restore
    )
for key in TEMPERATURES:
    SCHEMA[cv.Optional(key)] = sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_TEMPERATURE,
        state_class=STATE_CLASS_MEASUREMENT,
    )
for key in FAN_RPMS:
    SCHEMA[cv.Optional(key)] = sensor.sensor_schema(
        unit_of_measurement=UNIT_REVOLUTIONS_PER_MINUTE,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
        icon="mdi:fan",
    )
for key in FAN_POWERS:
    SCHEMA[cv.Optional(key)] = sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
        icon="mdi:fan",
    )
for key in RAW_CODES:
    SCHEMA[cv.Optional(key)] = sensor.sensor_schema(
        accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    )
for key in STATES:
    SCHEMA[cv.Optional(key)] = binary_sensor.binary_sensor_schema()
for key in PROBLEMS:
    SCHEMA[cv.Optional(key)] = binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_PROBLEM
    )

CONFIG_SCHEMA = (
    cv.Schema(SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "ilto_premium", baud_rate=38400, require_rx=True
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    if "flow_control_pin" in config:
        pin = await cg.gpio_pin_expression(config["flow_control_pin"])
        cg.add(var.set_flow_control_pin(pin))
    if "tx_uart_id" in config:
        tx_uart = await cg.get_variable(config["tx_uart_id"])
        cg.add(var.set_tx_uart(tx_uart))

    for key in ["efficiency", "co2", "humidity", "fan_speed", "boost_time", *TEMPERATURES, *FAN_RPMS, *FAN_POWERS, *RAW_CODES]:
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, f"set_{key}_sensor")(sens))
    if "mode" in config:
        sel = await select.new_select(config["mode"], options=MODE_OPTIONS)
        await cg.register_parented(sel, var)
        cg.add(var.set_mode_select(sel))
    if "step_mode" in config:
        sel = await select.new_select(config["step_mode"], options=STEP_OPTIONS)
        await cg.register_parented(sel, var)
        cg.add(var.set_step_select(sel))
    for key, (kind, _) in SWITCHES.items():
        if key in config:
            sw = await switch.new_switch(config[key])
            await cg.register_parented(sw, var)
            cg.add(sw.set_kind(IltoSwitchKind.enum(kind)))
            cg.add(getattr(var, f"set_{key}_switch")(sw))
    for key in [*STATES, *PROBLEMS]:
        if key in config:
            sens = await binary_sensor.new_binary_sensor(config[key])
            cg.add(getattr(var, f"set_{key}_binary_sensor")(sens))

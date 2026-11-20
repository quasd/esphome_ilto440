# SPDX-License-Identifier: 0BSD
# Copyright (C) 2026 quasd

import esphome.codegen as cg
from esphome import pins
import esphome.config_validation as cv
from esphome.components import binary_sensor, select, sensor, switch, text_sensor, uart
from esphome.const import (
    CONF_CO2,
    CONF_FLOW_CONTROL_PIN,
    CONF_HUMIDITY,
    CONF_ID,
    CONF_MODE,
    CONF_RESTORE_MODE,
    CONF_STEP_MODE,
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

# Keys ESPHome has no constant for.
CONF_TX_UART_ID = "tx_uart_id"
CONF_EFFICIENCY = "efficiency"
CONF_FAN_SPEED = "fan_speed"
CONF_BOOST_TIME = "boost_time"
CONF_UNIT_CLOCK = "unit_clock"
CONF_BASIC_DISPLAY = "basic_display"
AUTO_LOAD = ["sensor", "binary_sensor", "select", "switch", "text_sensor"]

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
# starting_up: the unit's start-up minute after switching on (0x73 p[4] 0x00 while running and not in
# fireplace mode, on both bus layouts), in which speed and fireplace commands are not reliably taken.
STATES = ["fireplace", "running", "starting_up", "bypass", "boost", "defrosting", "freeze_protection",
          "after_heating", "after_heating_enabled"]
PROBLEMS = ["filter_alarm", "service_reminder", "fan_failure", "emergency_stop"]

SCHEMA = {
    cv.GenerateID(): cv.declare_id(IltoPremium),
    # The transceiver's DE and RE, tied together: high while sending.
    cv.Optional(CONF_FLOW_CONTROL_PIN): pins.gpio_output_pin_schema,
    # Send on this UART instead of the receiving one.
    cv.Optional(CONF_TX_UART_ID): cv.use_id(uart.UARTComponent),
    cv.Optional(CONF_EFFICIENCY): sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
        icon="mdi:heat-wave",
    ),
    # Raw bytes as openHAB reads them; the scale is unchecked (0 on units without the option).
    cv.Optional(CONF_CO2): sensor.sensor_schema(
        unit_of_measurement=UNIT_PARTS_PER_MILLION,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_CARBON_DIOXIDE,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional(CONF_HUMIDITY): sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_HUMIDITY,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    # The speed selected on the panel, 1-5.
    cv.Optional(CONF_FAN_SPEED): sensor.sensor_schema(
        accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT, icon="mdi:fan"
    ),
}
# One control for off / speed 1-5 / fireplace.
SCHEMA[cv.Optional(CONF_MODE)] = select.select_schema(IltoModeSelect, icon="mdi:fan")
# Away / Home / Boost, as the panel's 3-step screen (CASA bus layout only).
SCHEMA[cv.Optional(CONF_STEP_MODE)] = select.select_schema(IltoStepSelect, icon="mdi:home-switch")
# The unit's own clock, "Fri 19:23" (CASA bus layout).
SCHEMA[cv.Optional(CONF_UNIT_CLOCK)] = text_sensor.text_sensor_schema(
    icon="mdi:clock-outline", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
)
# The panel's basic display from 0x73 p[15], "Basic display 1" / "Basic display 2" (CASA bus layout).
SCHEMA[cv.Optional(CONF_BASIC_DISPLAY)] = text_sensor.text_sensor_schema(
    icon="mdi:monitor-dashboard", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
)
# Minutes left of a timed boost (CASA bus layout), counted by the ESP from the boost's start; 0 otherwise.
SCHEMA[cv.Optional(CONF_BOOST_TIME)] = sensor.sensor_schema(
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

# format_hex_pretty_to() with a stack buffer (the log lines) first appeared in ESPHome 2026.1.0.
CONFIG_SCHEMA = cv.All(
    cv.Schema(SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA),
    cv.require_esphome_version(2026, 1, 0),
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "ilto_premium", baud_rate=38400, require_rx=True
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    if CONF_FLOW_CONTROL_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_FLOW_CONTROL_PIN])
        cg.add(var.set_flow_control_pin(pin))
    if CONF_TX_UART_ID in config:
        tx_uart = await cg.get_variable(config[CONF_TX_UART_ID])
        cg.add(var.set_tx_uart(tx_uart))

    for key in [
        CONF_EFFICIENCY, CONF_CO2, CONF_HUMIDITY, CONF_FAN_SPEED, CONF_BOOST_TIME,
        *TEMPERATURES, *FAN_RPMS, *FAN_POWERS, *RAW_CODES,
    ]:
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, f"set_{key}_sensor")(sens))
    if CONF_MODE in config:
        sel = await select.new_select(config[CONF_MODE], options=MODE_OPTIONS)
        await cg.register_parented(sel, var)
        cg.add(var.set_mode_select(sel))
    if CONF_BASIC_DISPLAY in config:
        ts = await text_sensor.new_text_sensor(config[CONF_BASIC_DISPLAY])
        cg.add(var.set_basic_display_text_sensor(ts))
    if CONF_UNIT_CLOCK in config:
        ts = await text_sensor.new_text_sensor(config[CONF_UNIT_CLOCK])
        cg.add(var.set_unit_clock_text_sensor(ts))
    if CONF_STEP_MODE in config:
        sel = await select.new_select(config[CONF_STEP_MODE], options=STEP_OPTIONS)
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

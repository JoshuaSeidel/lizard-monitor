"""Turns an SHT4x's raw readings into readings the controller may act on.

The lesson from tank-monitor's DS18B20: sensor-fault signatures pass the
CRC. A reading can be well-formed and wrong, and every wrong reading drives
the heat the wrong way. So nothing downstream ever sees the SHT4x directly.
Each raw sample is checked, and a sample that fails a check is "no reading"
-- never a value:

  * read error         NaN from the driver
  * out of range       outside 32-122 degF or 0-100 %RH
  * jump               faster than max_rate from the previous raw sample (a
                       lone spike is dropped; a real step is accepted on the
                       next sample, which agrees with it)
  * frozen             temperature AND humidity bit-identical for
                       frozen_time. At the SHT45's 0.003 degC resolution a
                       live sensor never does this; a hung one does.
  * stale              no accepted sample for stale_timeout

When the last accepted sample is older than stale_timeout the outputs go to
NaN (unknown) and stay there until a good sample arrives. The heat loop
treats NaN as "sensor fault: heat off".

CONDENSATION. A misted enclosure can wet the sensing element and pin RH at
100 %. The SHT4x has an on-chip heater for exactly this, but ESPHome's
built-in heater_max_duty fires it on a blind timer whatever the humidity --
and a heater pulse leaves the die tens of degrees hot, biasing the
temperature the heat loop reads. Here the heater fires only after RH has
read >= condensation_rh for condensation_time, at most once per
heater_min_gap, immediately after a measurement (the bus is idle then), and
samples are ignored for heater_blank afterwards while the die cools. The
last good value is held through that blank: it is minutes old at most.

Calibration offsets (degF, %RH) are applied here, on the device, and set at
runtime from number entities via set_offsets().
"""

import esphome.codegen as cg
from esphome.components import i2c, sensor, text_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_HUMIDITY,
    CONF_ID,
    CONF_STATUS,
    CONF_TEMPERATURE,
    DEVICE_CLASS_HUMIDITY,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_PERCENT,
)

CODEOWNERS = ["@JoshuaSeidel"]
DEPENDENCIES = ["i2c"]
AUTO_LOAD = ["sensor", "text_sensor"]
MULTI_CONF = True

sht_guard_ns = cg.esphome_ns.namespace("sht_guard")
ShtGuard = sht_guard_ns.class_("ShtGuard", cg.PollingComponent, i2c.I2CDevice)

CONF_RAW_TEMPERATURE = "raw_temperature"
CONF_RAW_HUMIDITY = "raw_humidity"
CONF_STALE_TIMEOUT = "stale_timeout"
CONF_MAX_RATE = "max_rate"
CONF_FROZEN_TIME = "frozen_time"
CONF_CONDENSATION_RH = "condensation_rh"
CONF_CONDENSATION_TIME = "condensation_time"
CONF_HEATER_MIN_GAP = "heater_min_gap"
CONF_HEATER_BLANK = "heater_blank"

UNIT_FAHRENHEIT = "°F"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ShtGuard),
            # The SHT4x's own sensors, temperature already in degF.
            cv.Required(CONF_RAW_TEMPERATURE): cv.use_id(sensor.Sensor),
            cv.Required(CONF_RAW_HUMIDITY): cv.use_id(sensor.Sensor),
            cv.Required(CONF_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_FAHRENHEIT,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Required(CONF_HUMIDITY): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_HUMIDITY,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Required(CONF_STATUS): text_sensor.text_sensor_schema(icon="mdi:thermometer-check"),
            cv.Optional(CONF_STALE_TIMEOUT, default="120s"): cv.positive_time_period_milliseconds,
            # degF per minute between consecutive raw samples.
            cv.Optional(CONF_MAX_RATE, default=5.0): cv.positive_float,
            cv.Optional(CONF_FROZEN_TIME, default="30min"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_CONDENSATION_RH, default=98.0): cv.float_range(min=80.0, max=100.0),
            cv.Optional(CONF_CONDENSATION_TIME, default="10min"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_HEATER_MIN_GAP, default="5min"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_HEATER_BLANK, default="120s"): cv.positive_time_period_milliseconds,
        }
    )
    .extend(cv.polling_component_schema("5s"))
    .extend(i2c.i2c_device_schema(0x44))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    raw_t = await cg.get_variable(config[CONF_RAW_TEMPERATURE])
    raw_h = await cg.get_variable(config[CONF_RAW_HUMIDITY])
    cg.add(var.set_raw(raw_t, raw_h))
    out_t = await sensor.new_sensor(config[CONF_TEMPERATURE])
    out_h = await sensor.new_sensor(config[CONF_HUMIDITY])
    status = await text_sensor.new_text_sensor(config[CONF_STATUS])
    cg.add(var.set_outputs(out_t, out_h, status))

    cg.add(var.set_stale_timeout(config[CONF_STALE_TIMEOUT]))
    cg.add(var.set_max_rate(config[CONF_MAX_RATE]))
    cg.add(var.set_frozen_time(config[CONF_FROZEN_TIME]))
    cg.add(var.set_condensation(config[CONF_CONDENSATION_RH], config[CONF_CONDENSATION_TIME]))
    cg.add(var.set_heater(config[CONF_HEATER_MIN_GAP], config[CONF_HEATER_BLANK]))

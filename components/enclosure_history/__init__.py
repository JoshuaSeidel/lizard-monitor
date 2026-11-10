"""On-device history for an enclosure with no Home Assistant.

Adapted from tank-monitor's tank_history, minus the water-test log. Keeps two
rings per series -- 288 five-minute means (24 h) and 720 hourly means
(30 days) -- saved to flash, so a reboot or power cut loses at most the half
hour since the last save (a planned reboot or OTA loses nothing). Serves:

  GET /history.json?range=day|month   the rings, oldest first, null = gap
  GET /charts                         a page that draws them

A gap (power off, no clock) stays a gap. Nothing is interpolated across it.
"""

import esphome.codegen as cg
from esphome.components import sensor, time, web_server_base
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_NAME, CONF_SENSOR, CONF_TIME_ID, CONF_UNIT_OF_MEASUREMENT

CODEOWNERS = ["@JoshuaSeidel"]
DEPENDENCIES = ["web_server_base", "time"]
AUTO_LOAD = ["web_server_base"]

enclosure_history_ns = cg.esphome_ns.namespace("enclosure_history")
EnclosureHistory = enclosure_history_ns.class_("EnclosureHistory", cg.PollingComponent)

CONF_WEB_SERVER_BASE_ID = "web_server_base_id"
CONF_SERIES = "series"
CONF_SCALE = "scale"
CONF_LABEL = "label"
CONF_CHART = "chart"

# Stored as int16 of value * scale, so scale sets both the resolution and the
# ceiling: 100 keeps 0.01 degF and tops out at 327 degF.
SERIES_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.All(cv.string, cv.Length(min=1, max=12)),
        cv.Required(CONF_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_SCALE, default=1.0): cv.positive_float,
        # How the charts page shows it. Series with the same chart share axes.
        cv.Required(CONF_LABEL): cv.All(cv.string, cv.Length(min=1, max=24)),
        cv.Required(CONF_CHART): cv.All(cv.string, cv.Length(min=1, max=24)),
        cv.Required(CONF_UNIT_OF_MEASUREMENT): cv.All(cv.string, cv.Length(max=8)),
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(EnclosureHistory),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server_base.WebServerBase),
        cv.Required(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
        cv.Required(CONF_SERIES): cv.All(cv.ensure_list(SERIES_SCHEMA), cv.Length(min=1, max=10)),
    }
).extend(cv.polling_component_schema("30s"))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    cg.add(var.set_base(base))
    clock = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time(clock))
    for s in config[CONF_SERIES]:
        sens = await cg.get_variable(s[CONF_SENSOR])
        cg.add(
            var.add_series(
                s[CONF_NAME], sens, s[CONF_SCALE], s[CONF_LABEL], s[CONF_CHART], s[CONF_UNIT_OF_MEASUREMENT]
            )
        )

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import DEVICE_CLASS_BATTERY, STATE_CLASS_MEASUREMENT

from . import CONF_TIMEMORE_DOT_ID, TimemoreDot

DEPENDENCIES = ["timemore_dot"]

CONF_WEIGHT = "weight"
CONF_BATTERY_LEVEL = "battery_level"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TIMEMORE_DOT_ID): cv.use_id(TimemoreDot),
        cv.Optional(CONF_WEIGHT): sensor.sensor_schema(
            unit_of_measurement="g",
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:scale",
        ),
        cv.Optional(CONF_BATTERY_LEVEL): sensor.sensor_schema(
            unit_of_measurement="%",
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_BATTERY,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TIMEMORE_DOT_ID])
    if CONF_WEIGHT in config:
        sens = await sensor.new_sensor(config[CONF_WEIGHT])
        cg.add(parent.set_weight_sensor(sens))
    if CONF_BATTERY_LEVEL in config:
        sens = await sensor.new_sensor(config[CONF_BATTERY_LEVEL])
        cg.add(parent.set_battery_sensor(sens))

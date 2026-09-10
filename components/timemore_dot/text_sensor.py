import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import CONF_TIMEMORE_DOT_ID, TimemoreDot

DEPENDENCIES = ["timemore_dot"]

CONF_STATUS = "status"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TIMEMORE_DOT_ID): cv.use_id(TimemoreDot),
        cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(
            icon="mdi:bluetooth",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TIMEMORE_DOT_ID])
    if CONF_STATUS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_STATUS])
        cg.add(parent.set_status_sensor(sens))

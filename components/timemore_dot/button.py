import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button

from . import CONF_TIMEMORE_DOT_ID, TimemoreDot, timemore_dot_ns

DEPENDENCIES = ["timemore_dot"]

TareButton = timemore_dot_ns.class_("TareButton", button.Button, cg.Component)

CONFIG_SCHEMA = button.button_schema(TareButton).extend(
    {
        cv.GenerateID(CONF_TIMEMORE_DOT_ID): cv.use_id(TimemoreDot),
    }
)


async def to_code(config):
    var = await button.new_button(config)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_TIMEMORE_DOT_ID])
    cg.add(var.set_parent(parent))

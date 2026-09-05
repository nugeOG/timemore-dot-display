import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@nuge"]
DEPENDENCIES = ["esp32"]
CONFLICTS_WITH = ["esp32_ble_tracker", "ble_client"]
AUTO_LOAD = ["sensor", "binary_sensor", "button"]
MULTI_CONF = True

timemore_dot_ns = cg.esphome_ns.namespace("timemore_dot")
TimemoreDot = timemore_dot_ns.class_("TimemoreDot", cg.Component)

CONF_TIMEMORE_DOT_ID = "timemore_dot_id"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(TimemoreDot),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    # Pinned so behavior doesn't silently shift on a NimBLE-Arduino bump --
    # the exact API surface used in timemore_dot.cpp/.h (setScanCallbacks,
    # secureConnection, subscribe callback signature, etc.) was written
    # against this version and has NOT been verified by an actual build,
    # per HANDOFF.md. Re-check against NimBLE-Arduino's changelog if you
    # bump this.
    cg.add_library("h2zero/NimBLE-Arduino", "1.4.1")

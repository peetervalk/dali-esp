import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import CONF_ID

from .. import DaliComponent, dali_ns

DEPENDENCIES = ["dali"]

CONF_DALI_ID    = "dali_id"
CONF_BUTTON_TYPE = "type"

DaliScanButton = dali_ns.class_("DaliScanButton", button.Button)

BUTTON_TYPES = {
    "scan":          0,
    "refresh":       1,
    "identify":      2,
    "find_couplers": 3,
    "turn_on":       4,
    "turn_off":      5,
    "max":           6,
    "min":           7,
}

CONFIG_SCHEMA = button.button_schema(DaliScanButton).extend(
    {
        cv.GenerateID(CONF_DALI_ID): cv.use_id(DaliComponent),
        cv.Optional(CONF_BUTTON_TYPE, default="scan"): cv.enum(BUTTON_TYPES, lower=True),
    }
)


async def to_code(config):
    btn = await button.new_button(config)
    parent = await cg.get_variable(config[CONF_DALI_ID])
    cg.add(btn.set_dali_component(parent))
    cg.add(btn.set_button_type(config[CONF_BUTTON_TYPE]))

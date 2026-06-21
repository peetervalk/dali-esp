import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import CONF_ID

from .. import DaliComponent, dali_ns

DEPENDENCIES = ["dali"]

CONF_DALI_ID = "dali_id"

DaliScanButton = dali_ns.class_("DaliScanButton", button.Button)

CONFIG_SCHEMA = button.BUTTON_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(DaliScanButton),
        cv.GenerateID(CONF_DALI_ID): cv.use_id(DaliComponent),
    }
)


async def to_code(config):
    btn = cg.new_Pvariable(config[CONF_ID])
    await button.register_button(btn, config)
    parent = await cg.get_variable(config[CONF_DALI_ID])
    cg.add(btn.set_dali_component(parent))

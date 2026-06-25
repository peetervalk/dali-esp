import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text

from .. import DaliComponent, dali_ns

DEPENDENCIES = ["dali"]

DaliCommandText = dali_ns.class_("DaliCommandText", text.Text, cg.Component)

CONF_DALI_ID = "dali_id"

CONFIG_SCHEMA = text.text_schema(DaliCommandText).extend(
    {
        cv.GenerateID(CONF_DALI_ID): cv.use_id(DaliComponent),
    }
)


async def to_code(config):
    var = await text.new_text(config)

    parent = await cg.get_variable(config[CONF_DALI_ID])
    cg.add(var.set_dali_component(parent))

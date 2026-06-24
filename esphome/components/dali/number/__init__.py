import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number

from .. import DaliComponent, dali_ns

DEPENDENCIES = ["dali"]

CONF_DALI_ID = "dali_id"

DaliAddressNumber = dali_ns.class_("DaliAddressNumber", number.Number)

CONFIG_SCHEMA = number.number_schema(DaliAddressNumber).extend(
    {
        cv.GenerateID(CONF_DALI_ID): cv.use_id(DaliComponent),
    }
)


async def to_code(config):
    var = await number.new_number(config, min_value=0, max_value=63, step=1)
    parent = await cg.get_variable(config[CONF_DALI_ID])
    cg.add(var.set_dali_component(parent))

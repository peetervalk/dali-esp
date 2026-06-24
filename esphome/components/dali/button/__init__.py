import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import CONF_ID

from .. import DaliComponent, dali_ns

DEPENDENCIES = ["dali"]

CONF_DALI_ID    = "dali_id"
CONF_BUTTON_TYPE = "type"

DaliScanButton    = dali_ns.class_("DaliScanButton",    button.Button)
DaliRefreshButton = dali_ns.class_("DaliRefreshButton", button.Button)

CONFIG_SCHEMA = button.button_schema(DaliScanButton).extend(
    {
        cv.GenerateID(CONF_DALI_ID): cv.use_id(DaliComponent),
        cv.Optional(CONF_BUTTON_TYPE, default="scan"): cv.one_of(
            "scan", "refresh", lower=True
        ),
    }
)


async def to_code(config):
    if config[CONF_BUTTON_TYPE] == "refresh":
        btn = cg.new_Pvariable(config[CONF_ID], DaliRefreshButton)
        await button.register_button(btn, config)
    else:
        btn = await button.new_button(config)

    parent = await cg.get_variable(config[CONF_DALI_ID])
    cg.add(btn.set_dali_component(parent))

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light
from esphome.const import CONF_OUTPUT_ID

from .. import DaliComponent, dali_ns

DEPENDENCIES = ["dali"]

DaliLightOutput = dali_ns.class_("DaliLightOutput", light.LightOutput)

CONF_DALI_ID = "dali_id"
CONF_TARGET_TYPE = "target_type"
CONF_TARGET_ADDRESS = "target_address"

# Must match DaliAddressType enum values in dali_frame.h
TARGET_TYPES = {
    "short": 0,       # DALI_ADDR_SHORT
    "group": 1,       # DALI_ADDR_GROUP
    "broadcast": 2,   # DALI_ADDR_BROADCAST
}

CONFIG_SCHEMA = light.BRIGHTNESS_ONLY_LIGHT_SCHEMA.extend(
    {
        cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(DaliLightOutput),
        cv.GenerateID(CONF_DALI_ID): cv.use_id(DaliComponent),
        cv.Required(CONF_TARGET_TYPE): cv.enum(TARGET_TYPES, lower=True),
        cv.Optional(CONF_TARGET_ADDRESS, default=0): cv.int_range(min=0, max=63),
    }
)


async def to_code(config):
    output = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await light.register_light(output, config)

    # Ensure the parent DaliComponent is registered (creates the include dep)
    await cg.get_variable(config[CONF_DALI_ID])

    cg.add(output.set_target(config[CONF_TARGET_TYPE], config[CONF_TARGET_ADDRESS]))

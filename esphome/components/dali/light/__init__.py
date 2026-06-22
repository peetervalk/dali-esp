import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light
from esphome.components.light import LightType

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

def _validate_target_address(config):
    if config.get(CONF_TARGET_TYPE) == TARGET_TYPES["group"]:
        addr = config.get(CONF_TARGET_ADDRESS, 0)
        if addr > 15:
            raise cv.Invalid(
                f"target_address for group must be 0-15, got {addr}",
                [CONF_TARGET_ADDRESS],
            )
    return config


CONFIG_SCHEMA = cv.All(
    light.light_schema(DaliLightOutput, LightType.BRIGHTNESS_ONLY).extend(
        {
            cv.GenerateID(CONF_DALI_ID): cv.use_id(DaliComponent),
            cv.Required(CONF_TARGET_TYPE): cv.enum(TARGET_TYPES, lower=True),
            cv.Optional(CONF_TARGET_ADDRESS, default=0): cv.int_range(min=0, max=63),
        }
    ),
    _validate_target_address,
)


async def to_code(config):
    var = await light.new_light(config)

    # Ensure the parent DaliComponent is a build dependency
    await cg.get_variable(config[CONF_DALI_ID])

    cg.add(var.set_target(config[CONF_TARGET_TYPE], config[CONF_TARGET_ADDRESS]))

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light
from esphome.components.light import LightType

from .. import DaliComponent, dali_ns

DEPENDENCIES = ["dali"]

DaliLightOutput = dali_ns.class_("DaliLightOutput", light.LightOutput)

CONF_DALI_ID        = "dali_id"
CONF_TARGET_TYPE    = "target_type"
CONF_TARGET_ADDRESS = "target_address"
CONF_QUERY_ADDRESS  = "query_address"
CONF_MEMBER_GROUPS  = "member_groups"

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
            # Optional short address for QUERY_ACTUAL_LEVEL (boot/refresh/poll).
            # Useful when target_type is 'group' and a representative gear address
            # is known.  Omit if state sync via queries is not needed.
            cv.Optional(CONF_QUERY_ADDRESS): cv.int_range(min=0, max=63),
            # List of DALI group numbers (0-15) this entity belongs to.
            # Only needed for short-address entities so that group-addressed
            # dispatch results also update this entity's state in HA.
            cv.Optional(CONF_MEMBER_GROUPS, default=[]): cv.All(
                cv.ensure_list(cv.int_range(min=0, max=15)),
                cv.Length(max=16),
            ),
        }
    ),
    _validate_target_address,
)


async def to_code(config):
    var = await light.new_light(config)

    parent = await cg.get_variable(config[CONF_DALI_ID])
    cg.add(var.set_target(config[CONF_TARGET_TYPE], config[CONF_TARGET_ADDRESS]))

    # member_groups must be set before set_dali_component because
    # set_dali_component calls register_light which snapshots member_groups_.
    groups = config.get(CONF_MEMBER_GROUPS, [])
    if groups:
        bitmask = 0
        for g in groups:
            bitmask |= (1 << g)
        cg.add(var.set_member_groups(bitmask))

    cg.add(var.set_dali_component(parent))

    if CONF_QUERY_ADDRESS in config:
        cg.add(var.set_query_address(config[CONF_QUERY_ADDRESS]))

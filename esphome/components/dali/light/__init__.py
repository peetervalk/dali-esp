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
CONF_MIN_LEVEL      = "min_level"
CONF_MAX_LEVEL      = "max_level"
CONF_DIMMING_CURVE  = "dimming_curve"

# Must match DaliAddressType enum values in dali_frame.h
TARGET_TYPES = {
    "short": 0,       # DALI_ADDR_SHORT
    "group": 1,       # DALI_ADDR_GROUP
    "broadcast": 2,   # DALI_ADDR_BROADCAST
}

DIMMING_CURVES = {
    "auto": 0xFF,
    "standard": 0,
    "linear": 1,
}

def _validate_target_address(config):
    if config.get(CONF_TARGET_TYPE) == TARGET_TYPES["group"]:
        addr = config.get(CONF_TARGET_ADDRESS, 0)
        if addr > 15:
            raise cv.Invalid(
                f"target_address for group must be 0-15, got {addr}",
                [CONF_TARGET_ADDRESS],
            )
    min_level = config.get(CONF_MIN_LEVEL)
    max_level = config.get(CONF_MAX_LEVEL)
    if min_level is not None and max_level is not None and min_level > max_level:
        raise cv.Invalid("min_level must not exceed max_level")
    return config


CONFIG_SCHEMA = cv.All(
    light.light_schema(DaliLightOutput, LightType.BRIGHTNESS_ONLY).extend(
        {
            cv.GenerateID(CONF_DALI_ID): cv.use_id(DaliComponent),
            cv.Required(CONF_TARGET_TYPE): cv.enum(TARGET_TYPES, lower=True),
            cv.Optional(CONF_TARGET_ADDRESS, default=0): cv.int_range(min=0, max=63),
            # Optional QUERY_ACTUAL_LEVEL short-address override. A short target
            # queries itself by default; group/broadcast targets need a
            # representative gear address (or scan-derived group membership).
            cv.Optional(CONF_QUERY_ADDRESS): cv.int_range(min=0, max=63),
            # List of DALI group numbers (0-15) this entity belongs to.
            # Only needed for short-address entities so that group-addressed
            # dispatch results also update this entity's state in HA.
            cv.Optional(CONF_MEMBER_GROUPS, default=[]): cv.All(
                cv.ensure_list(cv.int_range(min=0, max=15)),
                cv.Length(max=16),
            ),
            cv.Optional(CONF_MIN_LEVEL): cv.int_range(min=1, max=254),
            cv.Optional(CONF_MAX_LEVEL): cv.int_range(min=1, max=254),
            cv.Optional(CONF_DIMMING_CURVE, default="auto"): cv.enum(
                DIMMING_CURVES, lower=True
            ),
        }
    ),
    _validate_target_address,
)


async def to_code(config):
    var = await light.new_light(config)

    parent = await cg.get_variable(config[CONF_DALI_ID])
    cg.add(var.set_target(config[CONF_TARGET_TYPE], config[CONF_TARGET_ADDRESS]))

    if CONF_MIN_LEVEL in config:
        cg.add(var.set_min_level_override(config[CONF_MIN_LEVEL]))
    if CONF_MAX_LEVEL in config:
        cg.add(var.set_max_level_override(config[CONF_MAX_LEVEL]))
    if config[CONF_DIMMING_CURVE] != DIMMING_CURVES["auto"]:
        cg.add(var.set_dimming_curve_override(config[CONF_DIMMING_CURVE]))

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

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID, CONF_ADDRESS

from .. import DaliComponent, dali_ns

DEPENDENCIES = ["dali"]

DaliInputSensor = dali_ns.class_("DaliInputSensor", sensor.Sensor, cg.Component)

CONF_DALI_ID       = "dali_id"
CONF_INSTANCE      = "instance"
CONF_POLL_INTERVAL = "poll_interval"
CONF_VALUE_BYTES   = "value_bytes"
CONF_SCALE         = "scale"
CONF_OFFSET        = "offset"

CONFIG_SCHEMA = sensor.sensor_schema(DaliInputSensor).extend(
    {
        cv.GenerateID(CONF_DALI_ID): cv.use_id(DaliComponent),
        cv.Required(CONF_ADDRESS): cv.int_range(min=0, max=63),
        cv.Required(CONF_INSTANCE): cv.int_range(min=0, max=31),
        cv.Optional(CONF_POLL_INTERVAL, default=30): cv.int_range(min=1, max=3600),
        # 1 for 8-bit instances (occupancy, humidity),
        # 2 for 16-bit instances (lux, temperature).
        cv.Optional(CONF_VALUE_BYTES, default=1): cv.one_of(1, 2, int=True),
        cv.Optional(CONF_SCALE, default=1.0): cv.float_,
        cv.Optional(CONF_OFFSET, default=0.0): cv.float_,
    }
)


async def to_code(config):
    var = await sensor.new_sensor(config)

    parent = await cg.get_variable(config[CONF_DALI_ID])
    cg.add(var.set_address(config[CONF_ADDRESS]))
    cg.add(var.set_instance(config[CONF_INSTANCE]))
    cg.add(var.set_poll_interval_s(config[CONF_POLL_INTERVAL]))
    cg.add(var.set_value_bytes(config[CONF_VALUE_BYTES]))
    cg.add(var.set_scale(config[CONF_SCALE]))
    cg.add(var.set_offset(config[CONF_OFFSET]))
    cg.add(parent.register_input_sensor(var))

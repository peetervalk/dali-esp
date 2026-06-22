import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_ID

CODEOWNERS = ["@peetervalk"]
DEPENDENCIES = ["esp32"]
# Always bundle text_sensor C++ files — we use TextSensor in dali_component.cpp.
AUTO_LOAD = ["text_sensor"]

CONF_TX_PIN = "tx_pin"
CONF_RX_PIN = "rx_pin"
CONF_SCAN_STATUS = "scan_status"

dali_ns = cg.esphome_ns.namespace("dali")
DaliComponent = dali_ns.class_("DaliComponent", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DaliComponent),
        cv.Required(CONF_TX_PIN): cv.int_range(min=0, max=39),
        cv.Required(CONF_RX_PIN): cv.int_range(min=0, max=39),
        # Optional: shows "Idle" / "Scanning..." / "Found N devices" in HA
        cv.Optional(CONF_SCAN_STATUS): text_sensor.text_sensor_schema(),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_pins(config[CONF_TX_PIN], config[CONF_RX_PIN]))

    if CONF_SCAN_STATUS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_SCAN_STATUS])
        cg.add(var.set_scan_status_sensor(sens))

    # Include path for the protocol stack headers is handled by CMakeLists.txt
    # (INCLUDE_DIRS "${DALI_IDF_DIR}"). No Python-level flag needed.

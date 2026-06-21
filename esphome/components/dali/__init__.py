from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_ID
from esphome.core import CORE

CODEOWNERS = ["@peetervalk"]
# esp32 dependency ensures we only build for ESP32 IDF targets
DEPENDENCIES = ["esp32"]

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
        cv.Optional(CONF_SCAN_STATUS): text_sensor.TEXT_SENSOR_SCHEMA.extend(
            {cv.GenerateID(): cv.declare_id(text_sensor.TextSensor)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_pins(config[CONF_TX_PIN], config[CONF_RX_PIN]))

    if CONF_SCAN_STATUS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_SCAN_STATUS])
        cg.add(var.set_scan_status_sensor(sens))

    # Add the protocol stack header directory to the compiler include path.
    # The .c sources themselves are compiled via CMakeLists.txt file(GLOB).
    # Use POSIX path (forward slashes) so the flag works on both Linux and
    # Windows ESP-IDF toolchains.
    dali_idf_headers = Path(__file__).parent.parent.parent.parent / "components"
    cg.add_global_arg(f"-I{dali_idf_headers.as_posix()}")

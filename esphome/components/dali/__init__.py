from pathlib import Path
import shutil

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_ID
from esphome.core import CORE

CODEOWNERS = ["@peetervalk"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["text_sensor"]

CONF_TX_PIN          = "tx_pin"
CONF_RX_PIN          = "rx_pin"
CONF_SCAN_STATUS     = "scan_status"
CONF_SCAN_RESULT     = "scan_result"
CONF_YAML_RESULT     = "yaml_result"
CONF_COUPLERS_RESULT = "couplers_result"
CONF_BUS_MONITOR       = "bus_monitor"
CONF_COMMAND_RESULT    = "command_result"
CONF_BUS_FAULT         = "bus_fault"
CONF_POLL_INTERVAL     = "poll_interval"
CONF_HEADLESS_DISPATCH = "headless_dispatch"
CONF_EVENT_INFORMATION = "event_information"
CONF_EVENT_CODE        = "event_code"

dali_ns = cg.esphome_ns.namespace("dali")
DaliComponent = dali_ns.class_("DaliComponent", cg.Component)

# ── Dispatch entry enum mappings ──────────────────────────────────────────────
# Values must match the C enums in dali_event.h, dali_frame.h, dali_dispatch.h.

_FRAME_KIND = {
    "legacy_16bit": 1,   # DALI_EVENT_FRAME_LEGACY_16BIT
    "input_24bit":  2,   # DALI_EVENT_FRAME_INPUT_24BIT
}

_ADDRESS_KIND = {
    "none":      0,   # DALI_EVENT_ADDRESS_INVALID (Instance source scheme)
    "invalid":   0,   # explicit alias for the C enum name
    "short":     1,   # DALI_EVENT_ADDRESS_SHORT
    "group":     2,   # DALI_EVENT_ADDRESS_GROUP
    "broadcast": 3,   # DALI_EVENT_ADDRESS_BROADCAST
}

_OUTPUT_TYPE = {
    "short":     0,   # DALI_ADDR_SHORT
    "group":     1,   # DALI_ADDR_GROUP
    "broadcast": 2,   # DALI_ADDR_BROADCAST
}

_DISPATCH_ACTION = {
    "observe":    0,   # DALI_DISPATCH_ACTION_OBSERVE
    "mirror":     1,   # DALI_DISPATCH_ACTION_MIRROR
    "recall_max": 2,   # DALI_DISPATCH_ACTION_RECALL_MAX
    "recall_min": 3,   # DALI_DISPATCH_ACTION_RECALL_MIN
    "off":        4,   # DALI_DISPATCH_ACTION_OFF
    "go_to_last": 5,   # DALI_DISPATCH_ACTION_GO_TO_LAST
    "dim_up":     6,   # DALI_DISPATCH_ACTION_DIM_UP
    "dim_down":   7,   # DALI_DISPATCH_ACTION_DIM_DOWN
    "scene":      8,   # DALI_DISPATCH_ACTION_SCENE
    "toggle":     9,   # DALI_DISPATCH_ACTION_TOGGLE
}

_EVENT_ANY = 0xFFFF  # DALI_DISPATCH_EVENT_ANY
_INSTANCE_ANY = 0xFF  # DALI_DISPATCH_INSTANCE_ANY


def _any_or_event_information(value):
    """Accept 'any' or the full 10-bit Part-103 event information."""
    if isinstance(value, str) and value.lower() == "any":
        return _EVENT_ANY
    return cv.int_range(min=0, max=0x3FF)(value)


def _any_or_event_code(value):
    """Accept the legacy uint8 event-code range and its ANY sentinel."""
    if isinstance(value, str) and value.lower() == "any":
        return 0xFF
    return cv.int_range(min=0, max=0xFF)(value)


def _any_or_instance(value):
    """Accept 'any' or a Part-103 instance number."""
    if isinstance(value, str) and value.lower() == "any":
        return _INSTANCE_ANY
    value = cv.int_(value)
    if value == _INSTANCE_ANY:
        return value
    return cv.int_range(min=0, max=31)(value)


def _normalize_dispatch_event_information(config):
    """Keep event_code as a compatibility alias for event_information."""
    has_information = CONF_EVENT_INFORMATION in config
    has_code = CONF_EVENT_CODE in config
    if has_information and has_code:
        raise cv.Invalid(
            f"Specify only one of {CONF_EVENT_INFORMATION} or {CONF_EVENT_CODE}"
        )
    if has_code:
        legacy_code = config.pop(CONF_EVENT_CODE)
        # The old uint8 field used 0xFF as its ANY sentinel. Preserve that
        # meaning; use event_information when an exact value 255 is required.
        config[CONF_EVENT_INFORMATION] = (
            _EVENT_ANY if legacy_code == 0xFF else legacy_code
        )
    elif not has_information:
        config[CONF_EVENT_INFORMATION] = _EVENT_ANY
    return config


def _validate_dispatch_entry(config):
    """Reject dispatch keys/outputs that cannot match a valid wire frame."""
    frame_kind = config["frame_kind"]
    address_kind = config["address_kind"]
    address = config["address"]
    event_information = config[CONF_EVENT_INFORMATION]
    instance = config["instance"]

    if frame_kind == "legacy_16bit":
        if address_kind in ("none", "invalid"):
            raise cv.Invalid("legacy_16bit requires short, group, or broadcast")
        if address_kind == "group" and address > 15:
            raise cv.Invalid("legacy_16bit group address must be 0..15")
        if address_kind == "broadcast" and address != 0:
            raise cv.Invalid("broadcast dispatch address must be 0")
        if event_information != _EVENT_ANY and event_information > 0xFF:
            raise cv.Invalid("legacy_16bit event_information must be 0..255 or any")
        if instance != _INSTANCE_ANY:
            raise cv.Invalid("legacy_16bit does not carry an instance; use any")
    else:
        if address_kind == "broadcast":
            raise cv.Invalid("input_24bit events do not use a broadcast source")
        if address_kind == "group" and address > 31:
            raise cv.Invalid("Part-103 source group must be 0..31")
        if address_kind in ("none", "invalid") and address != 0:
            raise cv.Invalid("an Instance source has no address; use address: 0")
        if address_kind == "group" and instance != _INSTANCE_ANY:
            raise cv.Invalid("Part-103 group sources do not carry an instance number")
        if config["action"] in ("observe", "mirror"):
            raise cv.Invalid("observe and mirror are only valid for legacy_16bit")

    if config["output_type"] == "group" and config["output_address"] > 15:
        raise cv.Invalid("DALI output group address must be 0..15")
    if config["output_type"] == "broadcast" and config["output_address"] != 0:
        raise cv.Invalid("broadcast output_address must be 0")
    return config


_DISPATCH_ENTRY_SCHEMA = cv.All(
    cv.Schema({
        cv.Required("frame_kind"):    cv.one_of(*_FRAME_KIND,   lower=True),
        cv.Required("address_kind"):  cv.one_of(*_ADDRESS_KIND, lower=True),
        cv.Optional("address", default=0):     cv.int_range(min=0, max=63),
        cv.Optional(CONF_EVENT_INFORMATION): _any_or_event_information,
        cv.Optional(CONF_EVENT_CODE): _any_or_event_code,
        cv.Optional("instance", default="any"): _any_or_instance,
        cv.Required("output_type"):   cv.one_of(*_OUTPUT_TYPE,  lower=True),
        cv.Optional("output_address", default=0): cv.int_range(min=0, max=63),
        cv.Required("action"):        cv.one_of(*_DISPATCH_ACTION, lower=True),
        cv.Optional("scene", default=0): cv.int_range(min=0, max=15),
    }),
    _normalize_dispatch_event_information,
    _validate_dispatch_entry,
)


# ── Protocol stack vendoring ──────────────────────────────────────────────────
#
# The DALI stack is plain C99 in the repo-root components/dali/ IDF component,
# shared with the native app in main/ and the host tests in test/. ESPHome
# copies only THIS directory into its build tree, so those sources would never
# reach the compiler on their own. _copy_protocol_stack() vendors them into
# <build>/src/components/dali/ at codegen time, and the proto_dali_*.c shims
# here pull each one into a translation unit the build does compile.
#
# Each .c is vendored as .c.inc deliberately: ESPHome generates a src
# CMakeLists doing FILE(GLOB_RECURSE app_sources src/*.*), which sweeps the
# vendored directory too. Under a .c name every module would compile twice —
# once via the glob, once via its shim — and every non-static symbol would
# collide at link. CMake skips the unknown .inc extension, leaving the shims
# as the only entry point; one shim per module keeps one translation unit per
# module, so static helpers keep file scope.

def _protocol_source_dir():
    component_dir = Path(__file__).resolve().parent
    candidates = [
        component_dir.parents[2] / "components" / "dali",
        component_dir / "protocol",
    ]
    for candidate in candidates:
        if (candidate / "dali_phy.h").is_file() and (candidate / "dali_phy.c").is_file():
            return candidate
    raise cv.Invalid(
        "DALI protocol sources not found. Expected repo-root components/dali "
        "next to esphome/components/dali."
    )


def _copy_protocol_stack():
    src_dir = _protocol_source_dir()
    dst_dir = CORE.relative_src_path("components", "dali")
    dst_dir.mkdir(parents=True, exist_ok=True)

    for stale in list(dst_dir.glob("dali_*.h")) + list(dst_dir.glob("dali_*.c.inc")):
        stale.unlink()

    for header in src_dir.glob("dali_*.h"):
        shutil.copyfile(header, dst_dir / header.name)
    for source in src_dir.glob("dali_*.c"):
        # .inc so ESPHome's src/*.* glob cannot compile these behind the shims.
        shutil.copyfile(source, dst_dir / f"{source.name}.inc")

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DaliComponent),
        cv.Required(CONF_TX_PIN): cv.int_range(min=0, max=39),
        cv.Required(CONF_RX_PIN): cv.int_range(min=0, max=39),
        # Scan-status: "Idle" / "Scanning..." / "Found N devices"
        cv.Optional(CONF_SCAN_STATUS): text_sensor.text_sensor_schema(),
        # Scan-result: compact last-scan summary persisted in HA
        cv.Optional(CONF_SCAN_RESULT): text_sensor.text_sensor_schema(),
        # Yaml-result: full ESPHome light: YAML snippet from last scan
        cv.Optional(CONF_YAML_RESULT): text_sensor.text_sensor_schema(),
        # Couplers-result: unique frames seen during find_couplers window
        cv.Optional(CONF_COUPLERS_RESULT): text_sensor.text_sensor_schema(),
        # Bus-monitor: last unsolicited frame (live)
        cv.Optional(CONF_BUS_MONITOR): text_sensor.text_sensor_schema(),
        # Command-result: output of the last text command entity execution
        cv.Optional(CONF_COMMAND_RESULT): text_sensor.text_sensor_schema(),
        # Bus-fault: "OK" at boot; "Bus stuck: N" when bus idle timeout fires
        cv.Optional(CONF_BUS_FAULT): text_sensor.text_sensor_schema(),
        # Optional periodic QUERY_ACTUAL_LEVEL poll in seconds (0 = disabled)
        cv.Optional(CONF_POLL_INTERVAL, default=0): cv.int_range(min=0, max=3600),
        # Autonomous bus event dispatch — list of entries, each defining one
        # frame-match → action rule. Absent or empty = no dispatch.
        cv.Optional(CONF_HEADLESS_DISPATCH, default=[]): cv.ensure_list(
            _DISPATCH_ENTRY_SCHEMA
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    _copy_protocol_stack()

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_pins(config[CONF_TX_PIN], config[CONF_RX_PIN]))

    if CONF_SCAN_STATUS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_SCAN_STATUS])
        cg.add(var.set_scan_status_sensor(sens))

    if CONF_SCAN_RESULT in config:
        sens = await text_sensor.new_text_sensor(config[CONF_SCAN_RESULT])
        cg.add(var.set_scan_result_sensor(sens))

    if CONF_YAML_RESULT in config:
        sens = await text_sensor.new_text_sensor(config[CONF_YAML_RESULT])
        cg.add(var.set_yaml_result_sensor(sens))

    if CONF_COUPLERS_RESULT in config:
        sens = await text_sensor.new_text_sensor(config[CONF_COUPLERS_RESULT])
        cg.add(var.set_couplers_result_sensor(sens))

    if CONF_BUS_MONITOR in config:
        sens = await text_sensor.new_text_sensor(config[CONF_BUS_MONITOR])
        cg.add(var.set_bus_monitor_sensor(sens))

    if CONF_COMMAND_RESULT in config:
        sens = await text_sensor.new_text_sensor(config[CONF_COMMAND_RESULT])
        cg.add(var.set_command_result_sensor(sens))

    if CONF_BUS_FAULT in config:
        sens = await text_sensor.new_text_sensor(config[CONF_BUS_FAULT])
        cg.add(var.set_bus_fault_sensor(sens))

    if config[CONF_POLL_INTERVAL] > 0:
        cg.add(var.set_poll_interval(config[CONF_POLL_INTERVAL]))

    for entry in config[CONF_HEADLESS_DISPATCH]:
        cg.add(var.add_dispatch_entry(
            _FRAME_KIND[entry["frame_kind"]],
            _ADDRESS_KIND[entry["address_kind"]],
            entry["address"],
            entry[CONF_EVENT_INFORMATION],
            entry["instance"],
            _OUTPUT_TYPE[entry["output_type"]],
            entry["output_address"],
            _DISPATCH_ACTION[entry["action"]],
            entry["scene"],
        ))

// FALLBACK unity build for the DALI protocol stack.
//
// Primary path: CMakeLists.txt uses file(GLOB) to list the protocol .c files
// with absolute paths via CMAKE_CURRENT_LIST_DIR. Use that when possible — it
// compiles the .c sources as C (not C++), which is correct and avoids any
// C99-vs-C++ edge cases.
//
// Use THIS FILE only if ESPHome generates its own CMakeLists.txt (ignoring
// ours) and the build fails with "undefined reference to dali_phy_init" etc.
// To activate: add "dali_protocol_unity.cpp" to SRCS in CMakeLists.txt
// (replacing the GLOB block), or ensure ESPHome auto-discovers it.
//
// Relative paths are resolved from this file's location in the SOURCE tree
// (esphome/components/dali/), three levels up to the project root.
// If ESPHome copies this file to a build directory rather than using it in
// place, these paths will break — use the CMakeLists.txt approach instead.

extern "C" {
#include "../../../components/dali/dali_ringbuf.c"
#include "../../../components/dali/dali_phy.c"
#include "../../../components/dali/dali_scheduler.c"
#include "../../../components/dali/dali_protocol.c"
#include "../../../components/dali/dali_control.c"
#include "../../../components/dali/dali_event.c"
#include "../../../components/dali/dali_discovery.c"
#include "../../../components/dali/dali_commissioning.c"
#include "../../../components/dali/dali_memory.c"
#include "../../../components/dali/dali_input_device.c"
#include "../../../components/dali/dali_input_poll.c"
#include "../../../components/dali/dali_input_config.c"
#include "../../../components/dali/dali_gear_dt6.c"
#include "../../../components/dali/dali_gear_dt8.c"
#include "../../../components/dali/dali_mapping.c"
#include "../../../components/dali/dali_lunatone.c"
#include "../../../components/dali/dali_steinel.c"
}

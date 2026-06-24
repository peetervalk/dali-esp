# ESPHome IDF component registration for the DALI component.
#
# The protocol stack sources live in the sibling components/dali/ IDF component.
# We compile them here using absolute paths via CMAKE_CURRENT_LIST_DIR so the
# paths remain valid even if ESPHome copies this directory to its build tree.
#
# ESPHome with the esp-idf framework picks this file up when it processes
# the external_components entry. If ESPHome generates its own CMakeLists
# instead of using ours, the build will fail with "undefined reference to
# dali_phy_init" and similar — in that case fall back to the unity build
# (see dali_protocol_unity.cpp and the README/todo_esphome_release.md).

set(DALI_IDF_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../components/dali")
# CONFIGURE_DEPENDS: re-run CMake automatically when .c files are added/removed.
file(GLOB DALI_PROTOCOL_SRCS CONFIGURE_DEPENDS "${DALI_IDF_DIR}/dali_*.c")

idf_component_register(
    SRCS
        "dali_component.cpp"
        "dali_scan.cpp"
        "dali_headless.cpp"
        "light/dali_light_output.cpp"
        # Explicit entry guards against stale glob cache when files are added
        # after the initial CMake configure. CMake deduplicates if GLOB catches
        # it too.
        "${DALI_IDF_DIR}/dali_dispatch.c"
        ${DALI_PROTOCOL_SRCS}
    INCLUDE_DIRS
        "."
        "${DALI_IDF_DIR}"
    REQUIRES
        driver
        esp_timer
        freertos
        esp_hw_support
)

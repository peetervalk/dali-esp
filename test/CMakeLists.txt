cmake_minimum_required(VERSION 3.16)
project(dali_host_tests C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Treat warnings as errors; keep things clean
add_compile_options(-Wall -Wextra -Werror -DDALI_HOST_BUILD=1)

# ---------------------------------------------------------------------------
# Unity test framework (vendored single-file)
# ---------------------------------------------------------------------------
set(UNITY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/unity)

add_library(unity STATIC ${UNITY_DIR}/unity.c)
target_include_directories(unity PUBLIC ${UNITY_DIR})

# ---------------------------------------------------------------------------
# DALI component source files that are host-portable
# ---------------------------------------------------------------------------
set(DALI_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_ringbuf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_phy.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_protocol.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_transport.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_input_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_input_poll.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_event.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_discovery.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_commissioning.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_lunatone.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_steinel.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_mapping.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_memory.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_gear_dt6.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_gear_dt8.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_input_config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_control.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_dim_curve.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_scheduler.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_group_map.c
)

set(DALI_INC
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali
)

# The portable half of the native CLI. It lives in components/dali so that both
# the native app and the ESPHome console can reach it; only the tests that need
# it link it, which keeps the other suites' link lines small. dali_diag.c stays
# out: it owns the FreeRTOS task and the blocking scheduler slots, which have no
# host form.
set(DALI_CLI_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_cli.c
)

set(DALI_CLI_INC
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali
)

# ---------------------------------------------------------------------------
# Helper macro to define a test executable
# ---------------------------------------------------------------------------
# powf()/log10f() in the dimming curve need libm on Unix toolchains. The Windows
# CRTs fold those into the default libraries and ship no libm to find, so an
# empty result there is correct rather than a failure.
find_library(DALI_LIBM m)
if(NOT DALI_LIBM)
    set(DALI_LIBM "")
endif()

macro(add_dali_test name src)
    add_executable(${name} ${src} ${DALI_SRC})
    target_include_directories(${name} PRIVATE ${DALI_INC})
    target_link_libraries(${name} PRIVATE unity ${DALI_LIBM})
    add_test(NAME ${name} COMMAND ${name})
endmacro()

macro(add_dali_cli_test name src)
    add_executable(${name} ${src} ${DALI_SRC} ${DALI_CLI_SRC})
    target_include_directories(${name} PRIVATE ${DALI_INC} ${DALI_CLI_INC})
    target_link_libraries(${name} PRIVATE unity ${DALI_LIBM})
    add_test(NAME ${name} COMMAND ${name})
endmacro()

enable_testing()

add_dali_test(test_ringbuf    test_ringbuf.c)
add_dali_test(test_phy_encode test_phy_encode.c)
add_dali_test(test_phy_decode test_phy_decode.c)
add_dali_test(test_protocol   test_protocol.c)
add_dali_test(test_input_device test_input_device.c)
add_dali_test(test_transport  test_transport.c)
add_dali_test(test_input_poll test_input_poll.c)
add_dali_test(test_event test_event.c)
add_dali_test(test_discovery test_discovery.c)
add_dali_test(test_commissioning test_commissioning.c)
add_dali_test(test_vendor     test_vendor.c)
add_dali_test(test_mapping    test_mapping.c)
add_dali_test(test_memory     test_memory.c)
add_dali_test(test_gear_dt6    test_gear_dt6.c)
add_dali_test(test_gear_dt8    test_gear_dt8.c)
add_dali_test(test_input_config test_input_config.c)
add_dali_test(test_control    test_control.c)
add_dali_test(test_scheduler  test_scheduler.c)
add_dali_test(test_dispatch   test_dispatch.c)
add_dali_test(test_group_map  test_group_map.c)
add_dali_test(test_dim_curve  test_dim_curve.c)
add_dali_cli_test(test_cli    test_cli.c)

add_executable(test_light_state_mailbox test_light_state_mailbox.cpp)
target_link_libraries(test_light_state_mailbox PRIVATE unity)
add_test(NAME test_light_state_mailbox COMMAND test_light_state_mailbox)

add_executable(test_refresh_cursor test_refresh_cursor.cpp)
target_link_libraries(test_refresh_cursor PRIVATE unity)
add_test(NAME test_refresh_cursor COMMAND test_refresh_cursor)

add_executable(test_light_write test_light_write.cpp)
target_link_libraries(test_light_write PRIVATE unity)
add_test(NAME test_light_write COMMAND test_light_write)

add_executable(test_light_profile test_light_profile.cpp ${DALI_SRC})
target_include_directories(test_light_profile PRIVATE ${DALI_INC})
target_link_libraries(test_light_profile PRIVATE unity ${DALI_LIBM})
add_test(NAME test_light_profile COMMAND test_light_profile)

cmake_minimum_required(VERSION 3.16)
project(dali_host_tests C)

set(CMAKE_C_STANDARD 11)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali/dali_scheduler.c
)

set(DALI_INC
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dali
)

# ---------------------------------------------------------------------------
# Helper macro to define a test executable
# ---------------------------------------------------------------------------
macro(add_dali_test name src)
    add_executable(${name} ${src} ${DALI_SRC})
    target_include_directories(${name} PRIVATE ${DALI_INC})
    target_link_libraries(${name} PRIVATE unity)
    add_test(NAME ${name} COMMAND ${name})
endmacro()

enable_testing()

add_dali_test(test_ringbuf    test_ringbuf.c)
add_dali_test(test_phy_encode test_phy_encode.c)
add_dali_test(test_phy_decode test_phy_decode.c)
add_dali_test(test_protocol   test_protocol.c)
add_dali_test(test_input_device test_input_device.c)
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

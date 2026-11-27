if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(airgradient_provisioning_tests CXX C)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)

    get_filename_component(AIRGRADIENT_REPO_ROOT
                           "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
                           ABSOLUTE)

    include(FetchContent)

    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_Declare(
        trompeloeil
        GIT_REPOSITORY https://github.com/rollbear/trompeloeil.git
        GIT_TAG        v47
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2 trompeloeil)

    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(CTest)
    include(Catch)
endif()

# cJSON for host tests (firmware uses ESP-IDF's bundled copy via the
# `json` managed component).
if(NOT TARGET cjson_host)
    include(FetchContent)
    FetchContent_Declare(
        cjson
        GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
        GIT_TAG        v1.7.18
        GIT_SHALLOW    TRUE
    )
    # Avoid pulling in cJSON's own tests / install rules.
    set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
    set(ENABLE_CJSON_UTILS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(CJSON_OVERRIDE_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    set(CJSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(cjson)
    # cJSON's CMake doesn't attach include dirs to the `cjson` target by
    # default — its headers live at the source-tree root.
    target_include_directories(cjson PUBLIC
        "$<BUILD_INTERFACE:${cjson_SOURCE_DIR}>")
    add_library(cjson_host ALIAS cjson)
endif()

add_library(provisioning_test_support
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-provisioning/internal/scan_filter.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-provisioning/internal/dns_packet.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-provisioning/internal/provisioning_timer.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-provisioning/internal/captive_dns_responder.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-provisioning/internal/wifi_portal_transport.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-provisioning/internal/provisioning_json.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-provisioning/internal/ble_transport.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-ble/services/ble_device_information.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-provisioning/internal/provisioning_qr.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-provisioning/vendor/qrcodegen.c"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-provisioning/services/provisioning_manager.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-wifi/services/wifi_manager.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-wifi/services/wifi_credential_store.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/rtos.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/common.cpp"
)

target_include_directories(provisioning_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-provisioning"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-provisioning/tests"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-ble"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-wifi"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-wifi/tests"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-config/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-http-server"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-http-server/tests"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/include"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-gpio/hal"
)
target_include_directories(provisioning_test_support PRIVATE
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-provisioning/vendor"
)

target_compile_definitions(provisioning_test_support PUBLIC TEST_HOST)
target_link_libraries(provisioning_test_support PUBLIC cjson)

add_executable(airgradient_provisioning_tests
    scan_filter.tests.cpp
    dns_packet.tests.cpp
    wifi_portal_transport.tests.cpp
    provisioning_json.tests.cpp
    ble_transport.tests.cpp
    provisioning_manager.tests.cpp
    provisioning_qr.tests.cpp
)

target_link_libraries(airgradient_provisioning_tests PRIVATE
    provisioning_test_support
    Catch2::Catch2WithMain
    trompeloeil::trompeloeil
)

catch_discover_tests(airgradient_provisioning_tests)

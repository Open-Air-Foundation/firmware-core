if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(airgradient_common_tests CXX)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
    FetchContent_MakeAvailable(Catch2)

    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(CTest)
    include(Catch)
endif()

add_library(airgradient_common_air_quality_test_support
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/air_quality.cpp"
)

target_include_directories(airgradient_common_air_quality_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/include"
)

target_compile_definitions(airgradient_common_air_quality_test_support PUBLIC TEST_HOST)

add_executable(airgradient_common_air_quality_tests
    air_quality.tests.cpp
)

target_link_libraries(airgradient_common_air_quality_tests PRIVATE
    airgradient_common_air_quality_test_support
    Catch2::Catch2WithMain
)

catch_discover_tests(airgradient_common_air_quality_tests)

add_library(airgradient_common_correction_test_support
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/measurement_corrections.cpp"
)

target_include_directories(airgradient_common_correction_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/include"
)

target_compile_definitions(airgradient_common_correction_test_support PUBLIC TEST_HOST)

add_executable(airgradient_common_correction_tests
    measurement_corrections.tests.cpp
)

target_link_libraries(airgradient_common_correction_tests PRIVATE
    airgradient_common_correction_test_support
    Catch2::Catch2WithMain
)

catch_discover_tests(airgradient_common_correction_tests)

add_library(airgradient_common_rtos_test_support
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/rtos.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/retained_uptime.cpp"
)

target_include_directories(airgradient_common_rtos_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/include"
)

target_compile_definitions(airgradient_common_rtos_test_support PUBLIC TEST_HOST)

add_executable(airgradient_common_rtos_tests
    rtos.tests.cpp
)

target_link_libraries(airgradient_common_rtos_tests PRIVATE
    airgradient_common_rtos_test_support
    Catch2::Catch2WithMain
)

catch_discover_tests(airgradient_common_rtos_tests)

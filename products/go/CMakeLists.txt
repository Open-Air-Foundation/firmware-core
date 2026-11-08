cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components"
)

# Explicitly list components this product needs.
# airgradient-ble and esp-nimble-cpp require CONFIG_BT_ENABLED=y and
# CONFIG_BT_NIMBLE_ENABLED=y to compile (set in sdkconfig.defaults when
# RUN_TEST_BLE is selected). They are included here because the reference
# product is a test bed that exercises all available hardware.
set(COMPONENTS
    main
    airgradient-ble
    airgradient-bms
    airgradient-common
    airgradient-config
    airgradient-gpio
    airgradient-nand-storage
    airgradient-ota
    airgradient-payload-cache
    airgradient-sensors
    airgradient-serial
    airgradient-touch
    airgradient-watchdog
    ads1115
)

# Release tags use go-vMAJOR.MINOR.PATCH. Development builds retain the latest
# reachable release version and add the current commit and dirty state.
set(AG_GO_VERSION_MAX_LENGTH 31)
set(AG_GO_RELEASE_VERSION_MAX_LENGTH 16)
set(AG_GO_RELEASE_TAG_PATTERN
    "^go-v((0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*))$"
)
set(AG_GO_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
find_package(Git QUIET)

if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${AG_GO_REPOSITORY_ROOT}"
        RESULT_VARIABLE git_head_result
        OUTPUT_VARIABLE git_head
        ERROR_VARIABLE git_head_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT git_head_result EQUAL 0)
        message(FATAL_ERROR "Unable to determine Go commit: ${git_head_error}")
    endif()
    string(SUBSTRING "${git_head}" 0 7 commit_hash)

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" tag --merged HEAD --list "go-v*"
        WORKING_DIRECTORY "${AG_GO_REPOSITORY_ROOT}"
        RESULT_VARIABLE git_tags_result
        OUTPUT_VARIABLE git_tags_output
        ERROR_VARIABLE git_tags_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT git_tags_result EQUAL 0)
        message(FATAL_ERROR "Unable to inspect Go release tags: ${git_tags_error}")
    endif()

    string(REPLACE "\n" ";" reachable_tags "${git_tags_output}")
    set(valid_tags "")
    foreach(tag IN LISTS reachable_tags)
        if(tag MATCHES "${AG_GO_RELEASE_TAG_PATTERN}")
            set(tag_version "${CMAKE_MATCH_1}")
            string(LENGTH "${tag_version}" tag_version_length)
            if(tag_version_length LESS_EQUAL AG_GO_RELEASE_VERSION_MAX_LENGTH)
                list(APPEND valid_tags "${tag}")
            endif()
        endif()
    endforeach()

    if(valid_tags)
        set(git_describe_command
            "${GIT_EXECUTABLE}" describe --tags --long --abbrev=7
        )
        foreach(tag IN LISTS valid_tags)
            list(APPEND git_describe_command --match "${tag}")
        endforeach()
        execute_process(
            COMMAND ${git_describe_command}
            WORKING_DIRECTORY "${AG_GO_REPOSITORY_ROOT}"
            RESULT_VARIABLE git_describe_result
            OUTPUT_VARIABLE git_description
            ERROR_VARIABLE git_describe_error
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_STRIP_TRAILING_WHITESPACE
        )
        if(NOT git_describe_result EQUAL 0)
            message(FATAL_ERROR
                    "Unable to determine Go firmware version: ${git_describe_error}")
        endif()
        if(NOT git_description MATCHES
           "^go-v((0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*))-([0-9]+)-g([0-9a-f]+)$")
            message(FATAL_ERROR "Unexpected Go git description: ${git_description}")
        endif()
        set(base_version "${CMAKE_MATCH_1}")
        set(commit_count "${CMAKE_MATCH_5}")
        string(SUBSTRING "${CMAKE_MATCH_6}" 0 7 commit_hash)
    else()
        set(base_version "0.0.0")
        set(commit_count "-1")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=normal
        WORKING_DIRECTORY "${AG_GO_REPOSITORY_ROOT}"
        RESULT_VARIABLE git_status_result
        OUTPUT_VARIABLE git_status
        ERROR_VARIABLE git_status_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT git_status_result EQUAL 0)
        message(FATAL_ERROR "Unable to inspect Go worktree: ${git_status_error}")
    endif()

    if(git_status)
        set(dirty_suffix "-dirty")
    else()
        set(dirty_suffix "")
    endif()

    if(commit_count STREQUAL "0" AND NOT dirty_suffix)
        set(PROJECT_VER "${base_version}")
    else()
        set(PROJECT_VER "${base_version}-g${commit_hash}${dirty_suffix}")
    endif()
else()
    set(PROJECT_VER "0.0.0-unknown")
endif()

string(LENGTH "${PROJECT_VER}" project_ver_length)
if(project_ver_length GREATER AG_GO_VERSION_MAX_LENGTH)
    message(FATAL_ERROR
            "Go firmware version exceeds ${AG_GO_VERSION_MAX_LENGTH} characters: ${PROJECT_VER}")
endif()
message(STATUS "Resolved Go firmware version: ${PROJECT_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(airgradient-go)

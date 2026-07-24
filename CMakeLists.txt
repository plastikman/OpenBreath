# DragonBreath — open firmware for the BIGTREETECH Panda Breath (ESP32-C3)
cmake_minimum_required(VERSION 3.16)
# The shared board-agnostic core (event log, Wi-Fi/provisioning, Moonraker client)
# is now vendored locally as components/pb_evlog, pb_wifi, pb_moonraker — auto-
# discovered from components/, so no EXTRA_COMPONENT_DIRS is needed. It derives from
# the OpenVent family (justinh-rahb/OpenVent, MIT); see VENDORING.md for provenance.
# App version stamped into the ESP app descriptor (esp_app_get_description()->version,
# also shown in the web UI): the exact git tag when HEAD is tagged (releases), else
# the short commit hash (PR / local builds), with "-dirty" if the tree is modified.
execute_process(
    COMMAND git describe --tags --exact-match
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE DB_TAG OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET RESULT_VARIABLE DB_TAG_RC)
if(DB_TAG_RC EQUAL 0 AND DB_TAG)
    set(PROJECT_VER "${DB_TAG}")
else()
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE DB_HASH OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE DB_HASH_RC)
    execute_process(
        COMMAND git status --porcelain --untracked-files=no
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE DB_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT DB_HASH_RC EQUAL 0)
        set(PROJECT_VER "unknown")
    elseif(DB_DIRTY)
        set(PROJECT_VER "${DB_HASH}-dirty")
    else()
        set(PROJECT_VER "${DB_HASH}")
    endif()
endif()
message(STATUS "DragonBreath version: ${PROJECT_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dragonbreath)

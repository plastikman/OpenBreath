# OpenBreath — open firmware for the BIGTREETECH Panda Breath (ESP32-C3)
cmake_minimum_required(VERSION 3.16)
# Shared core from the OpenVent family (git submodule, pinned). These components
# are made AVAILABLE here but are not yet linked — they build into the app only
# once `main` (or another component) REQUIRES them. Verified drop-in / no Vent
# coupling by dependency probe: pv_evlog, pv_wifi, pv_moonraker. (pv_portal is
# NOT listed — it REQUIRES pv_motor/pv_policy and needs decoupling first.)
set(EXTRA_COMPONENT_DIRS
    external/OpenVent/firmware/components/pv_evlog
    external/OpenVent/firmware/components/pv_wifi
    external/OpenVent/firmware/components/pv_moonraker)
# App version stamped into the ESP app descriptor (esp_app_get_description()->version,
# also shown in the web UI): the exact git tag when HEAD is tagged (releases), else
# the short commit hash (PR / local builds), with "-dirty" if the tree is modified.
execute_process(
    COMMAND git describe --tags --exact-match
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE OB_TAG OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET RESULT_VARIABLE OB_TAG_RC)
if(OB_TAG_RC EQUAL 0 AND OB_TAG)
    set(PROJECT_VER "${OB_TAG}")
else()
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE OB_HASH OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE OB_HASH_RC)
    execute_process(
        COMMAND git status --porcelain --untracked-files=no
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE OB_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT OB_HASH_RC EQUAL 0)
        set(PROJECT_VER "unknown")
    elseif(OB_DIRTY)
        set(PROJECT_VER "${OB_HASH}-dirty")
    else()
        set(PROJECT_VER "${OB_HASH}")
    endif()
endif()
message(STATUS "OpenBreath version: ${PROJECT_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(openbreath)

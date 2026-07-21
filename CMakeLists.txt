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
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(openbreath)

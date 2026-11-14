# ws_alpha: the firmware of the outdoor unit (solar, ESP32-C3). A separate
# ESP-IDF project inside the esp32flight repo; it shares only the ws-link
# protocol header.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/ws-link")

# The outdoor unit's firmware build number: shown at boot, sent to the display
# as fw_build. Bump it by hand for every image that goes to the outdoor unit (OTA
# relies on it changing). Without this, ESP-IDF would use the repo's git
# version, which belongs to esp32flight.
set(PROJECT_VER "4")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# build only the components main actually requires
idf_build_set_property(MINIMAL_BUILD ON)
project(ws_alpha)

# ws_alpha: the solar outdoor station (ESP32-C3). A separate ESP-IDF project
# inside the esp32flight repo; it shares only the ws-link protocol header.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/ws-link")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# build only the components main actually requires
idf_build_set_property(MINIMAL_BUILD ON)
project(ws_alpha)

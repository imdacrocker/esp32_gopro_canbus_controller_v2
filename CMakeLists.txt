# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

set(IDF_TARGET "esp32s3")

# Tell ESP-IDF to also scan components/gopro/ for sub-components
# (open_gopro_ble, open_gopro_http, gopro_wifi_rc live there).
list(APPEND EXTRA_COMPONENT_DIRS "components/gopro")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_gopro_canbus_controller_v2)

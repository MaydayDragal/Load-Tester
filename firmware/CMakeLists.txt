# ESP-IDF top-level project for the EL15 standalone controller.
#
# Builds the SAME sources as the PlatformIO build (firmware/src/*), using
# arduino-esp32 as an ESP-IDF component so the Arduino setup()/loop() entry
# points work unchanged. Build with:
#
#   idf.py set-target esp32c6
#   idf.py build flash monitor
#
# See README.md ("Build & flash → ESP-IDF") for the one-time library setup.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(el15-load-control)

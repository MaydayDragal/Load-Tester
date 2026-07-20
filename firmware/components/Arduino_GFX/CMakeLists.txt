# ESP-IDF wrapper for moononournation's "GFX Library for Arduino" (Arduino_GFX),
# which is Arduino-only and not in the component registry. Clone the library's
# contents into this directory (see firmware/components/README.md) so that
# `Arduino_GFX/src/*` exists, then this globs and builds it as a component.

file(GLOB_RECURSE GFX_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/src/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")

idf_component_register(
    SRCS ${GFX_SRCS}
    INCLUDE_DIRS "src"
    REQUIRES arduino esp_lcd driver)

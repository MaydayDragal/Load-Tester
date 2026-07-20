# ESP-IDF wrapper for h2zero's NimBLE-Arduino, which is Arduino-only and not in
# the component registry. Clone the library's contents into this directory (see
# firmware/components/README.md) so that `NimBLE-Arduino/src/*` exists, then
# this globs and builds it against the IDF NimBLE host.

file(GLOB_RECURSE NIMBLE_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/src/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")

idf_component_register(
    SRCS ${NIMBLE_SRCS}
    INCLUDE_DIRS "src"
    REQUIRES arduino bt nvs_flash)

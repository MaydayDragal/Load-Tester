# ESP-IDF wrapper for greiman's SdFat, which is Arduino-only and not in the
# component registry. Clone the library's contents into this directory (see
# firmware/components/README.md) so that `SdFat/src/*` exists, then this globs
# and builds it as a component.
#
# The two definitions mirror the PlatformIO build flags (see ../platformio.ini
# for the reasons): SPI_DRIVER_SELECT=3 selects the project's own software-SPI
# driver in sd_card.cpp, and USE_SD_CRC=1 checksums every transfer.

file(GLOB_RECURSE SDFAT_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/src/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")

idf_component_register(
    SRCS ${SDFAT_SRCS}
    INCLUDE_DIRS "src"
    REQUIRES arduino)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    SPI_DRIVER_SELECT=3
    USE_SD_CRC=1)

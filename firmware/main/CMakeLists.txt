# The 'main' component compiles the shared sources living in ../src. Keeping
# them in one place means the PlatformIO (srcDir = src) and ESP-IDF builds
# compile byte-for-byte the same code.

set(SHARED_DIR "${CMAKE_CURRENT_LIST_DIR}/../src")

idf_component_register(
    SRCS
        "${SHARED_DIR}/main.cpp"
        "${SHARED_DIR}/display.cpp"
        "${SHARED_DIR}/el15_client.cpp"
        "${SHARED_DIR}/ui.cpp"
    INCLUDE_DIRS
        "${SHARED_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/../include"
    REQUIRES
        arduino
        lvgl
        Arduino_GFX
        NimBLE-Arduino
)

# Use the checked-in include/lv_conf.h instead of menuconfig for LVGL, and make
# it visible to the LVGL component's own sources (not just to 'main').
idf_build_set_property(COMPILE_DEFINITIONS "LV_CONF_INCLUDE_SIMPLE" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_CURRENT_LIST_DIR}/../include" APPEND)

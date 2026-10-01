cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Room in LVGL's list of areas to draw again for the radar's moving air, which
# past 32 drew the whole screen; components/board keeps as many.
idf_build_set_property(COMPILE_DEFINITIONS "LV_INV_BUF_SIZE=64" APPEND)
project(smart_flexispot)

# The published libjpeg-turbo component asks for the include path of a target
# it calls idf::libjpeg-turbo, but the component manager installs it under its
# namespace, as idf::espressif__libjpeg-turbo. Given the name it asks for, it
# builds from a clean checkout without its downloaded copy being touched.
if(TARGET idf::espressif__libjpeg-turbo AND NOT TARGET idf::libjpeg-turbo)
    get_target_property(jpeg_target idf::espressif__libjpeg-turbo ALIASED_TARGET)
    add_library(idf::libjpeg-turbo ALIAS ${jpeg_target})
endif()

# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Add extra component directories (must be before include)
set(EXTRA_COMPONENT_DIRS
    #"$ENV{HOME}/projets/esp-idf/esp-usb/host/class/uac"
    "$ENV{HOME}/projets/esp-idf/esp-usb/host/class/msc"
    "$ENV{HOME}/projets/lcdgfx"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(MINIMAL_BUILD ON)

add_compile_options(-Wno-ignored-qualifiers)
project(soundboard)

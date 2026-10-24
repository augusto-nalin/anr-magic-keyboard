# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 4.1)

option(ENABLE_VERSION "Enable semantic-versioning" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/debug)

set(CMAKE_PROJECT_NAME magic_keyboard)
set(CMAKE_PROJECT_VERSION 0.1.0.0)

include(cmake/version.cmake)

include(cmake/ccache.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

project(${CMAKE_PROJECT_NAME} C CXX)

# The keyboard asks for sniff (15ms) right after connecting, so every key report waits up to 15ms
# for the next slot. Bluedroid's default link policy allows that; leaving sniff and hold out of it
# makes the controller refuse, and Bluedroid's own HID idle timer respects it too. Role switch
# (0x0001) stays allowed. "Only when idle" toggles the same policy at runtime instead (hal/bt_power).
if (CONFIG_MKB_KEYBOARD_POWER_NEVER)
    idf_component_get_property(bt_lib bt COMPONENT_LIB)
    target_compile_definitions(${bt_lib} PRIVATE "BTA_DM_LINK_POLICY_SETTINGS=0x0001")
endif ()

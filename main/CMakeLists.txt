set(src "src/main.cpp"
        "src/hal/bt_stack.cpp"
        "src/hal/bt_power.c"
        "src/hal/bt_hid_host.cpp"
        "src/hal/ble_hid_device.cpp"
        "src/hal/status_led.cpp"
        "src/keyboard/translator.cpp"
)

set(include "."
        "src"
        "${CMAKE_BINARY_DIR}/gen"
)

idf_component_register(
        SRCS "${src}"
        INCLUDE_DIRS "${include}"
        REQUIRES "bt" "esp_hid" "nvs_flash" "esp_driver_gpio" "esp_timer")

# keymap.txt becomes the translator's table, regenerated whenever it or the generator changes
idf_build_get_property(python PYTHON)
set(keymap_txt "${CMAKE_SOURCE_DIR}/keymap.txt")
set(keymap_generator "${CMAKE_SOURCE_DIR}/scripts/gen_keymap.py")
set(keymap_header "${CMAKE_BINARY_DIR}/gen/keymap_table.h")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/gen")
add_custom_command(OUTPUT "${keymap_header}"
        COMMAND "${python}" "${keymap_generator}" "${keymap_txt}" "${keymap_header}"
        DEPENDS "${keymap_txt}" "${keymap_generator}"
        COMMENT "Generating keymap_table.h from keymap.txt"
        VERBATIM)
add_custom_target(keymap DEPENDS "${keymap_header}")
add_dependencies(${COMPONENT_LIB} keymap)

# bt_power.c is the one file reaching into Bluedroid internals, only it gets the private headers
idf_component_get_property(bt_dir bt COMPONENT_DIR)
idf_component_get_property(bt_private_includes bt PRIV_INCLUDE_DIRS)
list(TRANSFORM bt_private_includes PREPEND "${bt_dir}/")
set_source_files_properties(src/hal/bt_power.c
        TARGET_DIRECTORY ${COMPONENT_LIB}
        PROPERTIES INCLUDE_DIRECTORIES "${bt_private_includes}")

include(${CMAKE_SOURCE_DIR}/cmake/magic-keyboard-configs.cmake)

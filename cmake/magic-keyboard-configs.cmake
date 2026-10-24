message(STATUS "─────────────────────────────────")
message(STATUS "      MAGIC KEYBOARD CONFIGS     ")

if (NOT CONFIG_BT_ENABLED OR NOT CONFIG_BT_CLASSIC_ENABLED OR NOT CONFIG_BT_HID_HOST_ENABLED)
    message(WARNING "Classic BT HID Host must be enabled. Run 'idf.py menuconfig' → Component config → Bluetooth
   → Enable, select Bluedroid as host, enable Classic Bluetooth and Classic BT HID Host.")
endif ()

if (CONFIG_MKB_DISABLE_OPTIMIZATIONS)
    message(STATUS " * Optimizations disabled")
    target_compile_options(${COMPONENT_LIB} PRIVATE -O0)
endif ()

if (CONFIG_MKB_ADDITIONAL_WARNINGS)
    message(STATUS " * Additional warnings enabled")

    # -Wdangling-reference only exists for C++, and main has C sources too (hal/bt_power.c)
    target_compile_options(${COMPONENT_LIB} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-Wdangling-reference>
            -Wdangling-pointer=2
            -Wnull-dereference
            -Wcast-qual
            -Wcast-align=strict
            -Wdouble-promotion
            -Wstack-usage=4096
    )
endif ()

if (CONFIG_MKB_WARNINGS_AS_ERRORS)
    message(STATUS " * Warnings as errors enabled")
    target_compile_options(${COMPONENT_LIB} PRIVATE -Werror)
endif ()

message(STATUS "─────────────────────────────────")

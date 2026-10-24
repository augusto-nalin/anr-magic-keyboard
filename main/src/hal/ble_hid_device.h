#pragma once
#include <cstdint>
#include <functional>

#include "keyboard/translator.h"

namespace magic_keyboard::hal::ble {
    // BLE HID keyboard + consumer control that the PC pairs with, battery service included
    class hid_device {
    public:
        using leds_handler = std::function<void(uint8_t leds)>;

        hid_device() = default;

        // Bluedroid callbacks are global, only one device can exist
        hid_device(const hid_device&) = delete;

        hid_device& operator =(const hid_device&) = delete;

        // Needs bt::start_stack() first. Advertises until the PC connects, and again after it
        // disconnects. on_leds gets the keyboard LED output report (Caps Lock and friends).
        void start(leds_handler on_leds);

        // Only what changed since the last call goes out. Dropped while the PC isn't connected.
        void send(const keyboard::pc_report& report);

        void set_battery(uint8_t percent);

        bool is_connected() const;

        // Deletes the PC's saved report subscriptions, so a PC has to subscribe again from scratch
        void forget();
    };
}

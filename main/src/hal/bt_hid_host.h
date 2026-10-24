#pragma once
#include <cstdint>
#include <functional>
#include <span>

namespace magic_keyboard::hal::bt {
    // Classic Bluetooth HID host. Finds and pairs with the Magic Keyboard, reconnects to it once
    // bonded, and hands every input report it sends to the report handler.
    class hid_host {
    public:
        using report_handler = std::function<void(uint16_t report_id, std::span<const uint8_t> data)>;

        hid_host() = default;

        // Bluedroid callbacks are global, only one host can exist
        hid_host(const hid_host&) = delete;

        hid_host& operator =(const hid_host&) = delete;

        // Needs start_stack() first. Reconnects to the bonded keyboard, or scans for one to pair
        // with when there's none. on_report runs on the HID host event task.
        void start(report_handler on_report);

        // Removes every bond, so after a restart the bridge scans and pairs again
        void forget();

        // Keyboard LEDs (output report 1, Caps Lock is bit 1). Dropped while disconnected.
        void set_leds(uint8_t leds);

        bool is_connected() const;
    };
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <optional>
#include <thread>
#include <vector>

#include <driver/gpio.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs_flash.h>

#include "version.h"
#include "hal/ble_hid_device.h"
#include "hal/bt_hid_host.h"
#include "hal/bt_stack.h"
#include "hal/status_led.h"
#include "keyboard/translator.h"
#include "utils/log.h"

using namespace std::literals::chrono_literals;

namespace magic_keyboard {
    // BOOT button, held for FORGET_HOLD while running to forget the paired keyboard. Held through a
    // reset it can't work: GPIO0 low at reset starts the ROM download mode instead of the app.
    constexpr gpio_num_t FORGET_BUTTON{GPIO_NUM_0};
    constexpr auto FORGET_HOLD{3s};
    constexpr auto FORGET_POLL{100ms};

    // The board's LED, active high: solid with keyboard and PC connected, blinking otherwise
    constexpr gpio_num_t STATUS_LED{GPIO_NUM_2};

    // HID usage
    constexpr uint8_t CAPS_LOCK{0x39};

    // Keyboard LED output report bits
    constexpr uint8_t NUM_LOCK_LED{0x01};
    constexpr uint8_t CAPS_LOCK_LED{0x02};

    void init_nvs() {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        ESP_ERROR_CHECK(err);
    }

    void init_forget_button() {
        gpio_config_t config{};
        config.pin_bit_mask = 1ULL << FORGET_BUTTON;
        config.mode         = GPIO_MODE_INPUT;
        config.pull_up_en   = GPIO_PULLUP_ENABLE;
        ESP_ERROR_CHECK(gpio_config(&config));
    }

    keyboard::options configured_options() {
        keyboard::options options{};
#ifndef CONFIG_MKB_MEDIA_KEYS
        options.media_keys = false;
#endif
        return options;
    }

    extern "C" [[noreturn]] void app_main(void) {
        LOG_INFO("Magic Keyboard %s", magic_keyboard::gen::version::semantic);

        init_nvs();
        init_forget_button();
        hal::status_led led{STATUS_LED};

        hal::bt::start_stack();

        hal::ble::hid_device pc{};
        hal::bt::hid_host keyboard_host{};
        keyboard::translator translator{configured_options()};
        std::atomic<uint8_t> keyboard_battery{0};

        // The LEDs Windows last wrote and the media lock, which owns the Caps Lock light when
        // keymap.txt has a toggle_media key
        std::atomic<uint8_t> pc_leds{0};
        std::atomic<bool> media_keys{translator.is_media_keys()};
        const auto update_keyboard_leds = [&] {
            if (!keyboard::translator::toggles_media()) {
                keyboard_host.set_leds(pc_leds);
                return;
            }
            const uint8_t caps = media_keys ? CAPS_LOCK_LED : 0;
            keyboard_host.set_leds(static_cast<uint8_t>((pc_leds & ~CAPS_LOCK_LED) | caps));
        };

#ifdef CONFIG_MKB_LATENCY_MARKERS
        // Caps Lock round trip, all on our own clock: the report comes in, Windows toggles Caps Lock
        // and writes the LED state back. Half of it is about one way through BLE and Windows.
        std::atomic<int64_t> caps_pressed_at{0};
        std::atomic<int64_t> last_round_trip{0};
        bool caps_down{false};

        // The same round trip without the keyboard: the bridge presses Caps Lock itself, an even
        // number of times so it ends where it started. Run with the keyboard connected and off,
        // the difference is what sharing the radio with the keyboard costs the PC side.
        const auto pc_round_trip_test = [&] {
            constexpr int TOGGLES{40};
            LOG_WARN("pc round trip test: %d Caps Lock toggles, don't type", TOGGLES);

            std::vector<int64_t> samples{};
            for (int i = 0; i < TOGGLES; ++i) {
                // Random spacing, so the presses don't lock onto the connection events
                std::this_thread::sleep_for(std::chrono::milliseconds(250 + esp_random() % 250));

                last_round_trip = 0;
                keyboard::pc_report press{};
                press.keys[0]   = CAPS_LOCK;
                caps_pressed_at = esp_timer_get_time();
                pc.send(press);
                pc.send(keyboard::pc_report{});

                for (int waited = 0; waited < 200 && last_round_trip == 0; ++waited) {
                    std::this_thread::sleep_for(1ms);
                }
                if (const int64_t round_trip = last_round_trip; round_trip != 0) {
                    samples.push_back(round_trip);
                }
            }

            if (samples.empty()) {
                LOG_WARN("pc round trip test: no LED reports came back");
                return;
            }

            std::ranges::sort(samples);
            int64_t sum{0};
            for (const int64_t sample: samples) {
                sum += sample;
            }
            LOG_WARN("pc round trip test (%s): %zu/%d, min %" PRId64 " median %" PRId64 " mean %" PRId64
                     " p90 %" PRId64 " max %" PRId64 " us",
                     keyboard_host.is_connected() ? "keyboard connected" : "keyboard off",
                     samples.size(), TOGGLES,
                     samples.front(), samples[samples.size() / 2], sum / static_cast<int64_t>(samples.size()),
                     samples[(samples.size() * 9 + 9) / 10 - 1], samples.back());
        };
#endif

        pc.start([&](uint8_t leds) {
#ifdef CONFIG_MKB_LATENCY_MARKERS
            if (const int64_t pressed_at = caps_pressed_at.exchange(0); pressed_at != 0) {
                const int64_t round_trip = esp_timer_get_time() - pressed_at;
                last_round_trip          = round_trip;
                LOG_INFO("caps lock round trip %" PRId64 " us, one way ~%" PRId64 " us", round_trip, round_trip / 2);
            }
#endif
            pc_leds = leds;
            update_keyboard_leds();
        });

        keyboard_host.start([&](uint16_t report_id, std::span<const uint8_t> data) {
            if (report_id == keyboard::KEYS_REPORT_ID) {
#ifdef CONFIG_MKB_LATENCY_MARKERS
                // First thing, so tools/latency measures from the moment the report got here
                std::fputs("@K\n", stdout);
                std::fflush(stdout);
#endif
                if (auto keys = keyboard::apple_keys::parse(data)) {
#ifdef CONFIG_MKB_LATENCY_MARKERS
                    const int64_t arrived_at = esp_timer_get_time();
                    const bool caps          = std::ranges::find(keys->keys, CAPS_LOCK) != keys->keys.end();
                    if (caps && !caps_down) {
                        caps_pressed_at = arrived_at;
                    }
                    caps_down = caps;
#endif
                    translator.set_num_lock((pc_leds & NUM_LOCK_LED) != 0);
                    for (const auto& report: translator.translate(*keys)) {
                        pc.send(report);
                    }
                    if (media_keys != translator.is_media_keys()) {
                        media_keys = translator.is_media_keys();
                        LOG_INFO("F1-F12 are %s", media_keys ? "media keys" : "function keys");
                        update_keyboard_leds();
                    }
#ifdef CONFIG_MKB_LATENCY_MARKERS
                    // Translating plus handing the report to Bluedroid, which returns once it's queued
                    LOG_INFO("report through the bridge in %" PRId64 " us", esp_timer_get_time() - arrived_at);
#endif
                }
                return;
            }

            if (report_id == keyboard::BATTERY_REPORT_ID) {
                if (auto battery = keyboard::apple_battery::parse(data)) {
                    LOG_INFO("keyboard battery %u%%%s", battery->percent, battery->charging ? ", charging" : "");
                    keyboard_battery = battery->percent;
                    pc.set_battery(battery->percent);
                }
            }
        });

        std::chrono::milliseconds held{0};
        std::optional<bool> keyboard_was_connected{};
        std::optional<bool> pc_was_connected{};
#ifdef CONFIG_MKB_LATENCY_MARKERS
        constexpr auto ROUND_TRIP_TEST_DELAY{15s};
        std::optional<bool> tested_with_keyboard{};
        bool settled_with_keyboard{false};
        std::chrono::milliseconds settled{0};
#endif
        while (true) {
            std::this_thread::sleep_for(FORGET_POLL);

            const bool keyboard_connected = keyboard_host.is_connected();
            led.set_linked(keyboard_connected && pc.is_connected());

            // Windows has no "keyboard gone" of its own to show, so the battery drops to 0% while
            // the keyboard is away, and the last real level comes back with it. The keyboard
            // comes back with its LEDs off, so they're set again too.
            // Both are sent again when the PC connects too, or one set while it was away is lost
            // and Windows reads the default 100%.
            const bool pc_connected = pc.is_connected();
            if (keyboard_was_connected != keyboard_connected || pc_was_connected != pc_connected) {
                if (keyboard_connected && keyboard_was_connected != keyboard_connected) {
                    update_keyboard_leds();
                }
                keyboard_was_connected = keyboard_connected;
                pc_was_connected       = pc_connected;
                if (!keyboard_connected) {
                    pc.set_battery(0);
                } else if (const uint8_t percent = keyboard_battery; percent != 0) {
                    pc.set_battery(percent);
                }
            }

            const bool pressed = gpio_get_level(FORGET_BUTTON) == 0;
#ifdef CONFIG_MKB_LATENCY_MARKERS
            // The PC side round trip test runs by itself a while after the keyboard connects and
            // again after it disconnects, so switching it off gives the comparison. Not on a
            // button: pressing one on this board tends to cut its power through the USB connector.
            if (keyboard_connected != tested_with_keyboard) {
                settled = keyboard_connected == settled_with_keyboard ? settled + FORGET_POLL : 0ms;
                settled_with_keyboard = keyboard_connected;
                if (settled >= ROUND_TRIP_TEST_DELAY && pc.is_connected()) {
                    tested_with_keyboard = keyboard_connected;
                    pc_round_trip_test();
                }
            }
#endif
            held = pressed ? held + FORGET_POLL : 0ms;

            if (held >= FORGET_HOLD) {
                LOG_WARN("BOOT held, forgetting the keyboard and the pc's subscriptions, restarting to pair again");
                keyboard_host.forget();
                pc.forget();
                std::this_thread::sleep_for(500ms);
                esp_restart();
            }
        }
    }
}

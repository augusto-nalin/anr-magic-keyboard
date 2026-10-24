#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "keyboard/keymap.h"

namespace magic_keyboard::keyboard {
    // Magic Keyboard input report IDs
    constexpr uint16_t KEYS_REPORT_ID{0x01};
    constexpr uint16_t BATTERY_REPORT_ID{0x90};

    // Report 1: modifiers, reserved, 6 keys, flags (Globe/Fn is bit 1, Lock is bit 3)
    struct apple_keys {
        uint8_t modifiers{};
        std::array<uint8_t, 6> keys{};
        bool fn{false};
        bool lock{false};

        static std::optional<apple_keys> parse(std::span<const uint8_t> report);
    };

    // Report 0x90: flags (charging is bit 1), percentage
    struct apple_battery {
        uint8_t percent{};
        bool charging{false};

        static std::optional<apple_battery> parse(std::span<const uint8_t> report);
    };

    // What the PC gets: a boot keyboard report, one consumer control usage and the system
    // control bits (power, sleep, wake)
    struct pc_report {
        uint8_t modifiers{};
        std::array<uint8_t, 6> keys{};
        uint16_t consumer{};
        uint8_t system{};

        bool operator ==(const pc_report&) const = default;
    };

    struct options {
        // F1-F12 start as media keys, Caps Lock (toggle_media in keymap.txt) flips them
        bool media_keys{true};
    };

    // Turns Magic Keyboard reports into what Windows understands, following keymap.txt. Each key
    // is decided when it goes down and keeps that until it comes up, so letting go of a modifier
    // first doesn't turn what it sent into another key. One instance per keyboard.
    class translator {
    private:
        struct held_key {
            uint8_t input;
            uint8_t key;            // what the PC gets, 0 for nothing
            uint8_t modifiers;
            uint16_t consumer;
            uint8_t system;
            bool passthrough;       // no rule, the key as it is
        };

        bool media_keys;
        bool num_lock{false};
        bool was_fn{false};
        bool globe_alone{false};

        std::vector<held_key> held{};

        // Physical modifiers a rule took over, until they're let go: Cmd+C and then Cmd alone
        // mustn't bring Win back down, or letting go of Cmd opens Start
        uint8_t taken{0};

        // A [hold] rule's modifiers, kept down while the physical ones it took are
        uint8_t hold_from{0};
        uint8_t hold_modifiers{0};

        pc_report last{};
        // Win and Alt bits sent without a key since they went down: their release is a tap
        uint8_t lone_modifiers{0};

        void press(uint8_t key, uint8_t physical, bool fn, std::vector<pc_report>& reports);
        void tap(int rule, uint8_t physical, std::vector<pc_report>& reports);
        void emit(const pc_report& report, uint8_t physical, std::vector<pc_report>& reports);
        void send(const pc_report& report, uint8_t physical, std::vector<pc_report>& reports);

    public:
        explicit translator(options opts) : media_keys{opts.media_keys} {}

        // The current state last, preceded by any taps (sequences, Alt codes, a Globe tap)
        std::vector<pc_report> translate(const apple_keys& input);

        // From the PC's LED report, Alt codes need Num Lock on
        void set_num_lock(bool on) { this->num_lock = on; }

        // Whether F1-F12 are media keys right now
        bool is_media_keys() const { return this->media_keys; }

        // Whether keymap.txt has a toggle_media key, which then owns the Caps Lock light
        static bool toggles_media();
    };
}

#pragma once
#include <cstdint>

// What keymap.txt turns into: scripts/gen_keymap.py writes these into gen/keymap_table.h
namespace magic_keyboard::keyboard::keymap {
    // HID modifiers come in pairs, the left one in the low nibble and the right one four higher
    enum group : uint8_t { CTRL, SHIFT, ALT, GUI, GROUPS };

    // The Lock key is a flag in the Apple report, not a usage, so it gets one of its own
    constexpr uint8_t LOCK_KEY{0xF0};

    struct trigger {
        uint8_t key;        // 0 for a Globe tap
        uint8_t groups;     // a bit per group that needs one of its sides held
        uint8_t sides;      // modifier bits that must be held exactly
        bool globe;
        bool media;
    };

    enum class action : uint8_t { keys, consumer, system, sequence, altcode, toggle_media, none };

    struct step {
        uint8_t modifiers;
        uint8_t key;
    };

    struct rule {
        trigger when;
        action does;
        uint8_t modifiers;  // keys
        uint8_t key;        // keys
        uint16_t value;     // the consumer usage, system bits, Alt code or first step
        uint8_t steps;      // sequence length, or how many digits the Alt code has
        bool hold;
    };
}

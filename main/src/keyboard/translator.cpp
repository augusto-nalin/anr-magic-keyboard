#include "keyboard/translator.h"

#include <algorithm>
#include <bit>

#include "keymap_table.h"

namespace magic_keyboard::keyboard {
    namespace {
        using namespace keymap;
        namespace table = gen::keymap;

        namespace modifier {
            constexpr uint8_t LEFT_CTRL{0x01};
            constexpr uint8_t LEFT_ALT{0x04};

            // Win and Alt pressed and released with nothing in between do something of their
            // own in Windows: Start and the menu bar
            constexpr uint8_t TAPPABLE{0xCC};
        }

        namespace usage {
            constexpr uint8_t NUM_LOCK{0x53};
            constexpr uint8_t KEYPAD_1{0x59};
            constexpr uint8_t KEYPAD_0{0x62};
        }

        constexpr uint8_t pair(int group) {
            return static_cast<uint8_t>((1U << group) | (1U << (group + 4)));
        }

        // Held Mac modifiers as what keymap.txt's [modifiers] makes them, each side kept
        uint8_t to_pc(uint8_t physical) {
            uint8_t modifiers{0};
            for (int group = 0; group < GROUPS; ++group) {
                const int role = table::roles[group];
                if (role < 0) {
                    continue;
                }
                if ((physical & (1U << group)) != 0) {
                    modifiers |= static_cast<uint8_t>(1U << role);
                }
                if ((physical & (1U << (group + 4))) != 0) {
                    modifiers |= static_cast<uint8_t>(1U << (role + 4));
                }
            }
            return modifiers;
        }

        // The rule for a key going down: the one naming the most of what's held, the first on a tie
        int find_rule(uint8_t key, uint8_t physical, bool fn, bool media) {
            int best{-1};
            int best_score{-1};

            for (size_t i = 0; i < table::rules.size(); ++i) {
                const trigger& when = table::rules[i].when;
                if (when.key != key || (when.globe && !fn) || (when.media && !media)) {
                    continue;
                }
                if ((physical & when.sides) != when.sides) {
                    continue;
                }

                bool held{true};
                int named = std::popcount(when.sides) + (when.globe ? 1 : 0);
                for (int group = 0; group < GROUPS; ++group) {
                    if ((when.groups & (1U << group)) != 0) {
                        held = held && (physical & pair(group)) != 0;
                        ++named;
                    }
                }
                if (!held) {
                    continue;
                }

                const int score = named * 2 + (when.media ? 1 : 0);
                if (score > best_score) {
                    best       = static_cast<int>(i);
                    best_score = score;
                }
            }
            return best;
        }

        // The physical modifiers a rule names, which it replaces with its own
        uint8_t taken_by(const trigger& when, uint8_t physical) {
            uint8_t taken = when.sides;
            for (int group = 0; group < GROUPS; ++group) {
                if ((when.groups & (1U << group)) != 0) {
                    taken |= physical & pair(group);
                }
            }
            return taken;
        }

        // First free slot. With six keys already down it's dropped, like on any 6KRO keyboard.
        void press_key(pc_report& report, uint8_t key) {
            auto slot = std::ranges::find(report.keys, uint8_t{0});
            if (slot != report.keys.end()) {
                *slot = key;
            }
        }

        pc_report key_report(uint8_t modifiers, uint8_t key) {
            pc_report report{};
            report.modifiers = modifiers;
            report.keys[0]   = key;
            return report;
        }

        bool has_keys(const pc_report& report) {
            return report.consumer != 0 || report.system != 0 ||
                   std::ranges::any_of(report.keys, [](uint8_t key) { return key != 0; });
        }
    }

    std::optional<apple_keys> apple_keys::parse(std::span<const uint8_t> report) {
        if (report.size() != 9) {
            return std::nullopt;
        }

        apple_keys keys{};
        keys.modifiers = report[0];
        std::copy_n(report.begin() + 2, keys.keys.size(), keys.keys.begin());
        keys.fn   = (report[8] & 0x02) != 0;
        keys.lock = (report[8] & 0x08) != 0;
        return keys;
    }

    std::optional<apple_battery> apple_battery::parse(std::span<const uint8_t> report) {
        if (report.size() != 2) {
            return std::nullopt;
        }

        return apple_battery{report[1], (report[0] & 0x02) != 0};
    }

    bool translator::toggles_media() {
        return table::toggles_media;
    }

    void translator::emit(const pc_report& report, uint8_t physical, std::vector<pc_report>& reports) {
        // Windows can take the keys of a report before its modifiers, so Shift+Home in one report
        // is a plain Home. Modifiers that change along with a new key go out first on their own.
        const bool new_keys = std::ranges::any_of(report.keys, [&](uint8_t key) {
            return key != 0 && std::ranges::find(this->last.keys, key) == this->last.keys.end();
        });
        if (new_keys && report.modifiers != this->last.modifiers) {
            pc_report modifiers_first = this->last;
            modifiers_first.modifiers = report.modifiers;
            std::ranges::replace_if(modifiers_first.keys, [&](uint8_t key) {
                return std::ranges::find(report.keys, key) == report.keys.end();
            }, uint8_t{0});
            send(modifiers_first, physical, reports);
        }
        send(report, physical, reports);
    }

    void translator::send(const pc_report& report, uint8_t physical, std::vector<pc_report>& reports) {
        // A Win or Alt going up because a rule took it over, not because it was let go, would
        // still be a tap to Windows. A Ctrl in between keeps it from being one.
        const uint8_t going = this->last.modifiers & ~report.modifiers & this->lone_modifiers;
        if ((going & to_pc(physical)) != 0) {
            pc_report blocker = this->last;
            blocker.modifiers |= modifier::LEFT_CTRL;
            reports.push_back(blocker);
        }

        reports.push_back(report);
        this->lone_modifiers |= report.modifiers & ~this->last.modifiers & modifier::TAPPABLE;
        this->lone_modifiers &= report.modifiers;
        if (has_keys(report)) {
            this->lone_modifiers = 0;
        }
        this->last = report;
    }

    void translator::tap(int index, uint8_t physical, std::vector<pc_report>& reports) {
        const rule& rule = table::rules[static_cast<size_t>(index)];

        switch (rule.does) {
            case action::keys:
                emit(key_report(rule.modifiers, rule.key), physical, reports);
                emit(pc_report{}, physical, reports);
                break;
            case action::consumer: {
                pc_report report{};
                report.consumer = rule.value;
                emit(report, physical, reports);
                emit(pc_report{}, physical, reports);
                break;
            }
            case action::system: {
                pc_report report{};
                report.system = static_cast<uint8_t>(rule.value);
                emit(report, physical, reports);
                emit(pc_report{}, physical, reports);
                break;
            }
            case action::sequence:
                for (size_t i = rule.value; i < rule.value + rule.steps; ++i) {
                    const step& step = table::steps[i];
                    emit(key_report(step.modifiers, step.key), physical, reports);
                    emit(key_report(step.modifiers, 0), physical, reports);
                }
                emit(pc_report{}, physical, reports);
                break;
            case action::altcode: {
                // Alt held while the code is typed on the keypad, which only types digits with
                // Num Lock on. Its digits are the generator's, leading zeros included.
                const bool toggle_num_lock = !this->num_lock;
                if (toggle_num_lock) {
                    emit(key_report(0, usage::NUM_LOCK), physical, reports);
                    emit(pc_report{}, physical, reports);
                }

                emit(key_report(modifier::LEFT_ALT, 0), physical, reports);
                unsigned divisor{1};
                for (int i = 1; i < rule.steps; ++i) {
                    divisor *= 10;
                }
                for (; divisor > 0; divisor /= 10) {
                    const unsigned digit = (rule.value / divisor) % 10;
                    const uint8_t key    = digit == 0 ? usage::KEYPAD_0
                                                      : static_cast<uint8_t>(usage::KEYPAD_1 + digit - 1);
                    emit(key_report(modifier::LEFT_ALT, key), physical, reports);
                    emit(key_report(modifier::LEFT_ALT, 0), physical, reports);
                }
                emit(pc_report{}, physical, reports);

                if (toggle_num_lock) {
                    emit(key_report(0, usage::NUM_LOCK), physical, reports);
                    emit(pc_report{}, physical, reports);
                }
                break;
            }
            case action::toggle_media:
                this->media_keys = !this->media_keys;
                break;
            case action::none:
                break;
        }
    }

    void translator::press(uint8_t key, uint8_t physical, bool fn, std::vector<pc_report>& reports) {
        const int index = find_rule(key, physical, fn, this->media_keys != fn);
        if (index < 0) {
            this->held.push_back({key, key, 0, 0, 0, true});
            return;
        }

        const rule& rule  = table::rules[static_cast<size_t>(index)];
        const uint8_t took = taken_by(rule.when, physical);
        this->taken |= took;

        held_key entry{key, 0, 0, 0, 0, false};
        switch (rule.does) {
            case action::keys:
                entry.key       = rule.key;
                entry.modifiers = rule.modifiers;
                if (rule.hold) {
                    this->hold_from      = took;
                    this->hold_modifiers = rule.modifiers;
                }
                break;
            case action::consumer:
                entry.consumer = rule.value;
                break;
            case action::system:
                entry.system = static_cast<uint8_t>(rule.value);
                break;
            case action::sequence:
            case action::altcode:
            case action::toggle_media:
            case action::none:
                tap(index, physical, reports);
                break;
        }
        this->held.push_back(entry);
    }

    std::vector<pc_report> translator::translate(const apple_keys& input) {
        std::vector<pc_report> reports{};
        const uint8_t physical = input.modifiers;

        this->taken &= physical;
        if ((this->hold_from & physical) == 0) {
            this->hold_from      = 0;
            this->hold_modifiers = 0;
        }

        std::array<uint8_t, 7> down{};
        size_t down_count{0};
        for (const uint8_t key: input.keys) {
            if (key != 0) {
                down[down_count++] = key;
            }
        }
        if (input.lock) {
            down[down_count++] = LOCK_KEY;
        }
        const std::span<const uint8_t> pressed{down.data(), down_count};

        std::erase_if(this->held, [&](const held_key& held) {
            return std::ranges::find(pressed, held.input) == pressed.end();
        });

        // A Globe tap is a press and release with nothing else touched in between
        if (input.fn) {
            if (!this->was_fn) {
                this->globe_alone = true;
            }
            if (!pressed.empty() || physical != 0) {
                this->globe_alone = false;
            }
        } else if (this->was_fn) {
            if (this->globe_alone && pressed.empty()) {
                if (const int index = find_rule(0, physical, true, this->media_keys); index >= 0) {
                    tap(index, physical, reports);
                }
            }
            this->globe_alone = false;
        }
        this->was_fn = input.fn;

        for (const uint8_t key: pressed) {
            const bool known = std::ranges::any_of(this->held, [key](const held_key& held) { return held.input == key; });
            if (!known) {
                press(key, physical, input.fn, reports);
            }
        }

        pc_report report{};
        report.modifiers = to_pc(physical & ~this->taken) | this->hold_modifiers;
        bool passthrough{false};
        for (const held_key& held: this->held) {
            if (held.key != 0) {
                press_key(report, held.key);
            }
            if (report.consumer == 0) {
                report.consumer = held.consumer;
            }
            report.modifiers |= held.modifiers;
            report.system |= held.system;
            passthrough = passthrough || (held.passthrough && held.key != 0);
        }

        // A key without a rule gets back the modifiers an earlier rule took: Cmd+C, then Cmd+E
        // in the same hold is Ctrl+C, then Win+E
        if (passthrough) {
            report.modifiers |= to_pc(physical & this->taken & ~this->hold_from);
        }

        emit(report, physical, reports);
        return reports;
    }
}

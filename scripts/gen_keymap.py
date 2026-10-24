#!/usr/bin/env python3
"""Turns keymap.txt into the C++ table the translator runs.

Usage: gen_keymap.py <keymap.txt> <output header>

Any mistake in the file stops the build with its line number, so a typo never reaches the
keyboard as a silently dead key.
"""
import sys

LETTERS = {c: 0x04 + i for i, c in enumerate("abcdefghijklmnopqrstuvwxyz")}
DIGITS = {str((i + 1) % 10): 0x1E + i for i in range(10)}

# HID keyboard usages both sides share
COMMON = {
    **LETTERS,
    **DIGITS,
    "enter": 0x28, "esc": 0x29, "backspace": 0x2A, "tab": 0x2B, "space": 0x2C,
    "minus": 0x2D, "equal": 0x2E, "lbracket": 0x2F, "rbracket": 0x30, "semicolon": 0x33,
    "quote": 0x34, "comma": 0x36, "dot": 0x37, "slash": 0x38, "capslock": 0x39,
    **{f"f{i}": 0x3A + i - 1 for i in range(1, 13)},
    "printscreen": 0x46, "scrolllock": 0x47, "pause": 0x48, "insert": 0x49, "home": 0x4A,
    "pageup": 0x4B, "delete": 0x4C, "end": 0x4D, "pagedown": 0x4E,
    "right": 0x4F, "left": 0x50, "down": 0x51, "up": 0x52, "numlock": 0x53,
    "kp_slash": 0x54, "kp_asterisk": 0x55, "kp_minus": 0x56, "kp_plus": 0x57, "kp_enter": 0x58,
    **{f"kp{(i + 1) % 10}": 0x59 + i for i in range(10)},
    "kp_dot": 0x63, "menu": 0x65,
    **{f"f{i}": 0x68 + i - 13 for i in range(13, 25)},
}

# Mac UK legends. An Apple ISO keyboard sends § as the key a PC has by left Shift and ` as the
# one left of 1, and the key by Enter may come as either backslash usage.
INPUT_KEYS = {
    **{name: [usage] for name, usage in COMMON.items()},
    "section": [0x64],
    "grave": [0x35],
    "backslash": [0x31, 0x32],
    "lock": [0xF0],  # not a usage, the Lock key is a flag in the report (keyboard::keymap::LOCK_KEY)
}

# Standard UK PC legends
OUTPUT_KEYS = {
    **COMMON,
    "grave": 0x35,
    "hash": 0x32,
    "backslash": 0x64,
}

CONSUMER = {
    "brightness_down": 0x70, "brightness_up": 0x6F, "prev": 0xB6, "next": 0xB5, "play": 0xCD,
    "stop": 0xB7, "mute": 0xE2, "vol_down": 0xEA, "vol_up": 0xE9,
}

# Bits of the System Control report
SYSTEM = {"power": 0x01, "sleep": 0x02, "wake": 0x04}

# HID modifier pairs: left bit, right bit is four higher
GROUPS = {"ctrl": 0, "shift": 1, "alt": 2, "win": 3}

# Trigger modifiers, the Mac side: either side of a pair, or exactly one
TRIGGER_GROUPS = {"ctrl": 0, "shift": 1, "opt": 2, "cmd": 3}
TRIGGER_SIDES = {
    "lctrl": 0x01, "lshift": 0x02, "lopt": 0x04, "lcmd": 0x08,
    "rctrl": 0x10, "rshift": 0x20, "ropt": 0x40, "rcmd": 0x80,
}

OUTPUT_MODIFIERS = {
    "ctrl": 0x01, "shift": 0x02, "alt": 0x04, "win": 0x08,
    "rctrl": 0x10, "rshift": 0x20, "ralt": 0x40, "altgr": 0x40, "rwin": 0x80,
}

SIMPLE_ACTIONS = {"toggle_media", "none"}


class KeymapError(Exception):
    pass


def key_usage(token, names):
    if token.startswith("0x"):
        try:
            usage = int(token, 16)
        except ValueError:
            raise KeymapError(f"'{token}' is not a hex usage")
        if not 0 < usage <= 0xFF:
            raise KeymapError(f"'{token}' is out of range")
        return usage
    if token not in names:
        raise KeymapError(f"unknown key '{token}'")
    return names[token]


def parse_trigger(text):
    groups, sides, globe, media, keys = 0, 0, False, False, None
    for token in text.split("+"):
        token = token.strip()
        if token in TRIGGER_GROUPS:
            groups |= 1 << TRIGGER_GROUPS[token]
        elif token in TRIGGER_SIDES:
            sides |= TRIGGER_SIDES[token]
        elif token == "globe":
            globe = True
        elif token == "media":
            media = True
        elif keys is not None:
            raise KeymapError("a trigger takes one key")
        else:
            usage = key_usage(token, INPUT_KEYS)
            keys = usage if isinstance(usage, list) else [usage]

    if keys is None:
        # A modifier pressed and released on its own: only Globe sends nothing by itself
        if not globe or groups or sides or media:
            raise KeymapError("a trigger needs a key, only 'globe' works on its own (a Globe tap)")
        keys = [0]
    return keys, groups, sides, globe, media


def parse_step(text):
    modifiers, key = 0, None
    for token in text.split("+"):
        token = token.strip()
        if token in OUTPUT_MODIFIERS:
            modifiers |= OUTPUT_MODIFIERS[token]
        elif key is not None:
            raise KeymapError(f"'{text}' has more than one key")
        else:
            key = key_usage(token, OUTPUT_KEYS)
    if key is None and modifiers == 0:
        raise KeymapError("empty output")
    return modifiers, key or 0


def parse_output(text):
    """(action, modifiers, key, value, steps). An Alt code's steps are its digits: a leading 0
    picks the Windows code page over the DOS one, so it has to survive."""
    words = text.split()
    if words[0] == "altcode":
        if len(words) != 2 or not words[1].isdigit() or len(words[1]) > 4:
            raise KeymapError("altcode takes up to four digits, like 'altcode 0167'")
        return "altcode", 0, 0, int(words[1], 10), list(words[1])
    if words[0] == "consumer":
        if len(words) != 2:
            raise KeymapError("consumer takes one usage, like 'consumer 0x221'")
        usage = int(words[1], 0)
        if not 0 < usage <= 0x3FF:
            raise KeymapError("consumer usages go up to 0x3ff")
        return "consumer", 0, 0, usage, []
    if len(words) == 1 and words[0] in CONSUMER:
        return "consumer", 0, 0, CONSUMER[words[0]], []
    if len(words) == 1 and words[0] in SYSTEM:
        return "system", 0, 0, SYSTEM[words[0]], []
    if len(words) == 1 and words[0] in SIMPLE_ACTIONS:
        return words[0], 0, 0, 0, []

    steps = [parse_step(step) for step in text.split(",")]
    if len(steps) == 1:
        return "keys", steps[0][0], steps[0][1], 0, []
    return "sequence", 0, 0, 0, steps


def parse(path):
    roles = {"ctrl": "ctrl", "shift": "shift", "opt": "alt", "cmd": "win"}
    rules, steps, seen = [], [], {}
    section = None

    with open(path, encoding="utf-8") as file:
        for number, raw in enumerate(file, start=1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            try:
                if line.startswith("[") and line.endswith("]"):
                    section = line[1:-1].strip()
                    if section not in ("modifiers", "keys"):
                        raise KeymapError(f"unknown section [{section}], there's [modifiers] and [keys]")
                    continue

                if "=" not in line:
                    raise KeymapError("expected 'trigger = output'")
                left, right = (part.strip() for part in line.split("=", 1))
                if not left or not right:
                    raise KeymapError("expected 'trigger = output'")

                if section == "modifiers":
                    if left not in roles:
                        raise KeymapError(f"[modifiers] sets cmd, opt, ctrl or shift, not '{left}'")
                    if right not in GROUPS and right != "none":
                        raise KeymapError(f"a modifier becomes ctrl, shift, alt, win or none, not '{right}'")
                    roles[left] = right
                    continue
                if section != "keys":
                    raise KeymapError("rules go under [keys]")

                hold = False
                if right.endswith("[hold]"):
                    hold = True
                    right = right[: -len("[hold]")].strip()

                keys, groups, sides, globe, media = parse_trigger(left)
                action, modifiers, key, value, sequence = parse_output(right)
                if hold and action != "keys":
                    raise KeymapError("[hold] only works on a single key output")
                if action == "sequence":
                    value = len(steps)
                    steps.extend(sequence)

                for usage in keys:
                    trigger = (usage, groups, sides, globe, media)
                    if trigger in seen:
                        raise KeymapError(f"same trigger as line {seen[trigger]}")
                    seen[trigger] = number
                    rules.append((trigger, action, modifiers, key, value, len(sequence), hold))
            except KeymapError as error:
                raise KeymapError(f"{path}:{number}: {error}: {raw.strip()}")
            except ValueError as error:
                raise KeymapError(f"{path}:{number}: {error}: {raw.strip()}")

    return roles, rules, steps


def bool_cpp(value):
    return "true" if value else "false"


def write(path, roles, rules, steps):
    role_values = [
        "-1" if roles[name] == "none" else str(GROUPS[roles[name]])
        for name in ("ctrl", "shift", "opt", "cmd")
    ]
    toggles_media = any(rule[1] == "toggle_media" for rule in rules)

    lines = [
        "// auto-generated by scripts/gen_keymap.py from keymap.txt, edit that instead",
        "#pragma once",
        "#include <array>",
        "#include \"keyboard/keymap.h\"",
        "",
        "namespace magic_keyboard::gen::keymap {",
        "using namespace magic_keyboard::keyboard::keymap;",
        "",
        f"inline constexpr std::array<int8_t, GROUPS> roles{{{', '.join(role_values)}}};",
        f"inline constexpr bool toggles_media{{{bool_cpp(toggles_media)}}};",
        "",
        f"inline constexpr std::array<step, {len(steps)}> steps{{{{",
    ]
    for modifiers, key in steps:
        lines.append(f"    {{0x{modifiers:02x}, 0x{key:02x}}},")
    lines.append("}};")
    lines.append("")
    lines.append(f"inline constexpr std::array<rule, {len(rules)}> rules{{{{")
    for (usage, groups, sides, globe, media), action, modifiers, key, value, count, hold in rules:
        lines.append(
            f"    {{{{0x{usage:02x}, 0x{groups:02x}, 0x{sides:02x}, {bool_cpp(globe)}, {bool_cpp(media)}}}, "
            f"action::{action}, 0x{modifiers:02x}, 0x{key:02x}, {value}, {count}, {bool_cpp(hold)}}},"
        )
    lines.append("}};")
    # An empty std::array takes no element braces
    text ="\n".join(lines).replace("steps{{\n}};", "steps{};").replace("rules{{\n}};", "rules{};")
    text += "\n}\n"
    try:
        with open(path, encoding="utf-8") as file:
            if file.read() == text:
                return
    except FileNotFoundError:
        pass
    with open(path, "w", encoding="utf-8") as file:
        file.write(text)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        roles, rules, steps = parse(sys.argv[1])
    except KeymapError as error:
        sys.exit(f"keymap error: {error}")
    write(sys.argv[2], roles, rules, steps)


if __name__ == "__main__":
    main()

# magic keyboard bridge

ESP32 firmware that connects an Apple Magic Keyboard to a Windows PC and makes it work like on a Mac, with no driver or software on the PC.

```
Magic Keyboard ──Bluetooth Classic──► ESP32 ──Bluetooth LE──► PC
```

## what it does

- **F-row**: brightness, Task View, Search, voice typing, notifications, media and volume. Caps Lock switches between media keys and F1–F12, its light on means media. Globe flips it while held.
- **macOS shortcuts**: Cmd as Ctrl for copy, paste, undo and the like, Cmd+arrows to line start/end and document top/bottom, Cmd+Backspace deletes to line start, Cmd+Tab, Cmd+Q, Option+arrows/Backspace by word, Cmd+Shift+3/4/5 screenshots. Other Win shortcuts (Win+E...) still work.
- **Globe**: Globe+arrows for Home/End/Page Up/Page Down, Globe+Backspace for Delete, tap for the emoji panel.
- **Lock** locks the PC, **Cmd+F6** sleeps it.
- **UK layout**: § ±, \` ~, \\ |, @ and " where the keyboard prints them, Option+2 for €, Option+3 for #, with Windows set to "United Kingdom".
- **Battery** level in Windows, 0% while the keyboard is away.

All of it is in [`keymap.txt`](keymap.txt).

### keyboards

| keyboard | status |
|---|---|
| Magic Keyboard USB-C 2024 (`05AC:0320`), UK | tested |
| Magic Keyboard 2021+, with or without Touch ID or numeric keypad | should work, untested (no Touch ID on Windows) |
| Magic Keyboard 2015 (A1644) | unknown, Globe and battery may differ |
| Apple Wireless Keyboard (2009–2011) | no |

Other layouts or F-row legends need `keymap.txt` changes.

### ESP32 boards

Needs Bluetooth Classic, which only the original ESP32 has.

| chip | works |
|---|---|
| ESP32 (WROOM-32, WROVER, PICO, DevKitC...) | yes |
| ESP32-S3, C2, C3, C5, C6, C61, H2 | no, Bluetooth LE only |
| ESP32-S2, P4 | no, no Bluetooth |

4MB flash. Status LED on GPIO2, BOOT button on GPIO0.

## how to

### build and flash

Needs [ESP-IDF v6.0](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32/get-started/) at `~/source/esp-idf/v6.0` (`IDF_PATH` in the Makefile).

```bash
make build
make flash
```

On WSL, the board's USB serial port is attached with [usbipd-win](https://github.com/dorssel/usbipd-win): run `usbipd bind --hardware-id 10c4:ea60` once as admin, then `make` attaches it. Change the ID (and `USB_HWID` in the Makefile) for a board with a different USB chip.

### pair

1. Keyboard: remove it from other computers, switch it off and on near the ESP32.
2. PC: Bluetooth & devices → Add device → "Magic Keyboard Bridge".
3. Set the Windows keyboard layout to United Kingdom.

LED solid: keyboard and PC connected. Blinking: waiting for one of them.

Hold BOOT for 3 seconds to forget the keyboard and the PC, then pair both again.

### change keys

Edit `keymap.txt`, then `make flash`. One rule per line:

```
cmd+c         = ctrl+c
cmd+tab       = alt+tab [hold]
cmd+backspace = f24, shift+home, backspace
section       = altcode 0167
capslock      = toggle_media
```

Key names and actions are listed at the top of the file. Errors stop the build with the line number.

### Cmd+Backspace in terminals

Shells ignore Shift+Home, so Cmd+Backspace sends F24 first. In Windows Terminal's `settings.json`, map F24 to Ctrl+U and Ctrl+Home:

```json
"actions": [
    {
        "command": { "action": "sendInput", "input": "\u001b[85;22;21;1;8;1_\u001b[85;22;21;0;8;1_\u001b[36;71;0;1;264;1_\u001b[36;71;0;0;264;1_" },
        "id": "User.deleteToLineStart"
    }
],
"keybindings": [
    { "id": "User.deleteToLineStart", "keys": "f24" }
]
```

### settings

`make config` → Magic Keyboard Configuration.

| setting | default | |
|---|---|---|
| `MKB_MEDIA_KEYS` | on | F-row starts as media keys |
| `MKB_KEYBOARD_POWER` | idle | keyboard power saving: always, only when idle, never |
| `MKB_KEYBOARD_IDLE_SECONDS` | 20 | idle time before power saving |
| `MKB_KEYBOARD_POLL_SLOTS` | 6 | active link poll interval, 0.625ms slots |
| `MKB_KEYBOARD_SNIFF_SLOTS` | 0 | sniff interval to request, the keyboard ignores it |
| `MKB_LATENCY_MARKERS` | off | latency logs |

### make targets

| | |
|---|---|
| `build` | build |
| `flash` / `app-flash` | flash all / app only |
| `monitor` / `run` | serial monitor / flash and monitor |
| `log` / `decode-log` | capture serial / decode backtraces |
| `config` | menuconfig |
| `clean` | remove build output |

## how it works

The ESP32 is a Bluetooth Classic HID host for the keyboard and a Bluetooth LE HID keyboard for the PC, on one radio.

- **Keyboard link**: kept active and polled every 3.75ms while typing, sniff (15ms) after 20 idle seconds.
- **PC link**: 8.75ms connection interval. 7.5ms is half the keyboard's 15ms and can collide on the shared radio.
- **Keymap**: `scripts/gen_keymap.py` compiles `keymap.txt` into a table. Each key is resolved when pressed (the rule naming the most held modifiers wins) and held until released. Modifier changes are sent before the key, and a replaced Win or Alt gets a Ctrl in between so Windows doesn't treat it as a tap.
- **Reconnect**: bonds and the PC's report subscriptions are stored in flash. A disconnected keyboard is called after 20s, then every 15s. Links that don't become a working connection within a few seconds are dropped, working around a Bluedroid race that leaves them half open.
- **Security**: both links are encrypted after pairing. Pairing itself has no code on either side.

### latency

Key click (microphone) to Windows key event, median of 30 presses without the 2 fastest and slowest. Constant offsets included, compare rows.

| | median | p90 |
|---|---|---|
| USB cable | 7.0ms | 9.3ms |
| Bluetooth direct | 18.5ms | 24.1ms |
| **bridge** | **29ms** | **39ms** |
| bridge, keyboard-controlled sniff | 34ms | 45ms |

### layout

```
keymap.txt              keys
scripts/gen_keymap.py   keymap.txt → C++ table
main/src/main.cpp       keyboard → translator → PC
main/src/keyboard/      translator
main/src/hal/           Bluetooth, keyboard side, PC side, LED
tools/latency/          microphone latency tool
```

## license

Apache License 2.0, see [LICENSE](LICENSE). Copyright 2026 augusto-nalin.

A hobby project, not a product. Uses ESP-IDF (Apache License 2.0).

Not affiliated with or endorsed by Apple Inc. Apple and Magic Keyboard are trademarks of Apple Inc. No Apple code or firmware is included.

#!/usr/bin/env python3
"""Decode crash addresses in an ESP-IDF log file to source locations."""

import re
import subprocess
import sys

BACKTRACE_RE = re.compile(r"Backtrace:((?:\s+0x[0-9a-fA-F]+:0x[0-9a-fA-F]+)+)")
ABORT_RE     = re.compile(r"abort\(\) was called at PC (0x[0-9a-fA-F]+)")
ASSERT_RE    = re.compile(r"assert failed: (0x[0-9a-fA-F]+)")

def extract_pcs(line: str) -> list[str]:
    m = BACKTRACE_RE.search(line)
    if m:
        return re.findall(r"(0x[0-9a-fA-F]+):0x[0-9a-fA-F]+", m.group(1))
    m = ABORT_RE.search(line)
    if m:
        return [m.group(1)]
    m = ASSERT_RE.search(line)
    if m:
        return [m.group(1)]
    return []


def decode(addrs: list[str], elf: str) -> str:
    result = subprocess.run(
        ["xtensa-esp32-elf-addr2line", "-e", elf, "-f", "-p"] + addrs,
        capture_output=True, text=True
    )
    return result.stdout.strip()


def main() -> None:
    log_path = sys.argv[1] if len(sys.argv) > 1 else "build/debug/device.log"
    elf_path = sys.argv[2] if len(sys.argv) > 2 else "bin/debug/magic_keyboard.elf"

    with open(log_path) as f:
        lines = f.readlines()

    seen: set[frozenset[str]] = set()
    crash_num = 0

    for line in lines:
        pcs = extract_pcs(line)
        if not pcs:
            continue
        key = frozenset(pcs)
        if key in seen:
            continue
        seen.add(key)
        crash_num += 1
        print(f"=== Crash {crash_num} ===")
        print(decode(pcs, elf_path))
        print()


if __name__ == "__main__":
    main()

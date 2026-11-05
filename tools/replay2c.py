#!/usr/bin/env python3
"""Convert a .replay file to a C table for hardware tests: replay2c.py file offset_seconds."""
import os
import sys
offset = float(sys.argv[2])
KEYS = {"\n": (4, 7), " ": (5, 15), ".": (5, 6), "-": (1, 11), "/": (5, 7), ",": (5, 5), "=": (1, 12),
        ";": (4, 5), "[": (3, 1), "]": (3, 2)}
KEYS.update(zip("qwertyuiop", [(2, c) for c in (7, 8, 9, 10, 11, 12, 13, 14, 15)] + [(3, 0)]))
KEYS.update(zip("asdfghjkl", [(3, 12), (3, 13), (3, 14), (3, 15), (4, 0), (4, 1), (4, 2), (4, 3), (4, 4)]))
KEYS.update(zip("zxcvbnm", [(4, 14), (4, 15), (5, 0), (5, 1), (5, 2), (5, 3), (5, 4)]))
KEYS.update(zip("1234567890", [(1, c) for c in range(1, 11)]))
SHIFTED = {":": (4, 5), "$": (1, 4), "*": (1, 8), "!": (1, 1), "_": (1, 11), "<": (5, 5), ">": (5, 6),
           '"': (4, 6), "(": (1, 9), ")": (1, 10), "+": (1, 12), "?": (5, 7), "&": (1, 7), "%": (1, 5)}
SHIFT = 4, 12


def key(ms, row, col, down):
    print(f"{{{ms}, 'k', {(row << 8) | col}, {down}}},")


def type_text(ms, text):
    text = text.replace(chr(92) + "n", chr(10))
    for ch in text:
        lower = ch.lower()
        shift = ch.isupper() or ch in SHIFTED
        row, col = SHIFTED.get(ch) or KEYS[lower]
        # A console can stall for longer than a short press, and RISC OS then misses
        # the key, so hardware runs hold each one for TYPE_HOLD ms
        hold = int(os.environ.get("TYPE_HOLD", "20"))
        if shift:
            key(ms, *SHIFT, 1)
        key(ms + 10, row, col, 1)
        key(ms + 10 + hold, row, col, 0)
        if shift:
            key(ms + 20 + hold, *SHIFT, 0)
        ms += int(os.environ.get("TYPE_MS", "80"))


print("static const replay_t replay_events[] = {")
shot = 0
for line in open(sys.argv[1]):
    parts = line.split()
    if not parts or parts[0].startswith("#"):
        continue
    ms = int((float(parts[0]) + offset) * 1000)
    if parts[1] == "move":
        print(f"{{{ms}, 'm', {parts[2]}, {parts[3]}}},")
    elif parts[1] == "button":
        print(f"{{{ms}, 'b', {parts[2]}, 0}},")
    elif parts[1] == "shot":
        shot += 1
        print(f"{{{ms}, 's', {shot}, 0}},")
    elif parts[1] == "key":
        key(ms, int(parts[2]), int(parts[3]), int(parts[4]))
    elif parts[1] == "type":
        type_text(ms, line.split(None, 2)[2].rstrip("\n"))
    else:
        print(f"replay2c: skipping '{line.strip()}', checksums after it will differ", file=sys.stderr)
print("};")

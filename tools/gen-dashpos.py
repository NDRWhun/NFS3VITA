#!/usr/bin/env python3
"""Write the NFS3 dash HUD default-position files (edef.pos .. wdef.pos) from a config.dat.

Reads the HUD rectangle table stored at offset 0x110 of a 0x20f0-byte config.dat and writes it
in the .pos text format, one identical file per language letter.

usage: gen-dashpos.py <output-dir> <config.dat>
"""
import math
import os
import struct
import sys

CONFIG_SIZE = 0x20F0
TABLE_OFFSET = 0x110  # 0x6fbc50 - 0x6fbb40
SCREENS = ("", "Player1", "Player2")  # full screen, split-screen top, split-screen bottom
ROLES = ("Racer", "Cop")
ELEMENTS = (
    "DigitalSpeedometer", "AnalogSpeedometer", "DigitalTachometer", "AnalogTachometer",
    "LapCounter", "Time", "TutorIcon", "ConeStats", "ReplayHUD", "RearView", "Position",
    "SplitTime", "OpponentName", "OpponentList", "PlayerTickets", "WrongWayIndicator",
    "RadarDetector", "RacerMap", "Radar", "CopTickets", "RadarSpeeds", "SpikeBeltIndicator",
    "CopMap",
)


def records(config):
    """Yield (name, (x0, y0, x1, y1)) in file order; memory order is y0, x0, y1, x1."""
    for s, screen in enumerate(SCREENS):
        for r, role in enumerate(ROLES):
            base = TABLE_OFFSET + s * 0x348 + r * 0x1A4 + 0x24
            for i, element in enumerate(ELEMENTS):
                y0, x0, y1, x1 = struct.unpack_from("<4f", config, base + 16 * i)
                yield screen + role + element, (x0, y0, x1, y1)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    out_dir, config_path = sys.argv[1:]
    with open(config_path, "rb") as f:
        config = f.read()
    if len(config) != CONFIG_SIZE or struct.unpack_from("<I", config)[0] != CONFIG_SIZE:
        sys.exit("%s is not a 0x20f0-byte NFS3 config.dat" % config_path)

    lines = []
    for name, rect in records(config):
        # Reject values the text format cannot hold (no '-' allowed).
        if not all(math.isfinite(v) and v >= 0.0 for v in rect):
            sys.exit("%s has an unusable rectangle %r" % (name, rect))
        lines.append("%s: (%.6f,%.6f) - (%.6f,%.6f)\n" % ((name,) + rect))
    # End the text with a NUL byte.
    data = "".join(lines).encode("ascii") + b"\0"

    os.makedirs(out_dir, exist_ok=True)
    for letter in "egfsiw":
        with open(os.path.join(out_dir, letter + "def.pos"), "wb") as f:
            f.write(data)
    print("wrote %d records to %s/[egfsiw]def.pos" % (len(lines), out_dir))


if __name__ == "__main__":
    main()

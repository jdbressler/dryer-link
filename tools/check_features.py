#!/usr/bin/env python3
"""Checks that the simulator tests the feature list the firmware is built with.

The list lives in dryer-link.yaml (dryer_link: features:). tests/host/sim.cpp has its own copy (FEATURES), because
the simulator does not read YAML. This compares the two: data point, label, control or not, and write_check.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def from_yaml():
    text = (ROOT / "dryer-link.yaml").read_text(encoding="utf-8")
    block = text[text.index("\n  features:\n") :]
    block = block[: re.search(r"\n\S", block[1:]).start() + 1]  # up to the next top-level key
    out = []
    for item in re.split(r"\n    - ", block)[1:]:
        erd = int(re.search(r"erd:\s*(0x[0-9A-Fa-f]+)", item).group(1), 16)
        label = re.search(r"label:\s*(.+)", item).group(1).split("#")[0].strip()
        control = re.search(r"control:\s*true", item) is not None
        check = re.search(r"write_check:\s*(0x[0-9A-Fa-f]+)", item)
        out.append((erd, label, control, int(check.group(1), 16) if check else 0))
    return out


def from_sim():
    text = (ROOT / "tests" / "host" / "sim.cpp").read_text(encoding="utf-8")
    table = text[text.index("static const FeatureDef FEATURES[] = {") :]
    table = table[: table.index("};")]
    return [
        (int(erd, 16), label, control == "true", int(write, 16))
        for erd, label, control, write in re.findall(r'\{(0x[0-9A-Fa-f]+), "([^"]+)", (true|false), (?:true|false), (0x[0-9A-Fa-f]+|0)\}', table)
    ]


def main():
    a, b = from_yaml(), from_sim()
    if a == b:
        print(f"{len(a)} features, the same in dryer-link.yaml and tests/host/sim.cpp")
        return 0
    print("The feature lists differ:")
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else None
        y = b[i] if i < len(b) else None
        if x != y:
            print(f"  entry {i + 1}: dryer-link.yaml has {x}, sim.cpp has {y}")
    return 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Checks tools/flash_module.py without a module: the label it would write, the commands it would give esptool,
and that the firmware's own parser (tests/host/build/sim --parse-label) reads the password back.

  python3 tests/tools/test_flash_module.py        needs tests/host/build.sh to have been run
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import flash_module as fm  # noqa: E402

SIM = ROOT / "tests" / "host" / "build" / "sim"
failed = 0
checks = 0


def check(ok, what, detail=""):
    global failed, checks
    checks += 1
    if not ok:
        failed += 1
        print(f"  FAILED: {what}" + (f"\n     got: {detail}" if detail else ""))


def firmware_reads(image: bytes) -> str:
    return subprocess.run([str(SIM), "--parse-label", image[:80].hex()], capture_output=True, text=True, check=True).stdout.strip()


def entry(name, ptype, subtype, offset, size):
    return struct.pack("<HBBII16sI", 0x50AA, ptype, subtype, offset, size, name.encode(), 0)


# The table ESPHome builds for dryer-link.yaml (see DESIGN.md), then the checksum entry and erased flash.
TABLE = (
    entry("otadata", 1, 0, 0x9000, 0x2000) + entry("phy_init", 1, 1, 0xB000, 0x1000) + entry("app0", 0, 0x10, 0x10000, 0x1B0000)
    + entry("app1", 0, 0x11, 0x1C0000, 0x1B0000) + entry("nvs", 1, 2, 0x370000, 0x70000) + entry("dl_label", 0x40, 0, 0x3E0000, 0x1000)
    + b"\xeb\xeb" + b"\xff" * 14 + b"\x00" * 16 + b"\xff" * 2048
)

# A stand-in for esptool: notes how it was called, keeps a copy of every file it was given, prints what the real
# one prints about the chip.
FAKE_ESPTOOL = r'''
import json, os, shutil, sys
out = os.environ["FAKE_ESPTOOL_OUT"]
args = sys.argv[1:]
json.dump(args, open(os.path.join(out, "args.json"), "w"))
for a in args:
    if os.path.isfile(a):
        shutil.copy(a, os.path.join(out, os.path.basename(a).lstrip(".")))
print("esptool v5.4.0")
print("Connected to ESP32-C3 on /dev/cu.usbserial-0001:")
print("Chip type:          ESP32-C3 (QFN32) (revision v0.4)")
print("MAC:                24:58:7c:3f:a2:c4")
sys.exit(int(os.environ.get("FAKE_ESPTOOL_EXIT", "0")))
'''


def run_tool(tmp: Path, *args, exit_code=0):
    fake = tmp / "fake"
    (fake / "esptool").mkdir(parents=True, exist_ok=True)
    (fake / "esptool" / "__main__.py").write_text(FAKE_ESPTOOL)
    (fake / "esptool" / "__init__.py").write_text("")
    out = tmp / "out"
    if out.exists():
        for f in out.iterdir():
            f.unlink()
    out.mkdir(exist_ok=True)
    env = dict(os.environ, PYTHONPATH=str(fake), FAKE_ESPTOOL_OUT=str(out), FAKE_ESPTOOL_EXIT=str(exit_code))
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "flash_module.py"), *args], capture_output=True, text=True, env=env, cwd=tmp)
    return r, out


def main():
    if not SIM.is_file():
        print("tests/host/build/sim is missing: run tests/host/build.sh first")
        return 2

    print("label format")
    seen = set()
    for _ in range(300):
        pw = fm.new_password()
        seen.add(pw)
        if not re.fullmatch(r"[a-hjkmnp-z2-9]{4}-[a-hjkmnp-z2-9]{4}-[a-hjkmnp-z2-9]{4}", pw):
            check(False, "password shape", pw)
        image = fm.label_image(pw)
        if len(image) != 0x1000 or firmware_reads(image) != pw:
            check(False, "the firmware reads back the password the tool wrote", pw)
    check(len(seen) == 300, "300 passwords, all different", str(len(seen)))
    check(firmware_reads(fm.label_image("x" * 63)) == "x" * 63, "the longest Wi-Fi password round-trips")
    for bad in ("short7!", "has space in it", "x" * 64, "café-au-lait"):
        try:
            fm.label_blob(bad)
            check(False, "a password Wi-Fi cannot use is refused", bad)
        except (ValueError, UnicodeEncodeError):
            check(True, "")
    damaged = bytearray(fm.label_image("kq7m-x2hd-9pwc"))
    damaged[9] ^= 0x20
    check(firmware_reads(bytes(damaged)) == "", "a damaged label is not read as a password")
    check(fm.label_offset(TABLE) == 0x3E0000, "label partition found in the partition table")
    check(fm.module_name("24:58:7C:3F:A2:C4") == "dryer-link-3fa2c4", "module name from its MAC")
    check(fm.wifi_qr_text("dryer-link-3fa2c4", "kq7m-x2hd-9pwc") == "WIFI:T:WPA;S:dryer-link-3fa2c4;P:kq7m-x2hd-9pwc;;", "Wi-Fi QR text")
    check(fm.wifi_qr_text("a;b", 'p:"q' + chr(92)) == r'WIFI:T:WPA;S:a\;b;P:p\:\"q\\;;', "special characters are escaped in the QR text")

    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        build = tmp / "bld" / "build"
        (build / "partition_table").mkdir(parents=True)
        (build / "firmware.factory.bin").write_bytes(b"\xe9" + b"\x00" * 1000)
        (build / "partition_table" / "partition-table.bin").write_bytes(TABLE)

        print("a first flash")
        r, out = run_tool(tmp, "bld", "--port", "/dev/fake")
        check(r.returncode == 0, "exit code", r.stdout + r.stderr)
        import json

        args = json.load(open(out / "args.json"))
        check(args[:6] == ["--chip", "esp32c3", "--port", "/dev/fake", "--baud", "460800"], "chip, port and speed", str(args))
        check("write-flash" in args and "--erase-all" in args, "the module is wiped as it is flashed", str(args))
        i = args.index("0x0")
        check(args[i + 1].endswith("firmware.factory.bin") and args[i + 2] == "0x3e0000", "firmware at 0, label at its partition", str(args[i:]))
        svg = tmp / "labels" / "dryer-link-3fa2c4.svg"
        check(svg.is_file(), "the label is named after the module", str(list((tmp / "labels").iterdir())))
        text = svg.read_text() if svg.is_file() else ""
        m = re.search(r">([a-hjkmnp-z2-9]{4}-[a-hjkmnp-z2-9]{4}-[a-hjkmnp-z2-9]{4})<", text)
        check(m is not None and ">dryer-link-3fa2c4<" in text, "the label shows the network name and the password")
        written = (out / "label-partition.bin").read_bytes()
        check(m is not None and firmware_reads(written) == m.group(1), "the password on the label is the one written to the module")
        check(m is not None and m.group(1) not in r.stdout + r.stderr, "the password is not printed on the screen")
        check(not (tmp / "labels" / ".label-partition.bin").exists(), "no copy of the partition image is left behind")
        first_password = m.group(1) if m else ""

        print("a second module gets a different password")
        r, out = run_tool(tmp, "bld", "--port", "/dev/fake")
        m = re.search(r">([a-hjkmnp-z2-9]{4}-[a-hjkmnp-z2-9]{4}-[a-hjkmnp-z2-9]{4})<", svg.read_text())
        check(m is not None and m.group(1) != first_password, "different password")

        print("re-flashing with --keep")
        before = svg.read_text()
        r, out = run_tool(tmp, "bld", "--port", "/dev/fake", "--keep")
        args = json.load(open(out / "args.json"))
        check(r.returncode == 0 and "--erase-all" not in args and "0x3e0000" not in args, "nothing is wiped, the label partition is not written", str(args))
        check(args[-2:] == ["0x0", str(Path("bld") / "build" / "firmware.factory.bin")], "only the firmware image is written", str(args))
        check(svg.read_text() == before, "the label file is untouched")

        print("a flash that fails")
        svg.unlink()
        r, out = run_tool(tmp, "bld", "--port", "/dev/fake", exit_code=2)
        check(r.returncode == 1, "reported as failed", str(r.returncode))
        check(not svg.exists(), "no label for a module that was not flashed")
        check(not (tmp / "labels" / ".label-partition.bin").exists(), "no partition image left behind")

        print("no firmware built")
        r, out = run_tool(tmp, "nowhere", "--port", "/dev/fake")
        check(r.returncode == 2 and not (out / "args.json").exists(), "stops before touching the module", r.stdout)

    print(f"{checks} checks, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

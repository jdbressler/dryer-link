#!/usr/bin/env python3
"""Flash one Dryer Link module through the USB-serial adapter and make its label.

Every module gets the same firmware. What is different on each one is its setup password: this tool makes one up,
writes it into the module's small "dl_label" flash partition (which firmware updates never touch) and writes a
label to print, with the module's Wi-Fi name, the password and a QR code that joins the setup network.

Run it through flash.sh, which builds the firmware first and finds the right Python. Directly:

  python tools/flash_module.py BUILD_DIR [--port PORT] [--keep] [--labels DIR]

  BUILD_DIR   where ESPHome built the firmware, for example .esphome/build/dryer-link
  --keep      re-flash the firmware only: the module keeps its Wi-Fi details, what it learned and its password
              (without --keep the whole module is wiped and gets a new password and a new label)

Needs the esptool that comes with ESPHome (version 5). The password is stored in the label file and nowhere else:
keep the labels folder to yourself, and do not put it in the repository (.gitignore leaves it out).
"""
from __future__ import annotations

import argparse
import binascii
import re
import secrets
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qrcodegen  # noqa: E402  (MIT, Project Nayuki; a copy is next to this file)

# Keep in sync with components/dryer_link/__init__.py and label_esp32.cpp.
LABEL_PARTITION = "dl_label"
LABEL_PARTITION_TYPE = 0x40
LABEL_PARTITION_SUBTYPE = 0x00
LABEL_PARTITION_SIZE = 0x1000
LABEL_MAGIC = b"DLK1"

# Letters and digits that cannot be mistaken for one another on a printed label (no i, l, o, 0, 1).
ALPHABET = "abcdefghjkmnpqrstuvwxyz23456789"
GROUPS = 3
GROUP_LENGTH = 4


def new_password() -> str:
    """12 random characters in three groups, e.g. kq7m-x2hd-9pwc: about 59 bits, far beyond guessing over Wi-Fi."""
    return "-".join("".join(secrets.choice(ALPHABET) for _ in range(GROUP_LENGTH)) for _ in range(GROUPS))


def label_blob(password: str) -> bytes:
    """What the firmware reads: "DLK1", one length byte, the password, CRC-16/CCITT-FALSE of all of that."""
    raw = password.encode("ascii")
    if not 8 <= len(raw) <= 63 or any(not 0x21 <= b <= 0x7E for b in raw):
        raise ValueError("a Wi-Fi password is 8 to 63 printable characters without spaces")
    body = LABEL_MAGIC + bytes([len(raw)]) + raw
    return body + struct.pack(">H", binascii.crc_hqx(body, 0xFFFF))


def label_image(password: str) -> bytes:
    """The whole partition: the blob, then erased flash."""
    blob = label_blob(password)
    return blob + b"\xff" * (LABEL_PARTITION_SIZE - len(blob))


def label_offset(partition_table: bytes) -> int:
    """Where the label partition is, from the partition table ESPHome built."""
    for i in range(0, len(partition_table) - 31, 32):
        magic, ptype, subtype, offset, size, name = struct.unpack_from("<HBBII16s", partition_table, i)
        if magic != 0x50AA:
            break
        if name.rstrip(b"\0").decode("ascii", "replace") == LABEL_PARTITION:
            if (ptype, subtype, size) != (LABEL_PARTITION_TYPE, LABEL_PARTITION_SUBTYPE, LABEL_PARTITION_SIZE):
                raise ValueError("the label partition is not the kind this tool writes")
            return offset
    raise ValueError(f"this firmware has no '{LABEL_PARTITION}' partition")


def module_name(mac: str) -> str:
    """The name the module gives itself (and its setup network): dryer-link- plus the last six digits of its MAC."""
    return "dryer-link-" + mac.replace(":", "").lower()[-6:]


def wifi_qr_text(ssid: str, password: str) -> str:
    def esc(s: str) -> str:
        # The Wi-Fi QR format wants a backslash in front of these five characters.
        return "".join(chr(92) + c if c in chr(92) + ';,:"' else c for c in s)

    return f"WIFI:T:WPA;S:{esc(ssid)};P:{esc(password)};;"


def label_svg(name: str, password: str) -> str:
    """A 62 x 29 mm label (a common address-label size); prints at that size from any browser at 100 %."""
    qr = qrcodegen.QrCode.encode_text(wifi_qr_text(name, password), qrcodegen.QrCode.Ecc.MEDIUM)
    n = qr.get_size()
    side = 23.0  # mm, including the quiet zone of four modules that scanners need
    m = side / (n + 8)
    cells = "".join(
        f"M{3 + (x + 4) * m:.3f} {3 + (y + 4) * m:.3f}h{m:.3f}v{m:.3f}h-{m:.3f}z"
        for y in range(n)
        for x in range(n)
        if qr.get_module(x, y)
    )
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="62mm" height="29mm" viewBox="0 0 62 29">
<rect width="62" height="29" fill="#fff"/>
<path d="{cells}" fill="#000"/>
<g font-family="Helvetica, Arial, sans-serif" fill="#000">
<text x="28.5" y="5.9" font-size="3.6" font-weight="700">Dryer Link</text>
<text x="28.5" y="9.9" font-size="2.1">Setup Wi-Fi</text>
<text x="28.5" y="13.3" font-size="2.9" font-family="Menlo, Consolas, monospace">{name}</text>
<text x="28.5" y="17.3" font-size="2.1">Password</text>
<text x="28.5" y="20.7" font-size="2.9" font-family="Menlo, Consolas, monospace">{password}</text>
<text x="28.5" y="24.3" font-size="1.6">Scan to join the setup Wi-Fi.</text>
<text x="28.5" y="26.7" font-size="1.6">Status page: admin, same password.</text>
</g>
</svg>
"""


def find_port() -> str | None:
    import glob

    ports = sorted(glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/cu.wchusbserial*") + glob.glob("/dev/ttyUSB*"))
    return ports[0] if ports else None


def run_esptool(args: list[str]) -> tuple[int, str]:
    """Runs esptool, showing what it prints and keeping a copy."""
    proc = subprocess.Popen([sys.executable, "-m", "esptool", *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    seen = []
    assert proc.stdout is not None
    for line in proc.stdout:
        sys.stdout.write(line)
        sys.stdout.flush()
        seen.append(line)
    return proc.wait(), "".join(seen)


def main() -> int:
    ap = argparse.ArgumentParser(description="Flash one Dryer Link module and make its label.")
    ap.add_argument("build_dir", type=Path)
    ap.add_argument("--port")
    ap.add_argument("--keep", action="store_true", help="firmware only; keep Wi-Fi details, what it learned and the password")
    ap.add_argument("--labels", type=Path, default=Path("labels"))
    args = ap.parse_args()

    firmware = args.build_dir / "build" / "firmware.factory.bin"
    table = args.build_dir / "build" / "partition_table" / "partition-table.bin"
    if not firmware.is_file() or not table.is_file():
        print(f"No built firmware in {args.build_dir}. Run flash.sh, which builds it first.")
        return 2
    try:
        offset = label_offset(table.read_bytes())
    except ValueError as e:
        print(f"Cannot flash: {e}.")
        return 2

    port = args.port or find_port()
    if port is None:
        print("USB-serial adapter not found. Is it plugged in?")
        return 3

    base = ["--chip", "esp32c3", "--port", port, "--baud", "460800"]
    keep_image = ["--flash-mode", "keep", "--flash-freq", "keep", "--flash-size", "keep"]
    if args.keep:
        # The factory image is the bootloader, the partition table, an empty "which slot to start" record and the
        # firmware. The stored settings and the label partition lie behind it and are left alone.
        print(f"Flashing the firmware through {port}; the module keeps its settings and its password")
        code, out = run_esptool([*base, "write-flash", *keep_image, "0x0", str(firmware)])
        if code != 0:
            return 1
        mac = re.search(r"MAC:\s*([0-9a-fA-F:]{17})", out)
        print(f"\nDone: {module_name(mac.group(1)) if mac else 'the module'} has the new firmware. Its label is unchanged.")
        return 0

    password = new_password()
    args.labels.mkdir(parents=True, exist_ok=True)
    image = args.labels / ".label-partition.bin"
    image.write_bytes(label_image(password))
    try:
        print(f"Wiping and flashing the module through {port}")
        code, out = run_esptool([*base, "write-flash", "--erase-all", *keep_image, "0x0", str(firmware), hex(offset), str(image)])
    finally:
        image.unlink(missing_ok=True)  # the password belongs on the module and on its label, not in a stray file
    if code != 0:
        return 1
    mac = re.search(r"MAC:\s*([0-9a-fA-F:]{17})", out)
    if mac is None:
        # Flashed, but the name is unknown: the label can still be made once the module shows its network name.
        name = "dryer-link-xxxxxx"
        print("\nFlashed, but the module's address was not in the output, so the label has a placeholder name.")
    else:
        name = module_name(mac.group(1))
    label = args.labels / f"{name}.svg"
    label.write_text(label_svg(name, password), encoding="utf-8")
    print(f"\nDone: {name}")
    print(f"Label: {label}   (print it and put it on the module)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

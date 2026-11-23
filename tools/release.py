#!/usr/bin/env python3
"""What a release needs besides the compiler. Used by .github/workflows/build.yml; runs anywhere with Python 3.

  release.py version                   print the version in dryer-link.yaml
  release.py site BUILD_DIR OUT_DIR    check the built firmware and lay out what GitHub Pages serves:
                                       OUT_DIR/firmware/manifest.json, OUT_DIR/firmware/dryer-link-<version>.ota.bin
  release.py verify [MANIFEST_URL] [--version V] [--wait SECONDS]
                                       fetch the manifest and the firmware the way a module does and check them;
                                       without an address, the one the firmware is built with (dryer-link.yaml)

A module's update check reads the manifest (ESPHome's http_request update platform), compares "version" with its
own, and installs "path" if they differ, after checking the download against "md5". So whatever manifest.json says
at the update address is what every module in the field ends up running.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from urllib.parse import urljoin

ROOT = Path(__file__).resolve().parent.parent
CHIP_FAMILY = "ESP32-C3"  # must equal ESPHome's ESPHOME_VARIANT for the board
CHIP_ID_ESP32C3 = 5       # in the firmware image header
PROJECT_NAME = b"dryer-link.module"


def version() -> str:
    text = (ROOT / "dryer-link.yaml").read_text(encoding="utf-8")
    m = re.search(r'^substitutions:\n(?:[ \t#].*\n|\n)*?[ \t]+version:[ \t]*"([^"]+)"', text, re.M)
    if m is None or not re.fullmatch(r"\d+\.\d+\.\d+", m.group(1)):
        raise SystemExit('dryer-link.yaml: expected substitutions: version: "N.N.N"')
    return m.group(1)


def update_address() -> str:
    text = (ROOT / "dryer-link.yaml").read_text(encoding="utf-8")
    m = re.search(r"^[ \t]+update_manifest:[ \t]*(https://\S+)", text, re.M)
    if m is None:
        raise SystemExit("dryer-link.yaml: expected substitutions: update_manifest: https://...")
    return m.group(1)


def check_image(data: bytes, ver: str, smallest_app: int | None) -> list[str]:
    """Reasons this is not a release firmware for this board; empty when it is."""
    bad = []
    if len(data) < 0x10000 or data[0] != 0xE9:
        return ["not an ESP32 firmware image"]
    if struct.unpack_from("<H", data, 12)[0] != CHIP_ID_ESP32C3:
        bad.append("built for another chip")
    if PROJECT_NAME not in data or ver.encode() not in data:
        bad.append(f"project name or version {ver} not found in it")
    if b"0.0.0-test" in data or b"TEST BUILD" in data:
        bad.append("this is the test build")
    if smallest_app is not None and len(data) > smallest_app:
        bad.append(f"too large for the firmware partition ({len(data)} > {smallest_app})")
    return bad


def smallest_app_partition(table: bytes) -> int | None:
    sizes = []
    for i in range(0, len(table) - 31, 32):
        magic, ptype, _subtype, _offset, size = struct.unpack_from("<HBBII", table, i)
        if magic != 0x50AA:
            break
        if ptype == 0x00:
            sizes.append(size)
    return min(sizes) if sizes else None


def site(build_dir: Path, out_dir: Path, repo: str) -> int:
    ver = version()
    image = (build_dir / "build" / "firmware.ota.bin").read_bytes()
    table = (build_dir / "build" / "partition_table" / "partition-table.bin").read_bytes()
    bad = check_image(image, ver, smallest_app_partition(table))
    if bad:
        print("Not publishing: " + "; ".join(bad))
        return 1
    fw = out_dir / "firmware"
    fw.mkdir(parents=True, exist_ok=True)
    name = f"dryer-link-{ver}.ota.bin"
    (fw / name).write_bytes(image)
    manifest = {
        "name": "Dryer Link",
        "version": ver,
        "builds": [
            {
                "chipFamily": CHIP_FAMILY,
                "ota": {
                    "path": name,  # relative: a module resolves it against the manifest's own address
                    "md5": hashlib.md5(image).hexdigest(),
                    "summary": f"Dryer Link {ver}",
                    "release_url": f"https://github.com/{repo}/releases/tag/v{ver}",
                },
            }
        ],
    }
    (fw / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (out_dir / "index.html").write_text(
        "<!doctype html><meta charset=utf-8><title>Dryer Link firmware</title>"
        f"<p>Dryer Link modules fetch their firmware from here. Current version: {ver}.</p>"
        f'<p><a href="https://github.com/{repo}">Source and documentation</a></p>\n',
        encoding="utf-8",
    )
    print(f"{fw / 'manifest.json'}: version {ver}, {len(image)} bytes, md5 {manifest['builds'][0]['ota']['md5']}")
    return 0


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise urllib.error.HTTPError(req.full_url, code, f"redirects to {newurl}", headers, fp)


def fetch(url: str) -> tuple[bytes, dict]:
    # As the module asks: no compression, no redirects followed.
    opener = urllib.request.build_opener(NoRedirect)
    req = urllib.request.Request(url, headers={"User-Agent": "dryer-link-release-check", "Accept-Encoding": "identity"})
    with opener.open(req, timeout=30) as r:
        return r.read(), {k.lower(): v for k, v in r.headers.items()}


def verify_once(url: str, want: str | None) -> list[str]:
    bad = []
    if not url.startswith("https://"):
        bad.append("the update address is not https")
    try:
        body, headers = fetch(url)
    except Exception as e:  # noqa: BLE001
        return [f"manifest: {e}"]
    # The module allocates exactly Content-Length bytes for the manifest and for the firmware.
    if headers.get("content-length") != str(len(body)):
        bad.append("manifest: no usable Content-Length")
    try:
        m = json.loads(body)
    except ValueError:
        return bad + ["manifest: not JSON"]
    # The fields ESPHome's parser insists on.
    if not isinstance(m.get("name"), str) or not isinstance(m.get("version"), str) or not isinstance(m.get("builds"), list):
        return bad + ["manifest: name, version or builds missing"]
    if want is not None and m["version"] != want:
        return bad + [f"manifest: says version {m['version']}, expected {want}"]
    build = next((b for b in m["builds"] if isinstance(b, dict) and b.get("chipFamily") == CHIP_FAMILY), None)
    ota = build.get("ota") if build else None
    if not isinstance(ota, dict) or not isinstance(ota.get("path"), str) or not isinstance(ota.get("md5"), str):
        return bad + [f"manifest: no build for {CHIP_FAMILY} with ota.path and ota.md5"]
    if not re.fullmatch(r"[0-9a-f]{32}", ota["md5"]):
        bad.append("manifest: md5 must be 32 lower-case hex digits")
    fw_url = ota["path"] if ota["path"].startswith("http") else urljoin(url, ota["path"])
    if len(fw_url) > 200:
        bad.append("firmware address too long for the module's request buffer")
    try:
        image, headers = fetch(fw_url)
    except Exception as e:  # noqa: BLE001
        return bad + [f"firmware: {e}"]
    if headers.get("content-length") != str(len(image)):
        bad.append("firmware: no usable Content-Length")
    if hashlib.md5(image).hexdigest() != ota["md5"]:
        bad.append("firmware: does not match the md5 in the manifest")
    bad += [f"firmware: {reason}" for reason in check_image(image, m["version"], None)]
    if not bad:
        print(f"OK: {url}\n    version {m['version']}, firmware {fw_url}, {len(image)} bytes, md5 {ota['md5']}")
    return bad


def verify(url: str, want: str | None, wait: int) -> int:
    deadline = time.time() + wait
    while True:
        bad = verify_once(url, want)
        if not bad:
            return 0
        if time.time() >= deadline:
            print("FAILED: " + "; ".join(bad))
            return 1
        time.sleep(10)  # a fresh deployment takes a moment to reach every server


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("version")
    p = sub.add_parser("site")
    p.add_argument("build_dir", type=Path)
    p.add_argument("out_dir", type=Path)
    p.add_argument("--repo", required=True, help="owner/name on GitHub")
    p = sub.add_parser("verify")
    p.add_argument("url", nargs="?")
    p.add_argument("--version", dest="want")
    p.add_argument("--wait", type=int, default=0, help="keep trying for this many seconds")
    args = ap.parse_args()
    if args.cmd == "version":
        print(version())
        return 0
    if args.cmd == "site":
        return site(args.build_dir, args.out_dir, args.repo)
    return verify(args.url or update_address(), args.want, args.wait)


if __name__ == "__main__":
    sys.exit(main())

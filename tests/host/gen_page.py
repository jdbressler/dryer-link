#!/usr/bin/env python3
"""Print the setup page as the C array the firmware build embeds (same steps as components/dryer_link/__init__.py)."""
import gzip
from pathlib import Path

page = (Path(__file__).resolve().parent.parent.parent / "components" / "dryer_link" / "setup.html").read_text(encoding="utf-8")
assert page.count("const LIVE = false;") == 1
gz = gzip.compress(page.replace("const LIVE = false;", "const LIVE = true;").encode("utf-8"), compresslevel=9, mtime=0)
print("#include <cstddef>\n#include <cstdint>")
print(f"extern const uint8_t DRYER_LINK_PAGE_GZ[{len(gz)}] = {{{', '.join(str(b) for b in gz)}}};")
print(f"extern const size_t DRYER_LINK_PAGE_GZ_SIZE = {len(gz)};")

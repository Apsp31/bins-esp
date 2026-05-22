from pathlib import Path
import os
import re

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
VERSION_FILE = PROJECT_DIR / "VERSION"
HEADER_FILE = PROJECT_DIR / "include" / "version.h"


def next_patch_version(value):
    match = re.fullmatch(r"\s*(\d+)\.(\d+)\.(\d+)\s*", value)
    if not match:
        return "0.1.0"
    major, minor, patch = (int(part) for part in match.groups())
    return f"{major}.{minor}.{patch + 1}"


current_version = VERSION_FILE.read_text(encoding="utf-8") if VERSION_FILE.exists() else "0.0.0"
firmware_version = current_version.strip() if os.environ.get("BINS_SKIP_VERSION_BUMP") == "1" else next_patch_version(current_version)

VERSION_FILE.write_text(f"{firmware_version}\n", encoding="utf-8")
HEADER_FILE.parent.mkdir(parents=True, exist_ok=True)
HEADER_FILE.write_text(
    "#pragma once\n\n"
    f'#define FIRMWARE_VERSION "{firmware_version}"\n',
    encoding="utf-8",
)

print(f"Firmware version: {firmware_version}")

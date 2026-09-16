#!/usr/bin/env python3
"""Guarded local patch for one assertion-verified Arduino-ESP32 libnet80211.a layout."""
import argparse
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

TEXT_BASE = 0x307C
PATCH_VMA = 0x11C
ENTRY_VMA = 0x5C
ENTRY_BYTES = bytes.fromhex("368100")
ORIGINAL_BYTES = bytes.fromhex("d1c1ff")
PATCHED_BYTES = bytes.fromhex("460700")
RELA_FILE_OFFSET = 0xF020
RELA_SIZE = 0x450
RELA_ENTRY_SIZE = 12
OBJECT_NAME = "ieee80211_output.o"
FUNCTION_NAME = "ieee80211_raw_frame_sanity_check"


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    if result.returncode:
        raise RuntimeError("command failed: {}\n{}".format(" ".join(map(str, command)), result.stderr.strip()))
    return result.stdout


def md5(path):
    digest = hashlib.md5()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def default_objdump():
    candidate = Path.home() / ".platformio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-objdump"
    return str(candidate) if candidate.is_file() else "xtensa-esp32-elf-objdump"


def relocation_count_and_type(data):
    matches = []
    for offset in range(RELA_FILE_OFFSET, RELA_FILE_OFFSET + RELA_SIZE, RELA_ENTRY_SIZE):
        relocation_offset, relocation_info, _ = struct.unpack_from("<IIi", data, offset)
        if relocation_offset == PATCH_VMA:
            matches.append((offset, relocation_info & 0xff))
    assert len(matches) == 1, f"expected exactly one relocation at VMA 0x{PATCH_VMA:x}, found {len(matches)}"
    return matches[0]


def validate_entry(data):
    entry_offset = TEXT_BASE + ENTRY_VMA
    assert data[entry_offset:entry_offset + 3] == ENTRY_BYTES, "expected function entry bytes not found"


def validate_disassembly(objdump, obj):
    disassembly = run([objdump, "-dr", str(obj)])
    marker = f"<{FUNCTION_NAME}>:"
    assert marker in disassembly, f"function not found in objdump output: {FUNCTION_NAME}"
    function = disassembly[disassembly.index(marker):]
    lines = [line for line in function.splitlines() if line.strip().startswith(f"{PATCH_VMA:x}:")]
    branches = [line for line in lines if "13d" in line and "l32r" not in line]
    assert len(branches) == 1, "patched instruction is not exactly one branch to 0x13d"


def main():
    parser = argparse.ArgumentParser(description="Patch one supported local libnet80211.a in place.")
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--objdump", default=default_objdump())
    args = parser.parse_args()
    library = args.library.expanduser().resolve()
    backup = Path(f"{library}.bak-orig")
    assert library.is_file(), f"library not found: {library}"

    with tempfile.TemporaryDirectory(prefix="patch-libnet80211-") as temporary:
        workdir = Path(temporary)
        members = run(["ar", "t", str(library)]).splitlines()
        assert members, "archive has no members"
        run(["ar", "x", str(library)], cwd=workdir)
        obj = workdir / OBJECT_NAME
        assert obj.is_file(), f"required archive member missing: {OBJECT_NAME}"
        data = bytearray(obj.read_bytes())
        validate_entry(data)
        patch_offset = TEXT_BASE + PATCH_VMA
        actual = data[patch_offset:patch_offset + 3]
        relocation_offset, relocation_type = relocation_count_and_type(data)

        if actual == PATCHED_BYTES:
            assert relocation_type == 0, f"patched relocation at VMA 0x{PATCH_VMA:x} is not NONE"
            validate_disassembly(args.objdump, obj)
            print(f"Already patched {library}; MD5 {md5(library)}")
            return

        assert actual == ORIGINAL_BYTES, f"VMA 0x{PATCH_VMA:x} is {actual.hex()}, expected original or patched bytes"
        data[patch_offset:patch_offset + 3] = PATCHED_BYTES
        relocation_info = struct.unpack_from("<I", data, relocation_offset + 4)[0]
        data[relocation_offset + 4:relocation_offset + 8] = struct.pack("<I", relocation_info & ~0xff)
        obj.write_bytes(data)
        validate_entry(data)
        _, relocation_type = relocation_count_and_type(data)
        assert relocation_type == 0, f"patched relocation at VMA 0x{PATCH_VMA:x} is not NONE"
        validate_disassembly(args.objdump, obj)
        patched_archive = workdir / "libnet80211.a.patched"
        run(["ar", "rcs", str(patched_archive)] + [str(workdir / member) for member in members])
        assert run(["ar", "t", str(patched_archive)]).splitlines() == members, "archive member order changed"
        if not backup.exists():
            shutil.copy2(library, backup)
        shutil.copy2(patched_archive, library)
        assert md5(patched_archive) == md5(library), "installed archive MD5 differs from patched archive"
        print(f"Installed {library}; MD5 {md5(library)}")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, RuntimeError, struct.error) as error:
        sys.exit(f"patch not installed: {error}")

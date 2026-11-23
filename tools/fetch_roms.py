"""Fetches the Junior's ROMs into roms/ (git-ignored).

    python tools/fetch_roms.py              download the archives listed in tools/sources.lock.json
    python tools/fetch_roms.py --from DIR   take them from DIR instead (no network)

Each archive (the four BIOS versions' EPROM pairs and the CGA character
ROM, as archived at z80-romania.ro) is checked against its size and SHA-256
in tools/sources.lock.json before it is opened. The BIOS pairs are
interleaved into 64 KiB images as the board wires them (IC63 on the even
bytes, IC64 on the odd: the 8086's low and high byte lanes at F0000h), the
2 KiB character ROM is taken as it is, and every image is checked against
the SHA-256 MezinXT knows it by (src/core/romdb.c) before it is written as
roms/bios-3.0.rom, bios-4.0.rom, bios-5.0.rom, bios-5.0-alt.rom and
chargen.rom. Downloads are kept under work/downloads.

The ROMs are FEPER's; they are not part of MezinXT's source.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import sys
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / "tools" / "sources.lock.json"
DOWNLOADS = ROOT / "work" / "downloads"

# The images MezinXT recognises (src/core/romdb.c): name -> (archive, SHA-256).
IMAGES = {
    "bios-3.0.rom": ("bios-3.0.zip", "3e53b7d92f053101127b9da0ea85f81e173c9eb1ec5aec2ad7e045259e1d0fe3"),
    "bios-4.0.rom": ("bios-4.0.zip", "d9edcd90a2c279adf2dd2c4a481fdc75ca24decc84bd96d66597fb1a40a0b27f"),
    "bios-5.0.rom": ("bios-5.0.zip", "da6f960a91fdeb739950c425d0982f4074b0b7ea000db87a7557a595aa3779f5"),
    "bios-5.0-alt.rom": ("bios-5.0-alt.zip", "ba4e79d297f52a9ba90fadd004c86f3d108303512d913dd2baeb2345c31bc3e7"),
    "chargen.rom": ("video.zip", "36ec4e835203128d7e06aa48c699b5e2a206dc837031be4a06a1de0179ff0c51"),
}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def fetch(name: str, entry: dict, source: Path | None) -> bytes:
    """The archive's bytes, from the folder given or downloaded, checked."""
    if source is not None:
        path = source / name
        if not path.is_file():
            raise SystemExit(f"{path}: not found")
        data = path.read_bytes()
    else:
        cached = DOWNLOADS / name
        data = cached.read_bytes() if cached.is_file() else b""
        if len(data) != entry["bytes"] or sha256(data) != entry["sha256"]:
            parts = urllib.parse.urlsplit(entry["url"])
            url = urllib.parse.urlunsplit(parts._replace(path=urllib.parse.quote(parts.path)))
            print(f"downloading {name}")
            request = urllib.request.Request(url, headers={"User-Agent": "MezinXT-fetch_roms"})
            with urllib.request.urlopen(request, timeout=60) as response:
                data = response.read()
            DOWNLOADS.mkdir(parents=True, exist_ok=True)
            cached.write_bytes(data)
    if len(data) != entry["bytes"] or sha256(data) != entry["sha256"]:
        raise SystemExit(f"{name}: size or SHA-256 differs from tools/sources.lock.json")
    return data


def members(data: bytes) -> dict[str, bytes]:
    """The archive's files by lower-case base name."""
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        return {Path(n).name.lower(): z.read(n) for n in z.namelist() if not n.endswith("/")}


def interleave(even: bytes, odd: bytes) -> bytes:
    rom = bytearray(len(even) * 2)
    rom[0::2] = even
    rom[1::2] = odd
    return bytes(rom)


def build(archive: str, data: bytes) -> bytes:
    files = members(data)
    if archive == "video.zip":
        fonts = [b for n, b in files.items() if n.endswith(".bin")]
        if len(fonts) != 1 or len(fonts[0]) != 2048:
            raise SystemExit("video.zip: expected one 2 KiB .bin")
        return fonts[0]
    even, odd = files.get("ic63_m27c256.bin"), files.get("ic64_m27c256.bin")
    if not even or not odd or len(even) != 32768 or len(odd) != 32768:
        raise SystemExit(f"{archive}: expected IC63_M27C256.BIN and IC64_M27C256.BIN of 32 KiB")
    rom = interleave(even, odd)
    # The 8086 starts at FFFF0h: a far jump to F000:E000; the bytes sum to 0.
    if rom[0xFFF0:0xFFF5] != bytes.fromhex("ea00e000f0") or sum(rom) % 256:
        raise SystemExit(f"{archive}: the interleaved image has no reset jump or a bad checksum")
    return rom


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--from", dest="source", type=Path, help="folder holding the archives (no download)")
    parser.add_argument("--out", type=Path, default=ROOT / "roms", help="where the images go (default roms/)")
    args = parser.parse_args(argv)
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    args.out.mkdir(parents=True, exist_ok=True)
    for rom_name, (archive, want) in IMAGES.items():
        image = build(archive, fetch(archive, lock[archive], args.source))
        if sha256(image) != want:
            raise SystemExit(f"{rom_name}: the image's SHA-256 is not the one MezinXT knows")
        (args.out / rom_name).write_bytes(image)
        print(f"{rom_name}: {len(image)} bytes, SHA-256 {want[:16]}...")
    print(f"PASS: {len(IMAGES)} ROMs in {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

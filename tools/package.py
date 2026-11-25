"""Assembles a MezinXT release archive and checks it. tools/build_dist.ps1 (Windows) and tools/build_linux.sh (Linux)
call it after they have built the program; tests/verify.py's `release` check uses it too.

    python tools/package.py PLATFORM PROGRAM [--out DIR] [--run-folder DIR]
    python tools/package.py --list PLATFORM

PLATFORM is windows-x64, windows-arm64, linux-x64 or linux-arm64; PROGRAM the built mezinxt.exe or mezinxt. The
archive MezinXT-<version>-<platform>.zip (Windows) or .tar.gz (Linux, under a folder of the same name) goes to --out
(default dist/releases/), which holds the current version only: archives of other versions are removed, as is the
earlier archive of this platform. --run-folder also copies the files over
a folder that is used as a run folder (dist/ for windows-x64): its mezinxt.yaml and everything else in it stay.

The file list is fixed below. The program must be an executable of the platform's format and architecture, every
ROM must be the one whose SHA-256 is known (src/core/romdb.c through tools/fetch_roms.py), the manual disk is built
from src/manual-disk/, every relative link of the shipped documents must resolve inside the archive, and the finished
archive is opened again: its members must be exactly the list and their bytes the staged ones. Anything else stops
the build.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import re
import shutil
import struct
import sys
import tarfile
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fetch_roms  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
NAME = "MezinXT"
PLATFORMS = ("windows-x64", "windows-arm64", "linux-x64", "linux-arm64")

# What the archive holds besides the program, the configuration and the ROMs: (archive path, path in the repository).
DOCS = ["README.md", "README.ro.md", "LICENSE"] + [f"docs/{n}" for n in ("THIRD_PARTY.md", "CHANGELOG.md", "NOTES.md", "NOTES.ro.md")]
ROMS = sorted(fetch_roms.IMAGES)
# The lines of a README that show the pictures of assets/ (the logo, centred; the screenshot), which the archive does not hold.
PICTURE_LINE = re.compile(r"^(?:!\[[^\]]*\]\(assets/[^)]*\)|<p align=\"center\">.*\bassets/.*</p>)\r?\n\r?\n", re.M)
MANUAL_DISK = "disks/manual.img"       # built from src/manual-disk/ when the archive is made

# The configuration of the run folder: the manual disk in drive A, the composite monochrome monitor and the CRT look, every
# other setting its default (the first start of a folder without one writes the whole commented file instead).
CONFIG_TEXT = """\
# MezinXT configuration. The settings not listed here have their defaults; docs/NOTES.md lists them all, and
# the setup screen (F12) changes them.

drives:
  a:
    image: disks/manual.img

video:
  look: crt
  monitor: mono-green
"""

EXECUTABLES = {  # format, machine
    "windows-x64": ("pe", 0x8664), "windows-arm64": ("pe", 0xAA64),
    "linux-x64": ("elf", 0x3E), "linux-arm64": ("elf", 0xB7),
}


class PackageError(Exception):
    pass


def version() -> str:
    text = (ROOT / "src" / "VERSION").read_text(encoding="ascii").strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", text):
        raise PackageError(f"src/VERSION must hold MAJOR.MINOR.PATCH, not {text!r}")
    return text


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def program_name(platform: str) -> str:
    return "mezinxt.exe" if platform.startswith("windows") else "mezinxt"


def archive_name(platform: str) -> str:
    return f"{NAME}-{version()}-{platform}" + (".zip" if platform.startswith("windows") else ".tar.gz")


def expected_files(platform: str) -> list[str]:
    """The archive's members (without the Linux folder), sorted: the whole list of what a release holds."""
    return sorted([program_name(platform), "mezinxt.yaml"] + DOCS + [f"roms/{r}" for r in ROMS] + [MANUAL_DISK])


def check_executable(platform: str, path: Path) -> None:
    kind, machine = EXECUTABLES[platform]
    head = path.read_bytes()[:4096]
    if kind == "pe":
        offset = struct.unpack_from("<I", head, 0x3C)[0] if head[:2] == b"MZ" and len(head) > 0x40 else 0
        if head[:2] != b"MZ" or head[offset:offset + 4] != b"PE\0\0" or struct.unpack_from("<H", head, offset + 4)[0] != machine:
            raise PackageError(f"{path} is not a Windows executable for {platform}")
    else:
        if head[:4] != b"\x7fELF" or head[4] != 2 or head[5] != 1 or struct.unpack_from("<H", head, 18)[0] != machine:
            raise PackageError(f"{path} is not a 64-bit little-endian ELF executable for {platform}")


def manual_image() -> bytes:
    """The manual disk, built now from src/manual-disk/ (the application, its topics, the printed pages, the intro picture)."""
    sys.path.insert(0, str(ROOT / "tools" / "manual-disk"))
    import contextlib
    import io
    import build as manual_build
    out = ROOT / "work" / "package" / "manual.img"
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            manual_build.build(out)
    except (SystemExit, Exception) as e:
        raise PackageError(f"the manual disk cannot be built: {e}")
    return out.read_bytes()


def rom_images() -> dict[str, Path]:
    found = {}
    for name in ROMS:
        path = ROOT / "roms" / name
        if not path.is_file():
            raise PackageError(f"roms/{name} is missing: python tools/fetch_roms.py")
        if sha256(path.read_bytes()) != fetch_roms.IMAGES[name][1]:
            raise PackageError(f"roms/{name} is not the image MezinXT knows by that name (SHA-256)")
        found[name] = path
    return found


def gather(platform: str, program: Path) -> dict[str, bytes | Path]:
    """Archive path -> the file or the bytes that go there; every input checked."""
    check_executable(platform, program)
    entries: dict[str, bytes | Path] = {program_name(platform): program, "mezinxt.yaml": CONFIG_TEXT.encode("utf-8")}
    for f in DOCS:
        if not (ROOT / f).is_file():
            raise PackageError(f"{f} is missing")
        entries[f] = ROOT / f
        if f.startswith("README"):
            text = PICTURE_LINE.sub("", (ROOT / f).read_bytes().decode("utf-8"))
            if "](assets/" in text or '="assets/' in text:
                raise PackageError(f"{f} shows a picture of assets/ that is not on a line of its own")
            entries[f] = text.encode("utf-8")
    for name, path in rom_images().items():
        entries[f"roms/{name}"] = path
    entries[MANUAL_DISK] = manual_image()
    if sorted(entries) != expected_files(platform):
        raise PackageError(f"the files gathered differ from the list: {sorted(set(entries) ^ set(expected_files(platform)))}")
    return entries


def content(item: bytes | Path) -> bytes:
    return item if isinstance(item, bytes) else item.read_bytes()


# --- the documents' links ------------------------------------------------------------------------------------------

LINK = re.compile(r"(?<!!)\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")


def broken_links(files: dict[str, bytes | Path] | list[str], read=None) -> list[str]:
    """Relative links of the Markdown files in the list (read: archive path -> text) that point at nothing in it."""
    names = set(files)
    folders = {str(Path(n).parent).replace("\\", "/") for n in names}
    problems = []
    for name in sorted(n for n in names if n.endswith(".md")):
        text = read(name) if read else content(files[name]).decode("utf-8")  # type: ignore[index]
        base = Path(name).parent
        for target in LINK.findall(text):
            if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.I) or target.startswith("#"):
                continue
            path = (base / target.split("#")[0]).as_posix()
            path = re.sub(r"(^|/)\./", r"\1", path).rstrip("/")
            parts = []
            for p in path.split("/"):
                if p == "..":
                    if not parts:
                        break
                    parts.pop()
                elif p and p != ".":
                    parts.append(p)
            else:
                resolved = "/".join(parts)
                if resolved in names or resolved in folders:
                    continue
            problems.append(f"{name}: link to {target}")
    return problems


# --- the archive -----------------------------------------------------------------------------------------------------

def write_archive(platform: str, entries: dict[str, bytes | Path], target: Path) -> None:
    now = time.time()
    if platform.startswith("windows"):
        with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for name in sorted(entries):
                info = zipfile.ZipInfo(name, time.localtime(now)[:6])
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                z.writestr(info, content(entries[name]))
    else:
        top = f"{NAME}-{version()}-{platform}"
        with tarfile.open(target, "w:gz", compresslevel=9, format=tarfile.GNU_FORMAT) as t:
            folders = sorted({str(parent) for n in entries for parent in Path(n).parents})
            for folder in folders:
                info = tarfile.TarInfo(top if folder == "." else f"{top}/{Path(folder).as_posix()}")
                info.type, info.mode, info.mtime = tarfile.DIRTYPE, 0o755, int(now)
                info.uid = info.gid = 0
                info.uname = info.gname = "root"
                t.addfile(info)
            for name in sorted(entries):
                data = content(entries[name])
                info = tarfile.TarInfo(f"{top}/{name}")
                info.size, info.mtime = len(data), int(now)
                info.mode = 0o755 if name == program_name(platform) else 0o644
                info.uid = info.gid = 0
                info.uname = info.gname = "root"
                t.addfile(info, io.BytesIO(data))


def read_back(platform: str, archive: Path) -> dict[str, bytes]:
    """The members of an archive by path (the Linux folder removed), files only."""
    found: dict[str, bytes] = {}
    if platform.startswith("windows"):
        with zipfile.ZipFile(archive) as z:
            if z.testzip():
                raise PackageError(f"{archive}: a member is corrupt")
            for info in z.infolist():
                if info.is_dir():
                    continue
                if info.filename in found or info.filename.startswith(("/", "../")) or "\\" in info.filename:
                    raise PackageError(f"{archive}: bad member name {info.filename!r}")
                found[info.filename] = z.read(info)
    else:
        top = f"{NAME}-{version()}-{platform}/"
        with tarfile.open(archive, "r:gz") as t:
            for info in t:
                if info.isdir():
                    continue
                if not info.isfile() or not info.name.startswith(top):
                    raise PackageError(f"{archive}: unexpected member {info.name!r}")
                name = info.name[len(top):]
                if name in found or ".." in name.split("/"):
                    raise PackageError(f"{archive}: bad member name {info.name!r}")
                found[name] = t.extractfile(info).read()  # type: ignore[union-attr]
    return found


def package(platform: str, program: Path, out: Path, run_folder: Path | None = None) -> Path:
    entries = gather(platform, program)
    problems = broken_links(entries)
    if problems:
        raise PackageError("links that lead nowhere in the archive:\n  " + "\n  ".join(problems))
    out.mkdir(parents=True, exist_ok=True)
    final = out / archive_name(platform)
    work = ROOT / "work" / "package"
    work.mkdir(parents=True, exist_ok=True)
    temp = work / archive_name(platform)
    write_archive(platform, entries, temp)
    back = read_back(platform, temp)
    if sorted(back) != expected_files(platform):
        raise PackageError(f"the archive holds {sorted(set(back) ^ set(expected_files(platform)))} beyond or short of the list")
    for name, item in entries.items():
        if back[name] != content(item):
            raise PackageError(f"the archive's {name} differs from the staged one")
    # the current version only: every archive of another version goes
    for old in out.glob(f"{NAME}-*.*"):
        if re.fullmatch(rf"{NAME}-\d+\.\d+\.\d+-[a-z]+-[a-z0-9]+\.(zip|tar\.gz)", old.name) and not old.name.startswith(f"{NAME}-{version()}-"):
            old.unlink()
    shutil.move(str(temp), final)
    if run_folder is not None:
        for name in sorted(entries):
            target = run_folder / name
            if name == "mezinxt.yaml" and target.exists():
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content(entries[name]))
    print(f"{NAME} {version()} {platform}")
    for name in expected_files(platform):
        print(f"  {name:<56} {len(content(entries[name])):>12,} bytes")
    print(f"OK: {final} ({final.stat().st_size:,} bytes)")
    return final


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("platform", choices=PLATFORMS)
    parser.add_argument("program", nargs="?", type=Path)
    parser.add_argument("--out", type=Path, default=ROOT / "dist" / "releases")
    parser.add_argument("--run-folder", type=Path)
    parser.add_argument("--list", action="store_true", help="print the files of the archive and stop")
    args = parser.parse_args(argv)
    if args.list:
        print("\n".join(expected_files(args.platform)))
        return 0
    if not args.program:
        parser.error("the built program is required")
    try:
        package(args.platform, args.program, args.out, args.run_folder)
    except PackageError as e:
        print(f"package: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

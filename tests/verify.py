"""MezinXT's quick checks, beside ctest (tools/build.ps1): one short smoke check for each part, run on the program itself.

    python tests/verify.py            the quick tier
    python tests/verify.py --list     list the checks
    python tests/verify.py NAME...    run only these checks
    MEZINXT_BUILD=DIR python tests/verify.py   the programs built in DIR (tools/build_linux.sh)

Checks:
  post       BIOS 4.0 runs its POST headless (--trace): the DIP switches read, the CGA programmed, the keyboard's
             reset answered, the diskette controller reached (skipped without roms/)
  cga        the POST's text screen, saved headless and read back with the character ROM: the title, the three
             checks and the 640 KiB count (skipped without roms/)
  sound      the end-of-POST beep is in the recorded WAV (skipped without roms/)
  mda        the same POST on the monochrome coupler (machine.display: mda): a 720x350 screenshot whose text rows
             hold as many characters as the CGA's (skipped without roms/)
  manual     the manual disk (floppy): built from src/manual-disk/ and booted headless, it shows its intro picture
             (skipped without roms/)
  setup      the setup screen draws a page headless, and a setting changed by scripted keys is saved to
             mezinxt.yaml with the template's comments kept (skipped without the character ROM)
  cpu-suite  SingleStepTests' 8088 and 8086 suites (tests/cpu_suite.py), a sample of 200 tests per opcode file:
             every result right (skipped when the suites are not fetched; the whole suites: tests/cpu_suite.py run)
  release    (only by name: it needs the archive) this host's archive in dist/releases/ (tools/build_dist.ps1 or
             tools/build_linux.sh) extracted into a fresh folder: exactly the release's files, and with nothing
             from the repository the manual disk boots to its application headless

The fixed disk, the serial and parallel ports, the 8087, the configuration and the CPU's hand-written cases are
ctest programs (tests/).

The program is build/x64/mezinxt.exe (tools/build.ps1), or mezinxt in the folder MEZINXT_BUILD names
(tools/build_linux.sh). Everything is written under work/verify.
"""

from __future__ import annotations

import contextlib
import functools
import hashlib
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

ROOT = Path(__file__).resolve().parents[1]
# The built programs: build/x64, or the folder MEZINXT_BUILD names (tools/build_linux.sh builds elsewhere).
BUILD = Path(os.environ.get("MEZINXT_BUILD") or ROOT / "build" / "x64")
EXE = BUILD / ("mezinxt.exe" if os.name == "nt" else "mezinxt")
WORK = ROOT / "work" / "verify"
FRAMES = 720                                         # twelve seconds of guest time: the POST's memory count is done
NO_ROMS = "skipped (no roms/: python tools/fetch_roms.py)"


class Failure(Exception):
    pass


def run(args, cwd=ROOT, env=None, timeout=120, code=0):
    p = subprocess.run([str(EXE)] + [str(a) for a in args], cwd=cwd, env=env, capture_output=True, text=True, timeout=timeout)
    if code is not None and p.returncode != code:
        raise Failure(f"mezinxt {' '.join(map(str, args))}: exit {p.returncode}\n{p.stdout}{p.stderr}")
    return p


def folder(name: str) -> Path:
    d = WORK / name
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    return d


def config(d: Path, text: str) -> Path:
    """A mezinxt.yaml in d: the colour monitor (the default is the composite one) and ui.language en (the default
    language is the host's), unless the text names them."""
    if "monitor:" not in text:
        text = text.replace("video:\n", "video:\n  monitor: rgbi\n", 1) if re.search(r"^video:\n", text, re.M) else \
            text + ("" if text.endswith("\n") or not text else "\n") + "video:\n  monitor: rgbi\n"
    if "ui:" not in text:
        text += ("" if text.endswith("\n") or not text else "\n") + "ui:\n  language: en\n"
    path = d / "mezinxt.yaml"
    path.write_text(text, encoding="utf-8")
    return path


def read_bmp(path: Path):
    """(width, height, rows of 0xRRGGBB) of a 24- or 32-bit uncompressed BMP."""
    b = path.read_bytes()
    if b[:2] != b"BM":
        raise Failure(f"{path}: not a BMP")
    offset, = struct.unpack_from("<I", b, 10)
    width, height = struct.unpack_from("<ii", b, 18)
    bpp, = struct.unpack_from("<H", b, 28)
    step = bpp // 8
    pitch = (width * step + 3) & ~3
    rows = []
    for y in range(abs(height)):
        line = offset + (abs(height) - 1 - y if height > 0 else y) * pitch
        rows.append([b[line + x * step + 2] << 16 | b[line + x * step + 1] << 8 | b[line + x * step] for x in range(width)])
    return width, abs(height), rows


def roms_present(*names: str) -> bool:
    return all((ROOT / "roms" / n).is_file() for n in names)


def chargen_table(rom: bytes) -> dict[bytes, int]:
    """Glyph rows (8 bytes) to character code in the Junior's character ROM; blank is a space."""
    table: dict[bytes, int] = {}
    for code in range(256):
        table.setdefault(rom[code * 8:code * 8 + 8], code)
    table[bytes(8)] = 0x20
    return table


def screen_cells(rows, table: dict[bytes, int]) -> list[list]:
    """The character code of every 8x8 cell of a 640x200 frame (any two colours: the cell's ink is the colour that
    makes a glyph of the ROM); None where no glyph of the ROM fits."""
    cells = []
    for r in range(25):
        line = []
        for c in range(80):
            colours = sorted({rows[r * 8 + y][c * 8 + x] for y in range(8) for x in range(8)})
            code = 0x20 if len(colours) == 1 else None
            for ink in colours if len(colours) == 2 else []:
                bits = bytes(sum(1 << (7 - x) for x in range(8) if rows[r * 8 + y][c * 8 + x] == ink) for y in range(8))
                if bits in table:
                    code = table[bits]
                    break
            line.append(code)
        cells.append(line)
    return cells


def cell_text(cells, r: int) -> str:
    return "".join(chr(c) if c and 0x20 <= c < 0x7F else "?" for c in cells[r])


def cell_screen(shot: Path, glyphs: bytes) -> list[str]:
    """The 25 rows of an 80x25 text screen in a 640x200 BMP, whatever the colours of each cell."""
    w, h, rows = read_bmp(shot)
    if (w, h) != (640, 200):
        raise Failure(f"{shot}: the picture is {w}x{h}, not 640x200")
    cells = screen_cells(rows, chargen_table(glyphs))
    return [cell_text(cells, r).rstrip() for r in range(25)]


def wav_samples(path: Path) -> list[int]:
    with wave.open(str(path)) as w:
        data = w.readframes(w.getnframes())
    return list(struct.unpack(f"<{len(data) // 2}h", data))


# --- one boot of the Junior, shared by the checks that look at it ----------------------------------------------

@functools.lru_cache(maxsize=None)
def boot(display: str):
    """BIOS 4.0 on the 8086 with 640 KiB and no diskette, FRAMES frames: the I/O trace, the WAV and the screenshot."""
    d = folder(f"boot-{display}")
    yaml = config(d, f"machine:\n  bios: 4.0\n  cpu: 8086\n  ram_kb: 640\n  display: {display}\nvideo:\n  look: plain\n")
    trace, wav, shot = d / "io.txt", d / "post.wav", d / "screen.bmp"
    out = run(["--config", yaml, "--hidden", "--frames", FRAMES, "--trace", trace, "--wav", wav, "--screenshot", shot], timeout=300).stdout
    if "JUNIOR-XT, BIOS 4.0" not in out:
        raise Failure(f"not the Junior's board (roms/ without BIOS 4.0?):\n{out}")
    return trace, wav, shot


def post_text() -> list[str]:
    """The lines of the POST's text screen (grey on black) on the CGA, as the character ROM spells them."""
    w, h, rows = read_bmp(boot("cga80")[2])
    if (w, h) != (640, 200):
        raise Failure(f"the screenshot is {w}x{h}, not 640x200")
    cells = screen_cells(rows, chargen_table((ROOT / "roms" / "chargen.rom").read_bytes()))
    return [cell_text(cells, r).rstrip() for r in range(25)]


def check_post():
    if not roms_present("bios-4.0.rom"):
        return NO_ROMS
    lines = boot("cga80")[0].read_text().splitlines()

    def first(pred):
        return next((i for i, l in enumerate(lines) if pred(l.split())), None)
    switches = first(lambda f: f[:2] == ["IN", "0062"])
    keyboard = first(lambda f: f == ["IN", "0060", "AA"])
    diskette = first(lambda f: f[:2] == ["OUT", "03F2"] and int(f[2], 16) & 4)
    crtc = first(lambda f: f[:2] == ["OUT", "03D4"])
    if None in (switches, keyboard, diskette, crtc) or not switches < keyboard < diskette:
        raise Failure(f"POST incomplete: switches read at {switches}, keyboard AAh at {keyboard}, diskette at {diskette}, "
                      f"CRTC at {crtc} (trace lines); last: {lines[-5:]}")
    return "switches, CGA, keyboard, diskette controller reached"


def check_cga():
    if not roms_present("bios-4.0.rom", "chargen.rom"):
        return NO_ROMS
    text = post_text()
    want = ["JUNIOR-XT Computer V4.00", "Intr    Ok", "Timer   Ok", "Kbd     Ok", "640K RAM"]
    if text[:5] != want:
        raise Failure(f"the POST screen reads {text[:6]}, expected {want}")
    return "the POST screen reads back in the Junior's glyphs"


def check_sound():
    if not roms_present("bios-4.0.rom"):
        return NO_ROMS
    samples = wav_samples(boot("cga80")[1])
    peak = max((abs(v) for v in samples), default=0)
    loud = sum(1 for v in samples if abs(v) > peak // 4)
    if peak < 1000 or loud < 2000:
        raise Failure(f"no end-of-POST beep in the recording (peak {peak}, {loud} loud samples)")
    return f"the end-of-POST beep is recorded (peak {peak})"


def check_mda():
    if not roms_present("bios-4.0.rom", "chargen.rom"):
        return NO_ROMS
    w, h, rows = read_bmp(boot("mda")[2])
    if (w, h) != (720, 350):
        raise Failure(f"the monochrome coupler's screenshot is {w}x{h}, not 720x350")
    cga = post_text()
    # A text cell is 9x14: the cells of each of the first five rows that hold a lit point are the characters the CGA shows there.
    for r in range(5):
        lit = sum(1 for c in range(80) if any(rows[r * 14 + y][c * 9 + x] for y in range(14) for x in range(9)))
        want = sum(1 for ch in cga[r] if ch != " ")
        if lit != want:
            raise Failure(f"row {r}: {lit} characters on the monochrome coupler, {want} on the CGA ({cga[r]!r})")
    return "the POST's first five rows on a 720x350 screen"


# --- the manual disk ---------------------------------------------------------------------------------------------

def manual_disk(out: Path) -> bytes:
    """The manual disk as tools/manual-disk/build.py makes it."""
    sys.path.insert(0, str(ROOT / "tools" / "manual-disk"))
    import build as manual_build
    out.parent.mkdir(parents=True, exist_ok=True)
    with contextlib.redirect_stdout(io.StringIO()):
        manual_build.build(out)
    return out.read_bytes()


def check_manual():
    if not roms_present("bios-4.0.rom", "chargen.rom"):
        return NO_ROMS
    d = folder("manual")
    manual_disk(d / "a.img")
    yaml = config(d, "machine:\n  bios: 4.0\n  cpu: 8086\n  ram_kb: 640\ndrives:\n  a:\n    image: a.img\nvideo:\n  look: plain\nsound:\n  enabled: false\n")
    shot = d / "intro.bmp"
    run(["--config", yaml, "--hidden", "--frames", 1500, "--screenshot", shot], timeout=300)
    w, h, rows = read_bmp(shot)
    colours = {p for row in rows for p in row}
    # The POST's text is grey on black (two colours); the intro is a picture in many.
    if (w, h) != (640, 200) or len(colours) < 8:
        raise Failure(f"the manual disk did not reach its intro: the picture has {len(colours)} colours ({w}x{h})")
    return f"the manual disk boots to its intro picture ({len(colours)} colours)"


# --- the setup screen --------------------------------------------------------------------------------------------

def setup_run(d: Path, page: str, keys: str, shot: str | None = None):
    """mezinxt on the setup page with scripted keys, one a frame (the XT key codes of src/core/machine.h)."""
    named = {"down": 0x50, "up": 0x48, "enter": 0x1C, "esc": 0x01, "pgup": 0x49, "pgdn": 0x51}
    words = keys.split()
    args = ["--config", d / "mezinxt.yaml", "--hidden", "--frames", len(words) + 6, "--setup-page", page]
    for i, word in enumerate(words):
        args += ["--key", f"{named[word]:02X}@{i + 1}"]
    if shot:
        args += ["--screenshot", d / f"{shot}.bmp"]
    return run(args)


def check_setup():
    if not roms_present("chargen.rom"):
        return NO_ROMS
    version = (ROOT / "src" / "VERSION").read_text().strip()
    # A page, drawn: the title, the menu bar, the labels and the double frame.
    d = folder("setup")
    config(d, "video:\n  look: plain\nsound:\n  enabled: false\n")
    setup_run(d, "sound", "", shot="sound")
    w, h, rows = read_bmp(d / "sound.bmp")
    if (w, h) != (640, 200):
        raise Failure(f"the setup page is {w}x{h}, not 640x200")
    cells = screen_cells(rows, chargen_table((ROOT / "roms" / "chargen.rom").read_bytes()))
    title, bar, body = cell_text(cells, 0), cell_text(cells, 1), "\n".join(cell_text(cells, r) for r in range(3, 21))
    if f"MezinXT {version} - Setup" not in title or "Drives" not in bar or "Sound" not in bar or "Volume" not in body:
        raise Failure(f"the Sound page reads {title!r}, {bar!r}, {body!r}")
    if [cells[2][0], cells[2][79], cells[21][0], cells[21][79]] != [0xC9, 0xBB, 0xC8, 0xBC]:
        raise Failure("the setup screen's double frame is not whole")
    # A setting changed by scripted keys (mute, volume 80) is saved with every comment of the template kept: the template as
    # a first start writes it, drive A empty (no disks/manual.img here).
    template = (ROOT / "src" / "app" / "mezinxt.yaml").read_text(encoding="utf-8")
    template = template.replace("image: disks/manual.img", "image:" + " " * 17, 1)
    comments = lambda text: [line[line.index("#"):].rstrip() for line in text.splitlines() if "#" in line]
    d = folder("setup-change")
    path = config(d, template)
    p = setup_run(d, "sound", "down pgup pgup up enter esc")
    text = path.read_text(encoding="utf-8")
    if p.stderr.strip() or "enabled: false" not in text or "volume: 80" not in text:
        raise Failure(f"the sound settings were not saved to mezinxt.yaml:\n{p.stderr}{text}")
    if [c for c in comments(template) if c not in comments(text)]:
        raise Failure("comments of the template were lost from mezinxt.yaml")
    return "the Sound page is drawn; mute and volume are saved with the template's comments kept"


# --- the CPU suites, and the release -------------------------------------------------------------------------------

def check_cpu_suite():
    import cpu_suite
    runner = BUILD / ("cpu_suite.exe" if os.name == "nt" else "cpu_suite")
    cpus = [c for c in cpu_suite.SUITES if cpu_suite.fetched(c)]
    if not cpus:
        return "skipped (not fetched: python tests/cpu_suite.py fetch)"
    out = []
    for cpu in cpus:
        p = subprocess.run([str(runner), str(cpu_suite.folder(cpu)), "--quiet", "--sample", "200"], capture_output=True, text=True, timeout=300)
        summary = p.stdout.strip().splitlines()[-1] if p.stdout.strip() else p.stderr.strip()
        if p.returncode:
            raise Failure(f"{cpu}: {p.stdout[-2000:]}{p.stderr}")
        m = re.search(r"(\d+) tests: results \d+ \(([\d.]+)%\), cycles\+queue \d+ \(([\d.]+)%\)", summary)
        out.append(f"{cpu} {m.group(1)} tests, results {m.group(2)}%, cycles {m.group(3)}%" if m else summary)
    return "; ".join(out)


def release_platform() -> str:
    import platform
    arm = platform.machine().lower() in ("arm64", "aarch64")
    return ("windows-" if os.name == "nt" else "linux-") + ("arm64" if arm else "x64")


def check_release():
    """A fresh extraction of this host's archive in dist/releases/ starts the manual disk, using no file of the repository (the ROMs,
    the character ROM that reads the screen back and the disk are the archive's own)."""
    import package
    platform_name = release_platform()
    archive = ROOT / "dist" / "releases" / package.archive_name(platform_name)
    if not archive.is_file():
        raise Failure(f"{archive} is not there: build it first (tools/build_dist.ps1 or tools/build_linux.sh)")
    d = folder("release")
    members = package.read_back(platform_name, archive)
    if sorted(members) != package.expected_files(platform_name):
        raise Failure(f"the archive's members are not the release's list: {sorted(set(members) ^ set(package.expected_files(platform_name)))}")
    for name, data in members.items():
        (d / name).parent.mkdir(parents=True, exist_ok=True)
        (d / name).write_bytes(data)
    program = d / package.program_name(platform_name)
    if os.name != "nt":
        program.chmod(0o755)

    def start(*args, driver=None, timeout=300):
        env = dict(os.environ)
        if driver and CHOOSE_DRIVER:        # the shader needs OpenGL, which headless Linux has only on the offscreen driver
            env["SDL_VIDEODRIVER"] = driver
        p = subprocess.run([str(program), *map(str, args)], cwd=d, env=env, capture_output=True, text=True, timeout=timeout)
        if p.returncode:
            raise Failure(f"{program.name} {' '.join(map(str, args))}: exit {p.returncode}\n{p.stdout}{p.stderr}")
        return p.stdout

    version = (ROOT / "src" / "VERSION").read_text().strip()
    if start("--version").strip() != f"MezinXT {version}":
        raise Failure("the extracted program is not this version")
    # The archive's own configuration as it ships (the CRT look): the machine starts and shows a picture.
    start("--hidden", "--frames", 30, "--screenshot", d / "shipped.bmp", driver="offscreen")
    if read_bmp(d / "shipped.bmp")[:2] != (1280, 960):
        raise Failure("the archive's own configuration did not give the CRT look's picture")
    # The same with the plain look and the colour monitor, so that the screen can be read back with the archive's own character ROM.
    shipped = (d / "mezinxt.yaml").read_text(encoding="utf-8")
    if shipped != package.CONFIG_TEXT or "image: disks/manual.img" not in shipped or "look: crt" not in shipped or "monitor: mono-green" not in shipped:
        raise Failure(f"the archive's mezinxt.yaml is not the manual disk in drive A with the composite monitor and the CRT look: {shipped!r}")
    plain = shipped.replace("look: crt", "look: plain").replace("monitor: mono-green", "monitor: rgbi")
    (d / "plain.yaml").write_text(plain + "sound:\n  enabled: false\nui:\n  language: en\n", encoding="utf-8")
    shot = d / "boot.bmp"
    # A key passes the intro picture: the manual application's home topic.
    start("--config", d / "plain.yaml", "--hidden", "--frames", 1900, "--key", "1C@1600", "--key", "9C@1603", "--screenshot", shot)
    rows = cell_screen(shot, (d / "roms" / "chargen.rom").read_bytes())
    if "F1 Prezentare" not in rows[0] or not rows[1].strip():
        raise Failure(f"the manual disk did not start its application from the extracted archive: {rows[:3]}")
    if hashlib.sha256((d / "disks" / "manual.img").read_bytes()).hexdigest() != hashlib.sha256(members["disks/manual.img"]).hexdigest():
        raise Failure("a boot that wrote nothing changed the manual disk")
    return (f"{archive.name} ({archive.stat().st_size:,} bytes, {len(members)} files) extracted: the manual disk boots to its application "
            "with the archive's own ROMs and configuration")


CHECKS = {
    "post": check_post,
    "cga": check_cga,
    "sound": check_sound,
    "mda": check_mda,
    "manual": check_manual,
    "setup": check_setup,
    "cpu-suite": check_cpu_suite,
    "release": check_release,
}
# Not in the quick tier: it needs the archive that tools/build_dist.ps1 or tools/build_linux.sh makes.
NAMED_ONLY = {"release"}
CHOOSE_DRIVER = False


def main(argv: list[str]) -> int:
    if "--list" in argv:
        for name in CHECKS:
            print(name)
        return 0
    names = [a for a in argv if not a.startswith("-")] or [n for n in CHECKS if n not in NAMED_ONLY]
    unknown = [n for n in names if n not in CHECKS]
    if unknown:
        print(f"unknown check(s): {', '.join(unknown)}")
        return 2
    if not EXE.exists():
        print(f"{EXE} not found: build it first (tools/build.ps1)")
        return 2
    WORK.mkdir(parents=True, exist_ok=True)
    # Where there is no window system (Linux, headless) the checks run on SDL's dummy video driver (the software renderer);
    # SDL_VIDEODRIVER overrides.
    global CHOOSE_DRIVER
    CHOOSE_DRIVER = os.name != "nt" and not os.environ.get("SDL_VIDEODRIVER")
    if CHOOSE_DRIVER:
        os.environ["SDL_VIDEODRIVER"] = "dummy"
    failed = 0
    for name in names:
        try:
            print(f"PASS {name}: {CHECKS[name]()}", flush=True)
        except (Failure, subprocess.TimeoutExpired, OSError) as e:
            failed += 1
            print(f"FAIL {name}: {e}", flush=True)
    print("all checks passed" if not failed else f"{failed} check(s) failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

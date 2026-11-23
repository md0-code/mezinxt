"""Build the manual disk: a 360 KB self-booting diskette with the manual application.

    python tools/manual-disk/build.py [--out FILE.img]

The application (src/manual-disk/app.asm, tests.asm), its topics (src/manual-disk/app/), the printed manual's
pages (src/manual-disk/text/) and the pictures (src/manual-disk/pictures/) are laid out into the image. Without --out the image goes to work/manual-disk/manual.img. MezinXT's own.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SRC = ROOT / "src" / "manual-disk"
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(HERE))
from asm86 import assemble, AsmError  # noqa: E402
import topics as T  # noqa: E402

SECTOR = 512
SIZE = 40 * 2 * 9 * SECTOR
TABLE = 0x8000                      # where the application keeps the directory, in its segment
ENTRY = 12
MARK = re.compile(r"\{(/?[bu]|fig [a-z0-9-]+|\(|\))\}")
FIGURES = {"tastatura": "tastatura", "comutatoare": "sistem", "culori": "video"}     # a page's figure line opens a test
INVERSE = {"je": "jne", "jne": "je", "jz": "jnz", "jnz": "jz", "jb": "jae", "jae": "jb", "ja": "jbe", "jbe": "ja",
           "jc": "jnc", "jnc": "jc"}


def assemble_long(source: str) -> bytes:
    """The assembler's conditional jumps are short: one that does not reach is turned into its opposite over a near jump."""
    for _ in range(400):
        try:
            return assemble(source)
        except AsmError as e:
            m = re.match(r"(\w+) (\w+): the target is", str(e))
            if not m or m.group(1) not in INVERSE:
                raise
            jump, label = m.groups()
            pattern = re.compile(rf"^(\w+:)?(\s+){jump} {label}\s*$", re.M)
            if not pattern.search(source):
                raise
            source = pattern.sub(lambda mm: (mm.group(1) or "") + mm.group(2) + f"{INVERSE[jump]} $+5\n        jmp {label}", source)
    raise SystemExit("the jumps do not settle")


def key_table() -> str:
    """The 83 keys as the test draws them: row, column, width and three characters of name, by scan code."""
    keys = {}

    def row(n, column, width, gap, names_codes):
        for name, code in names_codes:
            w = width if not isinstance(name, tuple) else name[1]
            name = name if not isinstance(name, tuple) else name[0]
            keys[code] = (n, column, w, name.center(3)[:3])
            column += w + gap

    f = [("F1", 0x3B), ("F2", 0x3C), ("F3", 0x3D), ("F4", 0x3E), ("F5", 0x3F), ("F6", 0x40), ("F7", 0x41), ("F8", 0x42),
         ("F9", 0x43), ("F10", 0x44)]
    for i in range(5):
        row(i, 1, 3, 1, f[i * 2:i * 2 + 2])
    letters = lambda text, first: [(c, first + i) for i, c in enumerate(text)]
    row(0, 10, 3, 1, [("Esc", 1)] + letters("1234567890-=", 2) + [("BS", 0x0E)])
    row(1, 10, 3, 1, [("Tab", 0x0F)] + letters("QWERTYUIOP[]", 0x10) + [("Ent", 0x1C)])
    row(2, 10, 3, 1, [("Ctl", 0x1D)] + letters("ASDFGHJKL;'`", 0x1E))
    row(3, 10, 3, 1, [("Shf", 0x2A)] + letters("\\ZXCVBNM,./", 0x2B) + [("Shf", 0x36), ("PrS", 0x37)])
    row(4, 10, 3, 1, [("Alt", 0x38), (("Spatiu", 43), 0x39), ("Cap", 0x3A)])
    row(0, 67, 2, 1, [(("NumL", 5), 0x45), (("ScrL", 5), 0x46)])
    row(1, 67, 2, 1, [("7", 0x47), ("8", 0x48), ("9", 0x49), ("-", 0x4A)])
    row(2, 67, 2, 1, [("4", 0x4B), ("5", 0x4C), ("6", 0x4D), ("+", 0x4E)])
    row(3, 67, 2, 1, [("1", 0x4F), ("2", 0x50), ("3", 0x51)])
    row(4, 67, 2, 1, [(("0", 5), 0x52), ((".", 5), 0x53)])
    lines = ["keytab:"]
    for code in range(1, 84):
        n, column, w, name = keys[code]
        if w < 3:                               # a narrow cap shows one character
            name = name.strip()[:1].ljust(3)
            w = 3 if False else w
        lines.append(f"        db {n}, {column}, {max(w, 3)}, {ord(name[0])}, {ord(name[1])}, {ord(name[2])}")
    return "\n".join(lines) + "\n"


def plain(line: str) -> str:
    return MARK.sub(lambda m: {"(": "{", ")": "}"}.get(m.group(1), ""), line)


def read_pages(folder: Path):
    pages = []
    for path in sorted(folder.glob("p[0-9][0-9][0-9].txt")):
        lines = path.read_text(encoding="latin-1").split("\n")
        while lines and not lines[-1].strip():
            lines.pop()
        pages.append(dict(number=int(path.stem[1:]), lines=lines))
    if not pages:
        raise SystemExit(f"no pages in {folder}")
    return pages


def pack_page_line(line: str, test_index) -> bytes:
    """A line of the printed manual for the reader: bold bright, underlined cyan, a figure line a link to its test."""
    m = re.fullmatch(r"\s*\{fig ([a-z0-9-]+)\}\s*", line)
    if m:
        test = FIGURES.get(m.group(1))
        if test is None:
            return b"\0"
        text = "\x10 Figura: verificare pe microcalculator".encode("cp437")
        return bytes([4, 4, 2, T.TEST, test_index(test), 0, 0]) + text + b"\x03\0"
    out = bytearray()
    bold = under = False
    pos = 0
    for m in list(MARK.finditer(line)) + [None]:
        text = line[pos:m.start() if m else len(line)]
        if text:
            out += T.runs(text.encode("ascii"))
        if m is None:
            break
        pos = m.end()
        mark = m.group(1)
        if mark in "()":
            out.append(ord("{" if mark == "(" else "}"))
            continue
        if mark[-1] == "b":
            bold = not mark.startswith("/")
        else:
            under = not mark.startswith("/")
        out += bytes([1, 0x1E if bold and under else 0x1F if bold else 0x1B if under else T.NORMAL])
    while len(out) >= 2 and out[-2] == 4:
        del out[-2:]
    return bytes(out) + b"\0"


def find(pages, pattern, after=0):
    rx = re.compile(pattern)
    for index, page in enumerate(pages):
        if page["number"] <= after:
            continue
        for n, line in enumerate(page["lines"]):
            if rx.search(plain(line)):
                return index, n
    return None


def contents(pages):
    """The printed contents (page 3) as entries: (page index, line, column, bold, text)."""
    first = next((p for p in pages if p["number"] == 3), None)
    entries, missing = [], []
    if first is None:
        return entries, missing
    for raw in first["lines"]:
        text = plain(raw).rstrip()
        if not text.strip() or text.strip() == "CUPRINS":
            continue
        indent = len(text) - len(text.lstrip())
        text = re.sub(r"\s+", " ", text.strip())
        m = re.match(r"(CAPITOLUL|ANEXA) (\d+)\.", text)
        if m:
            if m.group(1) == "ANEXA" and m.group(2) == "4":
                continue                        # the schematics are not on the disk
            where = find(pages, rf"^\s*{m.group(1)} {m.group(2)}\b", after=3)
            entry = [where, 2, True, text]
        else:
            m = re.match(r"(\d+(?:\.\d+)+)\.? ?", text)
            if not m:
                if entries:                     # the rest of the entry above
                    last = entries[-1]
                    last[3] = last[3][:-1] + text if last[3].endswith("-") else last[3] + " " + text
                continue
            where = find(pages, rf"^\s*{re.escape(m.group(1))}\.? ", after=3)
            entry = [where, 6 if indent < 10 else 10, False, text]
        if entry[0] is None:
            missing.append(text)
            entry[0] = (entries[-1][0] if entries else (0, 0))
        entries.append(entry)
    out = []
    for where, column, bold, text in entries:
        room = 76 - column
        if len(text) > room:
            text = text[:room - 3].rsplit(" ", 1)[0] + "..."
        out.append((where[0], where[1], column, bold, text))
    return out, missing


def build(out: Path, text: Path = SRC / "text", app: Path = SRC / "app"):
    topics = T.parse(app)
    pages = read_pages(text)
    entries, missing = contents(pages)
    ids = {t["id"]: i for i, t in enumerate(topics)}
    cuprins = len(topics)
    first_page = cuprins + 1
    page_doc = {p["number"]: first_page + i for i, p in enumerate(pages)}

    def resolve(target, where):
        if target.startswith("pag:"):
            n = int(target[4:])
            if n not in page_doc:
                n = min((k for k in page_doc if k >= n), default=max(page_doc))
            return T.DOC, page_doc[n], 0
        if target.startswith("pic:"):
            return T.PICTURE, T.PICTURES.index(target[4:]), 0
        if target.startswith("test:"):
            return T.TEST, T.TESTS.index(target[5:]), 0
        if target not in ids:
            raise T.Problem(f"{where}: no topic {target}")
        return T.DOC, ids[target], 0

    docs = []                                   # dicts: data, nlines, section, pageno, prev, next
    for i, topic in enumerate(topics):
        data = T.runs(T.cp437(topic["title"], topic["where"])) + b"\0"
        data += b"".join(T.pack_line(line, where, resolve) for line, where in topic["lines"])
        section = T.SECTIONS.index(topic["section"])
        prev = i - 1 if i and topics[i - 1]["section"] == topic["section"] else 0xFFFF
        nxt = i + 1 if i + 1 < len(topics) and topics[i + 1]["section"] == topic["section"] else 0xFFFF
        docs.append(dict(data=data, nlines=len(topic["lines"]) + 1, section=section, pageno=0, prev=prev, next=nxt))
    carte = T.SECTIONS.index("carte")
    lines = [T.runs(b"Carte tehnica  -  cuprins") + b"\0", b"\0"]
    for page, line, column, bold, label in entries:
        lines.append(bytes([4, column]) + (bytes([1, 0x1F]) if bold else b"")
                     + bytes([2, T.DOC, (first_page + page) & 255, (first_page + page) >> 8, min(line, 255)])
                     + label.encode("ascii") + b"\x03\0")
    lines += [b"\0", bytes([4, 2, 2, T.DOC, page_doc[3] & 255, page_doc[3] >> 8, 0]) + b"Cuprinsul tiparit (pagina 3)\x03\0"]
    docs.append(dict(data=b"".join(lines), nlines=len(lines), section=carte, pageno=0, prev=0xFFFF, next=first_page))
    for i, page in enumerate(pages):
        data = T.runs(f"Carte tehnica  -  pagina {page['number']}".encode("ascii")) + b"\0"
        data += b"".join(pack_page_line(line, T.TESTS.index) for line in page["lines"])
        docs.append(dict(data=data, nlines=len(page["lines"]) + 1, section=carte, pageno=page["number"],
                         prev=first_page + i - 1 if i else cuprins, next=first_page + i + 1 if i + 1 < len(pages) else 0xFFFF))
    for i, doc in enumerate(docs):
        if doc["nlines"] > 255:
            raise SystemExit(f"topic {i}: {doc['nlines']} lines, the reader takes 255")

    boot = bytearray(assemble((SRC / "boot.asm").read_text(encoding="utf-8")))
    source = "\n".join((SRC / name).read_text(encoding="utf-8") for name in ("app.asm", "tests.asm")) + "\n" + key_table()
    program = bytearray(assemble_long(source))
    if len(program) > TABLE:
        raise SystemExit(f"the application is {len(program)} bytes, the directory starts at {TABLE}")
    program_sectors = -(-len(program) // SECTOR)
    table_at = 1 + program_sectors
    table_sectors = -(-len(docs) * ENTRY // SECTOR)
    if TABLE + table_sectors * SECTOR > 0xC000:          # the screen copies start there
        raise SystemExit("the directory does not fit the application's segment")
    sector = table_at + table_sectors
    pictures = []
    body = bytearray()
    for name in T.PICTURES:
        data = (SRC / "pictures" / f"{name}.pic").read_bytes()
        if len(data) != 8000:
            raise SystemExit(f"{name}.pic is {len(data)} bytes, not 8000")
        pictures.append(sector + len(body) // SECTOR)
        body += data.ljust(16 * SECTOR, b"\0")
    directory = bytearray()
    for doc in docs:
        at = len(body)
        body += doc["data"]
        offset = at % SECTOR
        directory += struct.pack("<HBBBBHHH", sector + at // SECTOR, -(-(offset + len(doc["data"])) // SECTOR), doc["nlines"],
                                 doc["section"], doc["pageno"], doc["prev"], doc["next"], offset)
    used = sector * SECTOR + len(body)
    if used > SIZE:
        raise SystemExit(f"the disk is full: {used} bytes of {SIZE}")
    if program[0] != 0xE9:
        raise SystemExit("the application does not start with a near jump")
    starts = [ids["acasa"]] + [ids[s] for s in T.SECTIONS[1:6]] + [cuprins]
    struct.pack_into("<HBH7H2H", program, 3, table_at, table_sectors, len(docs), *starts, *(pictures + [0])[:2])
    at = boot.index(b"Eroare") - 2
    struct.pack_into("<H", boot, at, program_sectors)
    image = bytearray(SIZE)
    image[0:SECTOR] = boot
    image[SECTOR:SECTOR + len(program)] = program
    image[table_at * SECTOR:table_at * SECTOR + len(directory)] = directory
    image[sector * SECTOR:sector * SECTOR + len(body)] = body
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(image)
    print(f"{out}: {len(topics)} topics, {len(pages)} pages, application {len(program)} bytes, {used} bytes used, {SIZE - used} free")
    for label in missing:
        print(f"  no heading found for: {label}")
    return dict(topics=topics, pages=pages, docs=docs, entries=entries, used=used, first_page=first_page, ids=ids)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=ROOT / "work" / "manual-disk" / "manual.img")
    args = ap.parse_args()
    try:
        build(args.out)
    except T.Problem as e:
        raise SystemExit(str(e))

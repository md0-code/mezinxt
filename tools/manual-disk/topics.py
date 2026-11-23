"""The manual application's topics: reads src/manual-disk/app/*.txt and packs each topic for the disk (build.py).

A file holds topics one after another:

    @doc ID | SECTION | Title
    text, at most 78 columns a line

ID is lower-case letters, digits and hyphens. SECTION is one of SECTIONS. In the text:

    {b}...{/}  emphasis        {h}...{/}  heading         {c}...{/}  a term or a value
    {g}...{/}  green           {r}...{/}  red             {y}...{/}  yellow          {d}...{/}  dim
    {a:XY}...{/}               any attribute: X background, Y foreground, hexadecimal CGA colours
    [[TARGET|text]]            a link; TARGET is a topic's ID, pag:N (a page of the printed manual),
                               pic:NAME (a picture) or test:NAME (a test)
    {(} and {)}                the braces themselves

Marks do not nest and close on their line. Characters are ASCII and the box and block characters of
the Junior's character ROM (code page 437), written in UTF-8. MezinXT's own.
"""
import re
from pathlib import Path

APP = Path(__file__).resolve().parents[1] / "app"
SECTIONS = ["prezentare", "instalare", "hardware", "software", "anexe", "teste", "carte"]
TESTS = ["tastatura", "video", "sistem", "disc"]
PICTURES = ["intro"]
WIDTH = 78
NORMAL = 0x17
STYLES = {"b": 0x1F, "h": 0x1E, "c": 0x1B, "g": 0x1A, "r": 0x1C, "y": 0x1E, "d": 0x13}
LINK = 0x1A
TOKEN = re.compile(r"\{(b|h|c|g|r|y|d|/|\(|\)|a:[0-9A-Fa-f]{2})\}|\[\[([^|\]]+)\|([^\]]+)\]\]")
# kinds of link, as the reader knows them
DOC, PICTURE, TEST = 0, 1, 2


class Problem(Exception):
    pass


def parse(folder: Path = APP):
    """The topics in file order: dicts of id, section, title, lines (source text) and where they stand."""
    topics = []
    for path in sorted(folder.glob("*.txt")):
        current = None
        for n, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
            line = line.rstrip()
            if line.startswith("@doc"):
                parts = [p.strip() for p in line[4:].split("|")]
                if len(parts) != 3 or not re.fullmatch(r"[a-z0-9-]+", parts[0]) or parts[1] not in SECTIONS or not parts[2]:
                    raise Problem(f"{path.name}:{n}: @doc ID | SECTION | Title, SECTION one of {', '.join(SECTIONS)}")
                current = dict(id=parts[0], section=parts[1], title=parts[2], lines=[], where=f"{path.name}:{n}")
                topics.append(current)
            elif current is None:
                if line.strip() and not line.startswith("#"):
                    raise Problem(f"{path.name}:{n}: text before the first @doc")
            else:
                current["lines"].append((line, f"{path.name}:{n}"))
        for topic in topics:
            while topic["lines"] and not topic["lines"][-1][0]:
                topic["lines"].pop()
    seen = {}
    for topic in topics:
        if topic["id"] in seen:
            raise Problem(f"{topic['where']}: topic {topic['id']} is also at {seen[topic['id']]}")
        seen[topic["id"]] = topic["where"]
    return topics


def tokens(line: str, where: str):
    """(kind, value) pieces of a line: text, attr, link (target, text)."""
    pos = 0
    for m in TOKEN.finditer(line):
        if m.start() > pos:
            yield "text", line[pos:m.start()]
        pos = m.end()
        if m.group(2):
            yield "link", (m.group(2).strip(), m.group(3))
        elif m.group(1) == "(":
            yield "text", "{"
        elif m.group(1) == ")":
            yield "text", "}"
        elif m.group(1) == "/":
            yield "attr", NORMAL
        elif m.group(1).startswith("a:"):
            yield "attr", int(m.group(1)[2:], 16)
        else:
            yield "attr", STYLES[m.group(1)]
    if pos < len(line):
        yield "text", line[pos:]
    rest = TOKEN.sub("", line)
    if "{" in rest or "}" in rest or "[[" in rest or "]]" in rest:
        raise Problem(f"{where}: a brace or bracket that is not a mark: {line!r}")


def cp437(text: str, where: str) -> bytes:
    try:
        data = text.encode("cp437")
    except UnicodeEncodeError as e:
        raise Problem(f"{where}: {text[e.start]!r} is not in the Junior's character set")
    if any(b < 32 for b in data):
        raise Problem(f"{where}: a control character")
    return data


def runs(data: bytes) -> bytes:
    """Spaces and repeated characters shortened: 4 n is n spaces, 5 n c is c n times."""
    out = bytearray()
    i = 0
    while i < len(data):
        j = i
        while j < len(data) and data[j] == data[i] and j - i < 255:
            j += 1
        n = j - i
        if data[i] == 32 and n >= 2:
            out += bytes([4, n])
        elif n >= 4:
            out += bytes([5, n, data[i]])
        else:
            out += data[i:j]
        i = j
    return bytes(out)


def pack_line(line: str, where: str, resolve) -> bytes:
    """One line as the reader takes it: 0 ends it, 1 a sets the attribute, 2 kind low high line starts a link, 3 ends it."""
    out = bytearray()
    width = 0
    for kind, value in tokens(line, where):
        if kind == "text":
            data = cp437(value, where)
            width += len(data)
            out += runs(data)
        elif kind == "attr":
            out += bytes([1, value])
        else:
            target, text = value
            link_kind, number, at = resolve(target, where)
            data = cp437(text, where)
            width += len(data)
            out += bytes([2, link_kind, number & 255, number >> 8, at]) + data + b"\x03"
    if width > WIDTH:
        raise Problem(f"{where}: {width} columns, at most {WIDTH}")
    while len(out) >= 2 and out[-2] == 4:           # spaces at the end of a line
        del out[-2:]
    return bytes(out) + b"\0"


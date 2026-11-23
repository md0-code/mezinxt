"""The CPU conformance material: SingleStepTests' hardware-captured 8088 and
8086 suites, fetched into work/cpu/ and converted for the C runner
(tests/cpu_suite.c).

    python tests/cpu_suite.py fetch [8088|8086]   download at the pinned commit,
                                                   check every file's SHA-256
                                                   against the manifest written
                                                   on first fetch, convert each
                                                   file to work/cpu/<cpu>/*.bin
    python tests/cpu_suite.py run [args]          run the runner over both suites
                                                   (args go to the runner: --sample N,
                                                   --cycles, --only NAME, --show N)
    python tests/cpu_suite.py status              show what is fetched

The suites are third-party test data (MIT, github.com/SingleStepTests/8088
and /8086); they are never committed. They live in the main checkout's
work/cpu/ (a worktree under work/wt/ finds them there), with MANIFEST.txt
recording the commit and the hash of every downloaded file.

Converted format (little-endian), one file per JSON file:
    'MXS2', u8 cpu (0 8088, 1 8086), u8 0, u16 flags mask (metadata.json's
    undefined flags cleared), u32 count, then per test:
    u32 idx, u16 name length, name bytes, u8 n, the instruction's n bytes,
    initial state, final state (the final one complete: unchanged registers
    filled in from the initial state), each:
        14 x u16: ax bx cx dx cs ss ds es sp bp si di ip flags
        u8 queue length, queue bytes
        u32 n, n x (u32 address, u8 byte)
    u32 m, m x 9-byte cycles:
        u32 address latch (20 bits) | t-state << 20 (Ti T1 T2 T3 T4 Tw = 0..5)
            | bus status << 23 (INTA IOR IOW HALT CODE MEMR MEMW PASV = 0..7)
            | segment << 26 (ES SS CS DS -- = 0..4) | ALE << 29 | INTR << 30 | NMI << 31
        u16 data bus
        u8 lines: MRDC 1, AMWC 2, MWTC 4, IORC 8, AIOWC 16, IOWC 32, BHE (pin level) 64
        u8 queue operation (- F S E = 0..3), u8 queue byte
"""
from __future__ import annotations

import gzip
import hashlib
import json
import os
import struct
import subprocess
import sys
import urllib.request
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main_checkout() -> Path:
    """The main checkout's root (a worktree lives under <main>/work/wt/<name>)."""
    if ROOT.parent.name == 'wt' and ROOT.parent.parent.name == 'work':
        return ROOT.parent.parent.parent
    return ROOT


WORK = Path(os.environ['MEZINXT_CPU_DIR']) if os.environ.get('MEZINXT_CPU_DIR') else main_checkout() / 'work' / 'cpu'
RUNNER = ROOT / 'build' / 'x64' / ('cpu_suite.exe' if os.name == 'nt' else 'cpu_suite')
SUITES = {
    '8088': ('SingleStepTests/8088', 'aea84484abc79d09639d855b7b0ab32bc9e4dbeb', 'v2', 0),
    '8086': ('SingleStepTests/8086', 'e71c68d215a6bb8c356bd4cb3842de3bef345ca9', 'v1', 1),
}
# Both suites have the same files: every opcode but the prefixes, 0F (POP CS),
# 9B (WAIT), F4 (HLT); the groups split by the modrm reg field.
GROUPS = {'80', '81', '82', '83', 'D0', 'D1', 'D2', 'D3', 'F6', 'F7'}
SKIP = {'0F', '26', '2E', '36', '3E', '9B', 'F0', 'F1', 'F2', 'F3', 'F4'}


def file_names() -> list[str]:
    names = []
    for op in range(256):
        h = f'{op:02X}'
        if h in SKIP:
            continue
        if h in GROUPS:
            names += [f'{h}.{r}' for r in range(8)]
        elif h == 'FE':
            names += ['FE.0', 'FE.1']
        elif h == 'FF':
            names += [f'FF.{r}' for r in range(8)]
        else:
            names.append(h)
    return names


FILES = file_names()
REGS = ['ax', 'bx', 'cx', 'dx', 'cs', 'ss', 'ds', 'es', 'sp', 'bp', 'si', 'di', 'ip', 'flags']
TSTATES = {'Ti': 0, 'T1': 1, 'T2': 2, 'T3': 3, 'T4': 4, 'Tw': 5}
BUS = {'INTA': 0, 'IOR': 1, 'IOW': 2, 'HALT': 3, 'CODE': 4, 'MEMR': 5, 'MEMW': 6, 'PASV': 7}
SEGS = {'ES': 0, 'SS': 1, 'CS': 2, 'DS': 3, '--': 4}
QOPS = {'-': 0, 'F': 1, 'S': 2, 'E': 3}
MAGIC = b'MXS2'


def folder(cpu: str) -> Path:
    return WORK / cpu


def flags_mask(meta: dict, name: str) -> int:
    op, _, reg = name.partition('.')
    entry = meta['opcodes'].get(op, {})
    if reg and 'reg' in entry:
        entry = entry['reg'].get(reg, entry)
    return int(entry.get('flags-mask', 0xFFFF))


def pack_state(s: dict, base: dict | None) -> bytes:
    regs = dict(base['regs']) if base else {}
    regs.update(s['regs'])
    out = struct.pack('<14H', *[regs[r] & 0xFFFF for r in REGS])
    q = s.get('queue', [])
    out += struct.pack('<B', len(q)) + bytes(q)
    out += struct.pack('<I', len(s['ram'])) + b''.join(struct.pack('<IB', a, v) for a, v in s['ram'])
    return out


def pack_cycle(c: list) -> bytes:
    pins, bus, seg, mem, io, bhe, data, status, t, qop, qbyte = c
    word = (bus & 0xFFFFF) | TSTATES[t] << 20 | BUS[status] << 23 | SEGS[seg] << 26 | (pins & 1) << 29 \
        | (pins >> 1 & 1) << 30 | (pins >> 2 & 1) << 31
    lines = (mem[0] == 'R') | (mem[1] == 'A') << 1 | (mem[2] == 'W') << 2 | (io[0] == 'R') << 3 \
        | (io[1] == 'A') << 4 | (io[2] == 'W') << 5 | (bhe & 1) << 6
    return struct.pack('<IHBBB', word, data & 0xFFFF, lines, QOPS[qop], qbyte & 0xFF)


def convert(job: tuple) -> str:
    src, dst, cpu, mask = job
    tests = json.loads(gzip.decompress(Path(src).read_bytes()))
    parts = [MAGIC, struct.pack('<BBHI', cpu, 0, mask, len(tests))]
    for t in tests:
        name = t['name'].encode('utf-8')
        parts += [struct.pack('<IH', t.get('idx', 0), len(name)), name, struct.pack('<B', len(t['bytes'])), bytes(t['bytes']),
                  pack_state(t['initial'], None), pack_state(t['final'], t['initial']),
                  struct.pack('<I', len(t['cycles']))]
        parts += [pack_cycle(c) for c in t['cycles']]
    tmp = Path(dst).with_suffix('.tmp')
    tmp.write_bytes(b''.join(parts))
    tmp.replace(dst)
    return Path(dst).name


def manifest(cpu: str) -> dict[str, str]:
    path = folder(cpu) / 'MANIFEST.txt'
    out = {}
    if path.exists():
        for line in path.read_text(encoding='utf-8').splitlines():
            if line.startswith('#') or not line.strip():
                continue
            digest, name = line.split(None, 1)
            out[name] = digest
    return out


def download(url: str, path: Path):
    if path.exists():
        return
    with urllib.request.urlopen(url, timeout=600) as r:
        data = r.read()
    part = path.with_suffix('.part')
    part.write_bytes(data)
    part.replace(path)


def current(path: Path) -> bool:
    if not path.exists():
        return False
    with path.open('rb') as f:
        return f.read(4) == MAGIC


def fetch_one(cpu: str) -> int:
    repo, commit, sub, cpu_id = SUITES[cpu]
    d = folder(cpu)
    d.mkdir(parents=True, exist_ok=True)
    base = f'https://raw.githubusercontent.com/{repo}/{commit}/{sub}/'
    names = [f'{n}.json.gz' for n in FILES] + ['metadata.json']
    with ThreadPoolExecutor(8) as pool:
        list(pool.map(lambda n: download(base + n, d / n), names))
    known = manifest(cpu)
    hashes = {n: hashlib.sha256((d / n).read_bytes()).hexdigest() for n in names}
    bad = [n for n in names if n in known and known[n] != hashes[n]]
    if bad:
        print(f'{cpu}: hash differs from the manifest: {", ".join(bad)}', file=sys.stderr)
        return 1
    if not known:
        lines = [f'# {repo} at {commit}, folder {sub}', '# sha256 of each downloaded file']
        lines += [f'{hashes[n]}  {n}' for n in sorted(hashes)]
        (d / 'MANIFEST.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    meta = json.loads((d / 'metadata.json').read_text(encoding='utf-8'))
    jobs = []
    for n in FILES:
        gz, out = d / f'{n}.json.gz', d / f'{n}.bin'
        if not current(out) or out.stat().st_mtime < gz.stat().st_mtime:
            jobs.append((str(gz), str(out), cpu_id, flags_mask(meta, n)))
    with ProcessPoolExecutor(max(1, (os.cpu_count() or 2) - 1)) as pool:
        for i, name in enumerate(pool.map(convert, jobs), 1):
            if i % 25 == 0 or i == len(jobs):
                print(f'{cpu}: converted {i}/{len(jobs)}', flush=True)
    size = sum((d / f'{n}.json.gz').stat().st_size for n in FILES)
    print(f'{cpu}: {len(FILES)} files ({size / 1e6:.0f} MB) at {commit[:12]} in {d}')
    return 0


def fetched(cpu: str) -> bool:
    return all(current(folder(cpu) / f'{n}.bin') for n in FILES)


def run(extra: list[str]) -> int:
    cpus = [c for c in SUITES if fetched(c)]
    if not cpus:
        print('fetch first: python tests/cpu_suite.py fetch', file=sys.stderr)
        return 2
    code = 0
    for c in cpus:
        code |= subprocess.run([str(RUNNER), str(folder(c))] + extra).returncode
    return code


def main(argv) -> int:
    if argv[:1] == ['fetch']:
        cpus = argv[1:] or list(SUITES)
        return max(fetch_one(c) for c in cpus)
    if argv[:1] == ['run']:
        return run(argv[1:])
    if argv[:1] == ['status']:
        for c in SUITES:
            n = sum(current(folder(c) / f'{x}.bin') for x in FILES)
            print(f'{c}: {n}/{len(FILES)} files converted in {folder(c)}')
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

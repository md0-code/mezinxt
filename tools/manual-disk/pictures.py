"""Make the manual disk's pictures: 160x100 in the CGA's sixteen colours, dithered.

    python tools/manual-disk/pictures.py            writes src/manual-disk/pictures/*.pic and previews to work/

A picture is 8000 bytes, one for each pair of points from the top left: the left point's colour in the high
four bits, the right one's in the low four (the attribute byte of character DEh in the 80x100 text mode).
MezinXT's own; needs Pillow and numpy. The sources are in assets/manual-disk/.

The three monitors (video.monitor) show the same sixteen colours quite differently, so the machine's photograph is
dithered for all of them at once (quantise): each point takes the colour that is closest to the photograph on the
colour monitors (RGBI and RGB without intensity steps) and, at the same time, gives the grey that the composite
monitor makes of it (video_out.c) the photograph's brightness. The previews show the picture as each monitor does.
"""
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
PICTURES = ROOT / "src" / "manual-disk" / "pictures"
ASSETS = ROOT / "assets" / "manual-disk"
CGA = [(0, 0, 0), (0, 0, 170), (0, 170, 0), (0, 170, 170), (170, 0, 0), (170, 0, 170), (170, 85, 0), (170, 170, 170),
       (85, 85, 85), (85, 85, 255), (85, 255, 85), (85, 255, 255), (255, 85, 85), (255, 85, 255), (255, 255, 85), (255, 255, 255)]
W, H = 160, 100
POINT = 5 / 6                    # a point's width over its height on a 4:3 screen

# What each monitor shows for the colours 0 to 15 (src/core/machine.c, video_out.c).
RGBI = np.array(CGA, float)                                                                               # the 9-pin monitor
RGB = np.array([[(0, 111, 255)[(i >> s & 1) + (i >> 3)] for s in (2, 1, 0)] for i in range(16)], float)   # the RCA jacks
GREY = np.array([0, 7, 153, 160, 71, 79, 225, 232, 23, 30, 176, 184, 95, 102, 248, 255], float)           # the composite jack
LUMA = np.array([0.299, 0.587, 0.114])


def to_cga(im: Image.Image, dither=True) -> Image.Image:
    pal = Image.new("P", (1, 1))
    pal.putpalette([v for c in CGA for v in c] + [0] * (768 - 48))
    return im.convert("RGB").quantize(palette=pal, dither=Image.Dither.FLOYDSTEINBERG if dither else Image.Dither.NONE)


def composite_taps(bandwidth=8e6, dot_hz=14.31818e6, radius=8):
    """The composite monitor's video bandwidth as video_out.c has it: the weights of the dots around one, Gaussian."""
    s = math.sqrt(math.log(2)) / (2 * math.pi * bandwidth) * dot_hz

    def ramp(x):
        return x * 0.5 * math.erfc(-x / s / math.sqrt(2)) + s * math.exp(-0.5 * (x / s) ** 2) / math.sqrt(2 * math.pi)
    side = []
    for m in range(1, radius + 1):
        w = ramp(m + 1) - 2 * ramp(m) + ramp(m - 1)
        if w * 65536 < 0.5:
            break
        side.append(w)
    return side[::-1] + [1 - 2 * sum(side)] + side


def monitor_view(im: Image.Image, monitor: str) -> Image.Image:
    """The 160x100 picture (colour indices) as `monitor` shows it in the 80x100 text mode, 640x480: each point four dots
    wide and two scan lines high; the composite monitor blurs along each line."""
    idx = np.asarray(im)
    if monitor == "composite":
        dots = np.repeat(GREY[idx], 4, axis=1)
        taps = composite_taps()
        r = len(taps) // 2
        pad = np.pad(dots, ((0, 0), (r, r)), mode="edge")
        dots = sum(w * pad[:, i:i + dots.shape[1]] for i, w in enumerate(taps))
        rgb = np.repeat(np.rint(dots)[..., None], 3, axis=2)
    else:
        rgb = np.repeat((RGB if monitor == "rgb" else RGBI)[idx], 4, axis=1)
    return Image.fromarray(np.repeat(rgb, 2, axis=0).astype(np.uint8)).resize((640, 480), Image.NEAREST)


def save(im: Image.Image, name: str):
    px = im.load()
    data = bytes(px[x, y] << 4 | px[x + 1, y] for y in range(H) for x in range(0, W, 2))
    PICTURES.mkdir(exist_ok=True)
    (PICTURES / f"{name}.pic").write_bytes(data)
    folder = ROOT / "work" / "manual-disk"
    folder.mkdir(parents=True, exist_ok=True)
    for monitor in ("rgbi", "rgb", "composite"):
        monitor_view(im, monitor).save(folder / f"{name}-{monitor}.png")


def write(im: Image.Image, text: str, x: int, y: int, colour, tall=1):
    """Text in the Junior's own characters (roms/chargen.rom), one point a dot."""
    rom = (ROOT / "roms" / "chargen.rom").read_bytes()
    for i, ch in enumerate(text.encode("cp437")):
        for row in range(8):
            for bit in range(8):
                if rom[ch * 8 + row] & (0x80 >> bit):
                    for t in range(tall):
                        im.putpixel((x + i * 8 + bit, y + row * tall + t), colour)


# the intro's last line: capitals three points wide, five high
TINY = {
    "A": [".#.", "#.#", "###", "#.#", "#.#"], "P": ["##.", "#.#", "##.", "#..", "#.."], "S": [".##", "#..", ".#.", "..#", "##."],
    "T": ["###", ".#.", ".#.", ".#.", ".#."], "I": ["###", ".#.", ".#.", ".#.", "###"], "O": [".#.", "#.#", "#.#", "#.#", ".#."],
    "R": ["##.", "#.#", "##.", "#.#", "#.#"], "C": [".##", "#..", "#..", "#..", ".##"], "E": ["###", "#..", "##.", "#..", "###"],
    " ": ["..."] * 5,
}

# capitals four points wide, six high, for the title's lines beside the mark
CAPS = {
    "M": ["#..#", "####", "####", "#..#", "#..#", "#..#"], "N": ["#..#", "##.#", "##.#", "#.##", "#.##", "#..#"],
    "I": ["###.", ".#..", ".#..", ".#..", ".#..", "###."], "C": [".###", "#...", "#...", "#...", "#...", ".###"],
    "R": ["###.", "#..#", "#..#", "###.", "#.#.", "#..#"], "O": [".##.", "#..#", "#..#", "#..#", "#..#", ".##."],
    "A": [".##.", "#..#", "#..#", "####", "#..#", "#..#"], "L": ["#...", "#...", "#...", "#...", "#...", "####"],
    "U": ["#..#", "#..#", "#..#", "#..#", "#..#", ".##."], "T": ["###.", ".#..", ".#..", ".#..", ".#..", ".#.."],
    "E": ["####", "#...", "###.", "#...", "#...", "####"], "H": ["#..#", "#..#", "####", "#..#", "#..#", "#..#"],
    " ": ["...."] * 6,
}


def write_small(im: Image.Image, text: str, x: int, y: int, colour, face, step):
    for i, ch in enumerate(text):
        for row, bits in enumerate(face[ch]):
            for col, bit in enumerate(bits):
                if bit == "#":
                    im.putpixel((x + i * step + col, y + row), colour)


# The machine's outline in junior_xt.jpg (pixels): the monitor, the system unit and the keyboard, traced by GrabCut and
# corrected by hand. Inside it is the machine, outside the studio backdrop.
OUTLINE = [
    (1168, 182), (1038, 184), (1024, 194), (548, 206), (528, 212), (532, 720), (602, 738), (606, 760), (380, 792), (338, 804),
    (334, 845), (332, 898), (340, 1036), (312, 1076), (294, 1084), (154, 1222), (150, 1250), (162, 1290), (700, 1308),
    (1000, 1318), (1458, 1338), (1450, 1144), (1476, 1100), (1480, 1062), (1476, 1022), (1432, 1004), (1432, 966),
    (1440, 956), (1444, 774), (1246, 764), (1240, 748), (1240, 718), (1270, 664), (1276, 520), (1276, 250), (1268, 232)]
FRAME = (140, 172, 1490, 1346)   # the machine and a margin
PANEL = (28, 31, 104, 61)        # the backdrop in the intro, in points: left, top, width, height. Magenta, the photograph's
                                 # violet: on the composite monitor a mid grey between the black of the monitor's case and
                                 # the cream of the unit and the keyboard
SCREEN = (0.62, 1.27, 0.46)      # the monitor's green phosphor, relative to the brightness of its picture


def blur(a: np.ndarray, sigma: float) -> np.ndarray:
    """Gaussian blur of a 2-D array, `sigma` high in points (a point is narrower than high), the edges extended."""
    for axis, s in ((0, sigma), (1, sigma / POINT)):
        r = max(1, round(3 * s))
        k = np.exp(-0.5 * (np.arange(-r, r + 1) / s) ** 2)
        pad = np.pad(a, [(r, r) if i == axis else (0, 0) for i in range(2)], mode="edge")
        a = sum(w * np.take(pad, range(i, i + a.shape[axis]), axis=axis) for i, w in enumerate(k / k.sum()))
    return a


def resize(a: np.ndarray, width: int, height: int) -> np.ndarray:
    """A float array averaged down to width x height, every channel."""
    if a.ndim == 3:
        return np.stack([resize(a[..., c], width, height) for c in range(a.shape[2])], 2)
    return np.asarray(Image.fromarray(a.astype(np.float32), "F").resize((width, height), Image.BOX))


def machine(rows: int):
    """The machine cut out of the photograph, `rows` points high: its colours (0-255) and where it is. A point is the
    machine's or the backdrop's, with no blend between them: the edges are not dithered with the backdrop."""
    photo = Image.open(ASSETS / "junior_xt.jpg").convert("RGB")
    mask = Image.new("L", photo.size, 0)
    ImageDraw.Draw(mask).polygon(OUTLINE, fill=255)
    mask = np.asarray(mask.filter(ImageFilter.GaussianBlur(2)), float)[FRAME[1]:FRAME[3], FRAME[0]:FRAME[2], None] / 255
    pixels = np.asarray(photo.crop(FRAME), float)
    width = round(rows * pixels.shape[1] / pixels.shape[0] / POINT)
    cover = resize(mask[..., 0], width, rows)
    return resize(pixels * mask, width, rows) / np.maximum(cover[..., None], 1e-3), cover >= 0.5


def prepare(canvas: np.ndarray, inside: np.ndarray) -> np.ndarray:
    """What the picture should show on `canvas` (the backdrop with the machine on it): the machine's edges raised (an
    unsharp mask of its brightness at two sizes: the details, and the parts of the machine against one another), the
    contrast a little more, the screen a clean green. Only the machine's points change."""
    y = canvas @ LUMA
    out = canvas + sum(amount * (y - blur(y, sigma)) for sigma, amount in ((1.0, 1.5), (4.0, 0.7)))[..., None]
    out = np.clip((out - 128) * 1.2 + 128, 0, 255)
    y = (out @ LUMA)[..., None]
    green = np.clip((out[..., 1] - np.maximum(out[..., 0], out[..., 2]) - 5) / 15, 0, 1)[..., None]
    out = out * (1 - green) + np.clip(y * SCREEN, 0, 255) * green
    return np.where(inside[..., None], out, canvas)


def spread_error(err: np.ndarray, y: int, x: int, step: int, e):
    """Floyd-Steinberg: seven sixteenths ahead, three, five and one below (err has a point of padding each side)."""
    err[y, x + 1 + step] += e * 7 / 16
    err[y + 1, x + 1 - step] += e * 3 / 16
    err[y + 1, x + 1] += e * 5 / 16
    err[y + 1, x + 1 + step] += e / 16


def quantise(target: np.ndarray, fixed: np.ndarray, grey_weight=0.65, reach=90, keep=2, luma=0.9, chroma=0.35, spread=0.9):
    """Colour indices for `target` (rows x points x RGB, 0-255), error-diffused (Floyd-Steinberg, serpentine) in the
    three spaces the monitors show: RGBI, RGB and the composite grey. A colour's cost is (1 - grey_weight) times the
    mean of its squared distances to the target on the two colour monitors (per channel) plus grey_weight times the
    squared difference of its composite grey from the target's brightness. Only the colours within `reach` of the
    target (at least the `keep` nearest) are tried: a point is not given a colour of another hue for the brightness it
    would add. Of the colour error the brightness part is carried on by `luma`, the hue part by `chroma` (less: hues
    that wander are confetti), of the grey error `spread`. `fixed` holds the colours of the points that are not
    dithered, -1 for the others."""
    rows, cols, _ = target.shape
    palettes = (RGBI, RGB)
    brightness = target @ LUMA
    err = [np.zeros((rows + 1, cols + 2, 3)) for _ in palettes]
    err_grey = np.zeros((rows + 1, cols + 2))
    out = np.zeros((rows, cols), np.uint8)
    for y in range(rows):
        step = 1 if y % 2 == 0 else -1
        for x in (range(cols) if step == 1 else range(cols - 1, -1, -1)):
            if fixed[y, x] >= 0:
                out[y, x] = fixed[y, x]
                continue
            want = [np.clip(target[y, x] + err[k][y, x + 1], 0, 255) for k in range(2)]
            want_grey = np.clip(brightness[y, x] + err_grey[y, x + 1], 0, 255)
            distance = np.sqrt(sum(((pal - target[y, x]) ** 2).sum(1) for pal in palettes) / 2)
            cost = (1 - grey_weight) * sum(((pal - w) ** 2).sum(1) for pal, w in zip(palettes, want)) / 6 \
                + grey_weight * (GREY - want_grey) ** 2
            cost[distance > max(reach, np.sort(distance)[keep - 1])] = np.inf
            c = int(np.argmin(cost))
            out[y, x] = c
            for k, pal in enumerate(palettes):
                e = want[k] - pal[c]
                lum = (e @ LUMA) * np.ones(3)
                spread_error(err[k], y, x, step, lum * luma + (e - lum) * chroma)
            spread_error(err_grey, y, x, step, (want_grey - GREY[c]) * spread)
    return out


def photograph() -> Image.Image:
    """The intro's picture below the band, W points wide, in the CGA's colours: the machine on its backdrop, the blue
    ground around."""
    left, top, width, height = PANEL
    rows = height - 2
    colours, inside = machine(rows)
    x0 = left + (width - colours.shape[1]) // 2
    canvas = np.tile(np.array(CGA[1], float), (height, W, 1))
    canvas[:, left:left + width] = CGA[5]
    where = np.zeros((height, W), bool)
    where[1:1 + rows, x0:x0 + colours.shape[1]] = inside
    canvas[1:1 + rows, x0:x0 + colours.shape[1]][inside] = colours[inside]
    fixed = np.full((height, W), 1, np.int16)                       # the ground, blue
    fixed[:, left:left + width] = 5                                 # the backdrop, magenta
    fixed[where] = -1
    return Image.fromarray(RGBI[quantise(prepare(canvas, where), fixed)].astype(np.uint8))


def main():
    """The intro: the maker's mark and the title on a white band, the machine below, "press any key"."""
    white, blue = CGA[15], CGA[1]
    logo = Image.open(ASSETS / "feper.png").convert("RGBA")
    lw = 64
    lh = round(lw * logo.height / logo.width * POINT)
    band = Image.new("RGB", (W, 30), white)
    band.paste(logo.resize((lw, lh), Image.LANCZOS), (5, (30 - lh) // 2), logo.resize((lw, lh), Image.LANCZOS))
    band = to_cga(band, dither=False).convert("RGB")
    # the manual's title, in the room right of the mark
    left, room = 5 + lw + 2, W - (5 + lw + 2)
    write_small(band, "MICROCALCULATORUL", left + (room - 17 * 5 + 1) // 2, 3, blue, CAPS, 5)
    write(band, "JUNIOR-XT", left + (room - 72) // 2, 11, blue)
    write_small(band, "CARTE TEHNICA", left + (room - 13 * 5 + 1) // 2, 21, CGA[4], CAPS, 5)
    out = Image.new("RGB", (W, H), blue)
    out.paste(band, (0, 0))
    out.paste(photograph(), (0, PANEL[1]))
    text = "APASATI ORICE TASTA"
    write_small(out, text, (W - len(text) * 4 + 1) // 2, 93, CGA[14], TINY, 4)
    save(to_cga(out, dither=False), "intro")


if __name__ == "__main__":
    main()

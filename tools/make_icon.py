"""The window's icon where the program file carries none (outside Windows), made from assets/mezinxt.ico.

    python tools/make_icon.py

Writes src/app/icon_data.h: the icon's 64 x 64 picture as ARGB words. Run again when the icon changes; the output is
committed. Needs Pillow.
"""
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SIZE = 64


def main() -> None:
    icon = Image.open(ROOT / "assets" / "mezinxt.ico")
    icon.size = (SIZE, SIZE)
    data = icon.convert("RGBA").tobytes()
    argb = [data[i + 3] << 24 | data[i] << 16 | data[i + 1] << 8 | data[i + 2] for i in range(0, len(data), 4)]
    words = "\n".join("    " + ",".join(f"0x{v:08X}u" for v in argb[i:i + 8]) + "," for i in range(0, len(argb), 8))
    text = f"""/* Made by tools/make_icon.py from assets/mezinxt.ico; do not edit. */
#ifndef MX_ICON_DATA_H
#define MX_ICON_DATA_H

#include <stdint.h>

/* The window's icon: ARGB, a transparent ground. */
#define MX_ICON_SIZE {SIZE}
static const uint32_t mx_icon_pixels[MX_ICON_SIZE * MX_ICON_SIZE] = {{
{words}
}};

#endif /* MX_ICON_DATA_H */
"""
    (ROOT / "src" / "app" / "icon_data.h").write_text(text, encoding="ascii", newline="\n")
    print("src/app/icon_data.h")


if __name__ == "__main__":
    main()

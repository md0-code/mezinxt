# Third-party components

Linked into the programs:

| Component | Use | License |
|---|---|---|
| [SDL 2.32.10](https://www.libsdl.org/) | Window, OpenGL context, input and sound output; fetched at build time and linked statically | zlib |
| [libyaml 0.2.5](https://github.com/yaml/libyaml/tree/0.2.5) | YAML syntax of `mezinxt.yaml` (`src/third_party/libyaml`, tag commit `2c891fc7a770e8ba2fec34fc6b545c672beb37e6`); linked statically | MIT (`src/third_party/libyaml/LICENSE`) |
| Microsoft Visual C++ runtime | Linked statically into the Windows programs | Microsoft's redistribution terms for Visual Studio |

The Linux programs link only the system's C library and math library; SDL loads the system's X11,
Wayland, ALSA, PulseAudio and OpenGL libraries when it runs, and none of them is shipped.

Shipped with the programs:

| Data | Use | License |
|---|---|---|
| The JUNIOR-XT's BIOS and character ROMs (`roms/`) | The Junior's firmware, FEPER's, preserved from the z80-romania.ro archive; they are not part of MezinXT's source, `tools/fetch_roms.py` fetches them from the archives listed with their SHA-256 in the source repository (`tools/sources.lock.json`) | FEPER's: the original ROM dumps are credited to FEPER and preserved from the z80-romania.ro archive |
| The Junior's technical manual (`disks/manual.img`) | The manual disk's text, transcribed from FEPER's *Carte tehnica* (1989), with FEPER's mark and a photograph of a JUNIOR-XT; the reader and the tests are MezinXT's own ([notes](NOTES.md#the-manual-disk)) | FEPER's content; the program code is MezinXT's |

## Notices

libyaml 0.2.5:

```
Copyright (c) 2017-2020 Ingy döt Net
Copyright (c) 2006-2016 Kirill Simonov

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

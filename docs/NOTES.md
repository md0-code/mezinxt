# Notes

The manual disk, every setting, and what MezinXT does not do. The keys, the setup screen and the machine's options are in the [README](../README.md).

## The manual disk

`disks/manual.img` is a 360 KB self-booting diskette with an application that presents the Junior's technical manual
(*Microcalculatorul JUNIOR-XT, Carte tehnica*, FEPER, 1989) on the Junior itself, in Romanian: topics with links and
drawings in the 80x25 colour text mode, an intro picture with FEPER's mark and a photograph of the machine in sixteen
colours, tests that run on the machine, and the printed manual page by page. There is no operating system on the
diskette: its boot sector loads the application, which uses only the BIOS. The schematics (annex 4) are not on the disk.
It runs on the 8086 and the 8088 with 256 KiB and one drive. A key passes the intro picture.

### Keys

| Key | Does |
|---|---|
| Up, Down | the link above or below; past the last one in sight the text moves a line |
| Tab | the next link in sight |
| Enter | opens the link: a topic, a page or a test |
| Backspace, Esc | back to where the link was opened (sixteen places are kept) |
| PgUp, PgDn, Home, End | the text by a screen, its top and bottom |
| Left, Right | the topic or page before and after this one in its section |
| F1 to F7 | the sections |
| P | "Pagina": a page number of the printed manual and Enter |

A read error is retried three times, then named on the bottom row; a key tries again.

### Tests

| Test | Shows |
|---|---|
| Tastatura | the 83 keys drawn; a key pressed lights up and stays marked when released, with the code the keyboard sent (the keyboard interrupt is taken for the time of the test); Esc twice leaves |
| Afisaj | one picture after another: the sixteen colours with the manual's names, the character set, the 40-column mode, 320x200 in its palettes, 640x200, a pattern for setting the monitor |
| Sistem | the configuration switches as the board gives them (ports 61h and 62h) and what they say, the memory the BIOS found, the coprocessor, the timer's count; M checks the memory above the program, S sounds the speaker |
| Discheta si port-uri | every track of the diskette read on both sides (A and B choose the drive), the printer's and the serial port's state as the BIOS reports them, the joystick port |

## Configuration

MezinXT reads its settings from `mezinxt.yaml` in the working folder (or the file named by
`--config FILE.yaml`). A folder without one gets a file with every setting at its default and a
comment on each, written on the first start. The setup screen (F12) writes its changes back to the
file and keeps every comment. A setting that is not in the file has its default, so a file may list
only what it changes; a key the program does not know, or a value it cannot use, stops the start with
the line of the file and what was expected.

Paths (disk images, ROM images, output files) may be absolute or relative; a relative one is relative
to the folder of the configuration file, not to the working folder.

The table of each section lists every key with its values and its default. The defaults are those of
the template the first start writes (`src/app/mezinxt.yaml`), which a check of the project compares with
this page.

### machine

The emulated Junior and its boards. A change made on the setup screen resets the machine when the
screen is left.

| Key | Values | Default | Meaning |
|---|---|---|---|
| `machine.bios` | `3.0`, `4.0`, `5.0`, `5.0-alt` | `4.0` | The BIOS version, from `roms/`. 4.0 is the one for this board: it drives the Junior's own serial adapter. 3.0 and 5.0 expect PC/XT 8250 serial ports (`ports.pc_serial`) |
| `machine.cpu` | `8086`, `8088` | `8086` | The processor: the Junior's 8086 (16-bit bus) or the 8088 the board can take instead (8-bit bus) |
| `machine.speed` | `4.77`, `7.16`, `9.54`, `unthrottled` | `4.77` | The processor speed in MHz. 4.77 is the original; 7.16 and 9.54 are not; `unthrottled` runs as fast as the host allows |
| `machine.ram_kb` | `64`, `128`, `192`, `256`, `640` | `640` | Memory in KiB. 64 to 256 are the board's banks; 640 is 256 KiB with the 384 KiB module, which the option ROM sockets (`machine.rom_*`) need |
| `machine.display` | `cga80`, `cga40`, `mda`, `none` | `cga80` | The display coupler: the CGA in 80 or 40 columns, the monochrome coupler (80 columns, green phosphor, its own printer port `ports.lpt_mda`) or none (a black screen) |
| `machine.fpu` | `true`, `false` | `false` | The 8087 math processor is fitted |
| `machine.drives` | `1` to `4` | `2` | The diskette drives fitted (`drives.a` to `drives.d`). DOS names only the first two; the others are reached through the BIOS |
| `machine.winchester` | `true`, `false` | `false` | The Winchester coupler, with the fixed disks `drives.hd0` and `drives.hd1`. Its BIOS takes C8000h, so `machine.rom_c8000` must stay empty |
| `machine.rom_c8000` | a file | empty | An EPROM image (up to 32 KiB) in the 384 KiB module's socket at C8000h. The BIOS runs a ROM that starts with 55h AAh and whose bytes add up to zero (else ERROR 701). Needs `machine.ram_kb: 640` |
| `machine.rom_f0000` | a file | empty | The same for the socket at F0000h |
| `machine.rom_f8000` | a file | empty | The same for the socket at F8000h, where at most 16 KiB fit (32 KiB would cover the BIOS) |

### drives

Disk images are raw sector dumps of 160, 180, 320, 360 or 720 KiB. An empty `image` is an empty drive.
`drives.c` and `drives.d` are there when `machine.drives` is 3 or 4. Changes the guest makes are written
back to the image file; a read-only file is write protected.

Fast access (`fast`) is set for each drive and is on by default. While a drive with it on is at work the
machine runs unthrottled, as fast as the host allows; a drive without it leaves the machine at its own
speed. The older key `machine.fast_disk` is still read: it sets `fast` of every drive that does not have
a `fast` of its own. It is no longer written, and the setup screen removes it from the file when it saves.

| Key | Values | Default | Meaning |
|---|---|---|---|
| `drives.a.image` | a file | `disks/manual.img` | The disk in drive A. The first start writes the manual disk (`disks/manual.img` beside the configuration, as a release's folder has it) when the file is there, else an empty drive; a file without the key has an empty drive |
| `drives.a.write_protect` | `true`, `false` | `false` | The disk in drive A is write protected |
| `drives.a.tracks` | `40`, `80` | `40` | The drive's tracks: an 80-track drive reads 720 KiB disks, a 40-track drive refuses them |
| `drives.a.fast` | `true`, `false` | `true` | Fast access. Not original: the machine runs at full host speed while drive A works (its motor turns), so loading is quick |
| `drives.b.image` | a file | empty | The disk in drive B |
| `drives.b.write_protect` | `true`, `false` | `false` | As `drives.a.write_protect` |
| `drives.b.tracks` | `40`, `80` | `40` | As `drives.a.tracks` |
| `drives.b.fast` | `true`, `false` | `true` | As `drives.a.fast` |
| `drives.c.image` | a file | empty | The disk in drive C (the third diskette drive) |
| `drives.c.write_protect` | `true`, `false` | `false` | As `drives.a.write_protect` |
| `drives.c.tracks` | `40`, `80` | `40` | As `drives.a.tracks` |
| `drives.c.fast` | `true`, `false` | `true` | As `drives.a.fast` |
| `drives.d.image` | a file | empty | The disk in drive D (the fourth diskette drive) |
| `drives.d.write_protect` | `true`, `false` | `false` | As `drives.a.write_protect` |
| `drives.d.tracks` | `40`, `80` | `40` | As `drives.a.tracks` |
| `drives.d.fast` | `true`, `false` | `true` | As `drives.a.fast` |
| `drives.hd0.image` | a file | empty | The first fixed disk of the Winchester coupler (`machine.winchester`): a raw image of 17-sector tracks, 20 to 70 MB; the geometry follows from its size |
| `drives.hd0.fast` | `true`, `false` | `true` | As `drives.a.fast`, for the first fixed disk: the machine runs at full host speed while it works |
| `drives.hd1.image` | a file | empty | The second fixed disk; it needs the first |
| `drives.hd1.fast` | `true`, `false` | `true` | As `drives.hd0.fast` |

### video

| Key | Values | Default | Meaning |
|---|---|---|---|
| `video.look` | `plain`, `crt` | `crt` | The picture as it is, or through the CRT emulation shader, which imitates a cathode-ray tube monitor |
| `video.monitor` | `rgbi`, `rgb`, `mono-green` | `mono-green` | The monitor on the CGA, one for each of its video outputs: `rgbi`, colour with intensity steps (the nine-pin connector: 16 colours, with brown); `rgb`, colour without intensity steps (the RCA jacks, which mix the intensity into every channel: three levels a channel, 15 colours, colour 6 dark yellow, colours 7 and 8 alike); `mono-green`, monochrome on the composite video signal (the RCA jack: 16 grey levels from the card's resistor network, softened along each line by the monitor's limited bandwidth, an 8 MHz estimate) in green phosphor. The monochrome coupler (`machine.display: mda`) always shows green |
| `video.aspect` | `original`, `square` | `original` | The picture's shape: 4:3 as on the monitor, or square pixels |
| `video.scale` | `fit`, `integer` | `fit` | Fit the window, or integer multiples of the picture only |
| `video.filter` | `nearest`, `linear`, `sharp` | `sharp` | How the plain look's picture is scaled to the window. `nearest`: each emulated pixel is repeated, so pixels come out in uneven widths at scales that are not whole multiples. `linear`: smooth, and blurred. `sharp`: crisp pixels of even size, with soft edges only where a pixel straddles two screen pixels. The `crt` look does its own smoothing and ignores the filter: the setup screen dims the row then, and the stored value applies again with the `plain` look (without OpenGL the picture is plain whatever `video.look` says, and the filter acts) |
| `video.fullscreen` | `true`, `false` | `false` | Start in full screen. Alt+Enter switches while running |
| `video.vsync` | `true`, `false` | `true` | Wait for the display's vertical sync |
| `video.snow` | `true`, `false` | `true` | The CGA card's snow in 80-column text, as the real card has it: while a program reads or writes the video store, the picture shows other data in one cell of the scanline for each processor cycle on the store (the card has no free memory cycle for the processor there). A byte written leaves the other half of the card's data bus as it last was. `false` is not original: the picture stays clean (the processor still waits for its memory cycle). The setup screen's Display page switches it at once. The monochrome coupler has no snow |
| `video.drive_lights` | `true`, `false` | `false` | Not original: a light in the picture's bottom row for each drive at work, the diskette drives' letters (A, B, ...) and the fixed disks' C and D in white on red. Messages (pause, a disk inserted, the speed, mouse capture) show whether the lights are on or not. The setup screen's Display page switches it at once |

### sound

| Key | Values | Default | Meaning |
|---|---|---|---|
| `sound.enabled` | `true`, `false` | `true` | `false` mutes the speaker and the drives |
| `sound.volume` | `0` to `100` | `100` | The overall volume in percent, for the speaker and the drives |
| `sound.drives` | `true`, `false` | `true` | The sound of the disk drives, as on the real machine: the diskette motors, the stepper's clicks, the head loads, a fixed disk's spindle and seeks. Made up from what the drives do, not recorded; with fast access (`drives.*.fast`) the machine runs faster than real time while a disk works, and the drives are then heard at the pace of the clock, as many clicks as there were steps |
| `sound.drives_volume` | `0` to `100` | `50` | The drives' loudness in percent, against the speaker's (0 is the same as `sound.drives: false`) |

### ui

| Key | Values | Default | Meaning |
|---|---|---|---|
| `ui.language` | `en`, `ro` | `en` | The language of the setup screen and of the messages in the window: English or Romanian. When the key is absent, and in the file a first start writes, it is Romanian on a system whose user language is Romanian, else English |

### ports

The serial channels, the printer ports and the PC/XT serial ports reach the host through one syntax,
the end of the port:

| End | Meaning |
|---|---|
| `none` | Nothing connected. The serial service of the BIOS times out; a parallel port reads off line |
| `loopback` | The channel's transmitter wired to its receiver (a loopback plug), in the guest's own format and speed |
| `COM3`, `/dev/ttyUSB0` | A serial port of the host |
| `tcp:HOST:PORT` | A TCP connection the emulator makes, again every second while nobody answers or after the other side leaves |
| `tcp-listen:PORT`, `tcp-listen:HOST:PORT` | A TCP server, one client at a time, on 127.0.0.1 unless a HOST such as `0.0.0.0` is given |
| `pipe:PATH` | A local pipe: on Windows `\\.\pipe\NAME`, on Linux the path of a Unix socket. MezinXT serves it and waits for a client, or joins one that exists |
| `file:FILE` | The bytes the guest sends are appended to a file |
| `mouse` | Only on `ports.com1` and `ports.com2`: the host mouse as a Microsoft serial mouse. At most one port is the mouse. The setup screen's Controllers page sets it too (Mouse: none, COM1 or COM2), which fits the PC/XT adapter for it and takes the Junior's own serial and printer adapter out (they share IRQ 4); the Ports page's SIO channels cannot be the mouse, a Microsoft mouse driver needs an 8250 port |

The parallel ports (`ports.lpt`, `ports.lpt_mda`) take only `none` or `file:FILE`. A value with a
backslash or a colon is written in single quotes, as the setup screen does (`'pipe:\\.\pipe\junior'`).

| Key | Values | Default | Meaning |
|---|---|---|---|
| `ports.adapter` | `true`, `false` | `true` | The Junior's serial and parallel adapter board is fitted: the Z80-SIO at 250h with IRQ 4 (BIOS 4.0 finds it at 253h) and the printer port at 378h. It must be `false` with `ports.pc_serial` `one` or `two`, which also use IRQ 4: the setup screen takes it out when the PC serial ports are fitted (on the Ports page or by the Controllers page's Mouse row), with a note |
| `ports.sio_a` | an end | `none` | Channel A of the Z80-SIO |
| `ports.sio_b` | an end | `none` | Channel B of the Z80-SIO |
| `ports.lpt` | `none`, `file:FILE` | `none` | The adapter's printer port. With `none` the printer is off line (INT 17h times out); a file is an on-line printer |
| `ports.pc_serial` | `none`, `one`, `two` | `none` | The PC/XT asynchronous adapter: 8250 serial ports at 3F8h (COM1, IRQ 4) and 2F8h (COM2, IRQ 3), which BIOS 3.0 and 5.0 find and a serial mouse needs. It replaces the Junior's own serial and printer adapter (`ports.adapter`) |
| `ports.com1` | an end, `mouse` | `none` | COM1 of the PC/XT adapter. The mouse is set on the setup screen's Controllers page (Mouse) or here |
| `ports.com2` | an end, `mouse` | `none` | COM2 of the PC/XT adapter |
| `ports.lpt_mda` | `none`, `file:FILE` | `none` | The printer port of the monochrome coupler (3BCh), only with `machine.display: mda` |

### controllers

| Key | Values | Default | Meaning |
|---|---|---|---|
| `controllers.adapter` | `true`, `false` | `false` | The game control adapter (joystick coupler, port 201h) is fitted |
| `controllers.light_pen` | `true`, `false` | `false` | The host mouse is a light pen on the CGA: its position over the picture, the left button the pen's switch |
| `controllers.keys.up` | an SDL key name | `Up` | The keyboard joystick's key for up. The six keys of `controllers.keys` are named as `--hostkey` takes them (`Up`, `A`, `Space`, `Left Ctrl`, `Keypad 8`); each needs a key of its own, and F11, F12, Pause and PrintScreen stay the window's. While a joystick is fed by the keyboard these keys are the stick's and do not reach the guest, until F11 gives them back to the computer (F11 again takes them for the stick; a short message says which). The setup screen's Controllers page sets them (Joystick keys...) |
| `controllers.keys.down` | an SDL key name | `Down` | The key for down |
| `controllers.keys.left` | an SDL key name | `Left` | The key for left |
| `controllers.keys.right` | an SDL key name | `Right` | The key for right |
| `controllers.keys.fire1` | an SDL key name | `Left Ctrl` | The key for button 1 |
| `controllers.keys.fire2` | an SDL key name | `Left Shift` | The key for button 2 |
| `controllers.joystick1.device` | `none`, `keyboard`, `controller N` (1 to 8) | `none` | What feeds joystick 1: nothing, the keyboard (the keys of `controllers.keys`: the arrow keys, Left Ctrl and Left Shift unless changed) or the Nth host game controller |
| `controllers.joystick1.dead_zone` | `0` to `50` | `10` | Percent of the travel that counts as centred |
| `controllers.joystick1.range` | `50` to `100` | `100` | Percent of the travel that gives full deflection |
| `controllers.joystick1.centre_x` | `-50` to `50` | `0` | Where the stick rests horizontally, percent of the travel |
| `controllers.joystick1.centre_y` | `-50` to `50` | `0` | Where the stick rests vertically; the setup screen's Calibrate sets the range and the centre |
| `controllers.joystick2.device` | `none`, `keyboard`, `controller N` (1 to 8) | `none` | What feeds joystick 2 |
| `controllers.joystick2.dead_zone` | `0` to `50` | `10` | As `controllers.joystick1.dead_zone` |
| `controllers.joystick2.range` | `50` to `100` | `100` | As `controllers.joystick1.range` |
| `controllers.joystick2.centre_x` | `-50` to `50` | `0` | As `controllers.joystick1.centre_x` |
| `controllers.joystick2.centre_y` | `-50` to `50` | `0` | As `controllers.joystick1.centre_y` |

## Limitations

What MezinXT does not do, or does less than exactly, and what has not been tried.

### Timing

- The speeds 7.16 MHz and 9.54 MHz are not the Junior's: they make the processor and its bus cycles
  (the DMA cycles too) faster and leave the timers, the video and the diskette and serial chips at their
  own clocks. `unthrottled` keeps the 4.77 MHz clock and only stops pacing the machine to real time.

### Boards and catalogue items not emulated

The Junior's catalogue lists thirteen boards and sixteen peripherals; its manual documents only the
diskette, CGA, serial and parallel, and memory boards. MezinXT emulates the others as their IBM PC/XT
equivalents where there is one and does not emulate:

- **The synchronous serial coupler** (703.300.005): no documentation and no software were found.
- **The magnetic tape coupler and unit** (703.300.007, 703.806).
- **The instrumentation coupler** (703.300.009).
- **Paper tape reader and punch, display and graphics tablet** (703.809 to 703.814): their interfaces
  are unknown.
- **8-inch drives** (UDF-102A, 703.801): the manual gives one side, 77 tracks and MFM, but not the data
  rate, the rotation speed, the sector layout or the controller clock, and the BIOSes have one
  diskette parameter table, for 5.25" media.
- **Plotters and printers as devices**: a plotter on a serial port or a printer on a parallel port gets
  the bytes the program sends and they are written to a file (`file:`); there is no plotter or printer
  model that draws or prints them.
- **Memory parity errors**: the RAM never fails; the parity latch, its enable and the NMI path are wired
  and tested.
- Beyond the catalogue: sound cards, video cards other than the CGA and the monochrome coupler,
  memory above 640 KiB, 3.5" drives and network cards.

### The serial adapter

- The Z80-SIO is emulated in full in its **asynchronous modes**. The synchronous modes (monosync,
  bisync, SDLC) are not: their register writes are accepted and stored, but there is no synchronous
  character engine, CRC, hunt phase or end-of-frame handling. A channel set to a synchronous mode
  runs asynchronously, as the Junior's own RTL does.
- **The PC/XT asynchronous adapter (two 8250s) excludes the Junior's own serial adapter.** Both drive
  IRQ 4, so `ports.pc_serial` of `one` or `two` needs `ports.adapter: false`, and the adapter's
  printer port at 378h goes with it. BIOS 4.0, the version that matches the board, uses the Junior's
  own adapter; BIOS 3.0 and 5.0 expect the 8250 ports.
- The 8250 is the INS8250 of the original PC/XT: no scratch register, no FIFO.

### Display

- **The monochrome coupler (MDA) and the CGA are not fitted together.** The IBM design allows both,
  the switches naming the primary; the shell has one picture, so `machine.display` names one coupler.
- **The Junior's character ROM holds no Romanian diacritics**, only the code page 437 letters with a
  circumflex (â, î), and no capitals of them. The Romanian texts of the setup screen are drawn without
  diacritics (ă and â as a, î as i, ș as s, ț as t); a character ROM with the whole set would show them. The monochrome coupler's 8x14 font is made from the same
  ROM, as no ROM dump of that coupler exists.
- The CRT controller's interlace modes are not modelled, and the CRT look is the shader's imitation of
  a monitor, not a measurement of one.
- **The composite monitor's video bandwidth is an estimate.** `video.monitor: mono-green` softens the picture
  along each line with a Gaussian low-pass filter of 8 MHz (-3 dB), chosen from period monochrome composite
  monitors and the legibility of 80-column text;
  no source documents the response of the monitor the Junior had. The beam spot's size and the phosphor are
  not modelled, and the RGBI and RGB monitors and the monochrome coupler's picture are shown pixel-sharp.
- **The composite output's red resistor is uncertain.** R1 reads 1K on schematic sheet 10 and 1K2 in the card's
  parts list; MezinXT takes the parts list's 1K2, and the grey levels of the colours with red would be up to
  10 of 255 different with 1K. A photograph of the part on a
  real card would settle it.
- **The CGA's wait states and snow are derived from the schematic**, with typical component delays and a few
  readings of unclear labels, not measured on a real card:
  a card cycle takes 7 to 12 clocks here, and what a snow cell shows in a byte write rests on how the
  undriven half of the card's data bus behaves.

### The 8087

- The transcendental instructions (FPTAN, FPATAN, F2XM1, FYL2X, FYL2XP1) are computed to within one unit in
  the last place, not by the chip's own microcode: a result can differ from a real 8087's in the last bit.

### Disks

- **A `FORMAT` that asks the BIOS for the drive parameters stops with an error**: INT 13h function 08h
  is not in the Junior's BIOSes (functions 0 to 5 only). A disk is formatted by a program that uses the
  BIOS's own format function (INT 13h function 05h).
- A blank disk made on the setup screen is unformatted until a program formats it; a fixed disk needs
  `FDISK` and `FORMAT`.
- Images are raw sector dumps recognised by size (160, 180, 320, 360 and 720 KiB). Copy-protected
  formats and image formats other than raw (IMD, TD0, DMK, HFE, FDI) are not read.
- A nine-sector disk (360 and 720 KiB) needs boot software that supplies its own diskette parameter
  table; BIOS 4.0's table is for eight sectors.

### Never tried on real equipment

- **No physical Junior.** The emulator has been checked against the Junior's documentation, its
  firmware (the four BIOS versions), the hardware-captured processor suites, the chips' data sheets and
  the JUNIOR-XT core for MiSTer. It has not been compared with a real JUNIOR-XT, and the reconstructed
  boards (game control adapter, Winchester coupler, monochrome coupler, PC/XT serial adapter, the
  8087's coupling, light pen) have not met real hardware or the software written for it.
- **Real displays.** Screens are checked headless, picture against picture. The Linux programs have been
  run headless (SDL's offscreen video driver), never in a window on a real X11 or Wayland display.
- **Real controllers and ports.** Host game controllers are tested against a virtual SDL controller,
  not a physical one. The loopback, TCP and file ends of the serial and parallel ports are tested;
  pipes and host serial ports (`COMn`, `/dev/tty...`) have not been connected to anything by the
  project's checks.
- **ARM64.** The Windows ARM64 program is compiled but has not been run; the Linux ARM64 program has been
  run only under qemu user-mode emulation. No ARM64 machine has run either.
- **The drives' sound is synthesised**, not recorded from a Junior: the diskette drive's clicks and hum have the character of a period
  5.25" drive, set by ear; the Winchester is modelled on measurements of recordings of a 1987 Miniscribe 8425, not of
  the drives FEPER fitted, which are not known. The levels against the speaker's were set by ear, not measured on a machine. Its seeks
  follow the Miniscribe's own timing, which is slower than the coupler's, so a quick run of seeks is heard as each one taking over the last.
- **Sound** is recorded to files and measured; the Linux sound output (ALSA, PulseAudio) has been
  exercised only through SDL's dummy driver.

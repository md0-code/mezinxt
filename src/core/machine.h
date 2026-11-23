/* The machine interface: everything the shell (src/app, src/platform)
 * knows of an emulated Junior.
 *
 * The core has no SDL, no OS call and no wall clock. Guest time is a count
 * of periods of the board's 14.31818 MHz crystal (MX_OSC_*, hw.h): every
 * run is deterministic at any host speed, and the shell paces it to wall
 * time (or, headless, runs a fixed amount of guest time per frame).
 *
 * mx_machine_create builds the Junior's board (hw/junior.h); when the
 * configured BIOS is not among the ROMs it builds the stub machine
 * (stub_machine.c) instead, a test screen in the Junior's own glyphs that
 * names the missing ROMs and echoes keys. mx_machine_create_stub always
 * builds the stub (checks of the shell, the configuration and the
 * presenter). */

#ifndef MX_MACHINE_H
#define MX_MACHINE_H

#include "../hw/hw.h"
#include "config.h"
#include "romdb.h"

#include <stddef.h>
#include <stdint.h>

typedef struct mx_machine mx_machine;

typedef enum mx_run_status {
    MX_RUN_OK,          /* ran the time asked */
    MX_RUN_HALTED,      /* the CPU halted with interrupts off: nothing more will happen */
    MX_RUN_ERROR        /* stopped: mx_machine_error says why */
} mx_run_status;

/* --- life cycle ----------------------------------------------------------------- */

/* A machine for the configuration (machine.*), with its ROMs taken from the
 * index (copied: the index may go afterwards). NULL with the reason. Media
 * are inserted afterwards with mx_machine_insert. */
mx_machine *mx_machine_create(const mx_config *config, const mx_rom_index *roms, char *error, size_t capacity);
mx_machine *mx_machine_create_stub(const mx_config *config, const mx_rom_index *roms, char *error, size_t capacity);
void mx_machine_destroy(mx_machine *m);
/* What runs, in words ("FEPER JUNIOR-XT, BIOS 4.0, 8086, 640 KiB"). */
const char *mx_machine_description(const mx_machine *m);
const char *mx_machine_error(const mx_machine *m);
/* Power cycle (power_on 1: the state at switching on, memory cleared) or
 * the board's RESET line (power_on 0: memory kept). */
void mx_machine_reset(mx_machine *m, int power_on);

/* --- time ----------------------------------------------------------------------- */

/* Runs until guest time has advanced by at least `ticks` OSC periods (the
 * last instruction may end past it). */
mx_run_status mx_machine_run(mx_machine *m, uint64_t ticks);
/* Guest time so far, in OSC periods. */
uint64_t mx_machine_time(const mx_machine *m);
/* The video frame's period in OSC periods (the CGA's 912 x 262 dots: 59.92
 * Hz): the shell presents one frame per period. */
uint64_t mx_machine_frame_period(const mx_machine *m);

/* --- the picture ---------------------------------------------------------------- */

/* A frame as the display adapter produced it: one byte per pixel, rows
 * `pitch` bytes apart, its size the adapter's own (the CGA's 640x200, the
 * monochrome coupler's 720x350). What an index means is the frame's
 * `format`: MX_FRAME_RGBI, an index into the CGA's 16 RGBI colours (0-15: bit
 * 0 blue, 1 green, 2 red, 3 intensity); MX_FRAME_MONO, the monochrome
 * coupler's levels (0 dark, 1 normal, 2 intense). The monitor makes colours of
 * them (mx_frame_argb). `aspect_w:aspect_h` is the shape the whole frame had
 * on the monitor (4:3 for both adapters). `serial` changes whenever the pixels
 * change. */
enum { MX_FRAME_RGBI, MX_FRAME_MONO };
typedef struct mx_frame {
    uint32_t width, height, pitch;
    const uint8_t *pixels;
    uint32_t aspect_w, aspect_h;
    uint64_t serial;
    uint32_t format;                    /* MX_FRAME_* */
} mx_frame;
/* 0 when the machine shows nothing (the frame is then black). */
int mx_machine_frame(mx_machine *m, mx_frame *frame);
/* The frame as ARGB8888 into out (width x height) through a monitor
 * (MX_MONITOR_*, config.h), the three outputs of the CGA card (video_out.h):
 * RGBI is the nine-pin connector's colour monitor (16 colours, colour 6
 * brown); RGB is the RCA jacks' monitor without intensity steps (the
 * intensity line is mixed into every channel: three levels a channel, 15
 * colours, colour 6 dark yellow); mono is the composite jack's monitor, the
 * brightness the resistor network gives each colour as 16 gray levels, softened
 * along each scanline by the monitor's limited video bandwidth (video_out.h:
 * the only monitor that blurs), which the presenter's phosphor tints. A
 * monochrome-coupler frame (MX_FRAME_MONO)
 * has no colour to choose: its levels are the gray levels of the monochrome
 * monitor, whatever the monitor (the shell pairs it with the green phosphor). */
void mx_frame_argb(const mx_frame *frame, uint32_t monitor, uint32_t *out);
/* The monochrome coupler's monitor for the shell's own CGA-format pictures (the setup screen): not a setting, one past the
 * catalogue's. The same gray levels as mono, sharp: the coupler's monitor is no composite monitor, so the bandwidth filter
 * (video_out.h) is not applied. */
#define MX_MONITOR_MDA 3u
/* The colour of an RGBI index on the RGBI monitor (0x00RRGGBB). */
uint32_t mx_rgbi_color(unsigned index);

/* The CGA's snow (hw/cga.h: a CPU cycle on the video store in 80-column text takes the fetch of one character cell of
 * the picture, and that cell shows what the card's bus carries) on or off, at once. A machine made from a
 * configuration starts with video.snow; the shell sets it again when the setup screen changes it. Off is not
 * original. A machine without a CGA (the monochrome coupler: its store never waits; the stub) ignores it. */
void mx_machine_set_snow(mx_machine *m, int on);

/* --- sound ---------------------------------------------------------------------- */

/* Interleaved stereo 16-bit frames at mx_machine_audio_rate (Hz; 0 when
 * the machine makes no sound): the frames the guest produced so far, at
 * most `frames`; returns how many were written. */
uint32_t mx_machine_audio_rate(const mx_machine *m);
size_t mx_machine_audio_pull(mx_machine *m, int16_t *stereo, size_t frames);
/* The drives' sound (hw/drivesnd.h: the diskette drives' motors, steps and head loads and the Winchester's spindle and
 * seeks, synthesised from what the drives do) is mixed into the frames above while it is on: `enabled` at `volume`
 * percent (0 to 100; 0 is off). A machine made from a configuration starts with sound.drives and sound.drives_volume;
 * the shell sets it again to follow the mute switch and the window. Off costs the chips nothing: they have no
 * observer. The stub machine has no drives to hear. */
void mx_machine_set_drive_sound(mx_machine *m, int enabled, unsigned volume);
/* For a machine that runs faster than real time (the shell's free running: a drive with fast access at work, speed unthrottled):
 * guest time passes much faster than the clock, so the frames above would be a fraction of a second of crowded
 * sound. Takes the speaker's frames up to now and drops them (it has no sound at that speed), and writes `frames`
 * frames of the drive sound as a summary at the pace of the clock (the caller asks for as many frames as the clock
 * has passed): the motors as they run, the steps and the seek ticks that happened as a stream of clicks at the
 * usual rate, in proportion. Returns `frames`; silence when the drive sound is off. */
size_t mx_machine_audio_free(mx_machine *m, int16_t *stereo, size_t frames);

/* --- input ---------------------------------------------------------------------- */

/* A key of the 83-key XT keyboard pressed (down 1) or released: its scan
 * code set 1 make code (01h-53h, IBM PC/XT Technical Reference); the
 * keyboard sends the release as make + 80h. 0 when the keyboard's queue is
 * full (offer it again later). */
int mx_machine_key(mx_machine *m, uint8_t make_code, int down);
/* Joystick `stick` (0 or 1) of the game control adapter (the board has one
 * when controllers.adapter says so; otherwise these calls do nothing):
 * axes from -32768 (left, up) to 32767, buttons in bits 0 and 1. A stick
 * counts only while it is plugged in; one that is not has no resistance on
 * its axes (they never time out, as an empty game port reads) and no
 * buttons. Sticks start unplugged. */
void mx_machine_joystick(mx_machine *m, unsigned stick, int x, int y, unsigned buttons);
void mx_machine_joystick_plug(mx_machine *m, unsigned stick, int plugged);
/* The host pointer over the picture, in frame pixels (the light pen on the
 * display coupler: its position, `inside` 0 when off the picture, button
 * bit 0 the pen's switch). The shell passes it only while the light pen is
 * enabled (controllers.light_pen); a pen needs a lit spot to see the beam
 * (hw/cga.h). */
void mx_machine_pointer(mx_machine *m, int x, int y, int inside, unsigned buttons);
/* Relative motion since the last call (positive to the right and down) and the buttons (bit 0 left, 1 right) for the
 * serial mouse on an 8250 port whose host end is the mouse (ports.com1 or com2: mouse); otherwise it does nothing. */
void mx_machine_mouse(mx_machine *m, int dx, int dy, unsigned buttons);

/* --- media ---------------------------------------------------------------------- */

/* Disk images are core/disk.h's (package P4a). The image stays the
 * caller's: the machine uses it until it is ejected or the machine is
 * destroyed, then the caller closes it. */
struct mx_disk;
typedef enum mx_media_kind { MX_MEDIA_FLOPPY, MX_MEDIA_HARD_DISK } mx_media_kind;
/* The drives of a kind the machine has. */
unsigned mx_machine_drives(const mx_machine *m, mx_media_kind kind);
int mx_machine_insert(mx_machine *m, mx_media_kind kind, unsigned drive, struct mx_disk *disk, char *error, size_t capacity);
/* The image in a drive, NULL when empty. */
struct mx_disk *mx_machine_media(const mx_machine *m, mx_media_kind kind, unsigned drive);
/* Takes the image out (the caller may close it then); 0 when empty. */
int mx_machine_eject(mx_machine *m, mx_media_kind kind, unsigned drive);
/* Activity for the status line: bit n set while diskette drive n's motor
 * runs, or while fixed disk n works on the Winchester coupler (and a fifth
 * of a second after its last command). */
unsigned mx_machine_drive_activity(const mx_machine *m, mx_media_kind kind);

/* --- ports ---------------------------------------------------------------------- */

/* The guest's byte ports: the SIO adapter's channels, the 8250 pair (an
 * expansion board), the parallel ports. The machine calls `transmit` for
 * every byte the guest sends and `outputs` when its handshake outputs
 * change; bytes from the host go in with mx_machine_serial_receive, which
 * returns how many the port took (offer the rest again later), and the
 * host side's modem inputs with mx_machine_serial_modem. A parallel port
 * only transmits: its modem call says whether a printer is connected and on
 * line (`dsr`); until one is, the port reads off line and busy, as with
 * nothing on the connector. The inputs of a serial port start deasserted.
 * `loopback` (optional) is asked for each port when the host is set: 1 when
 * its end is a loopback plug, which wires the channel's TxD to its own RxD
 * (the guest's own framing and baud rate, bit for bit); the channel then
 * sends nothing to `transmit`. */
enum { MX_PORT_SIO_A, MX_PORT_SIO_B, MX_PORT_COM1, MX_PORT_COM2, MX_PORT_LPT, MX_PORT_LPT_MDA, MX_PORT_COUNT };
typedef struct mx_serial_host {
    void *ctx;
    void (*transmit)(void *ctx, unsigned port, uint8_t byte);
    void (*outputs)(void *ctx, unsigned port, int dtr, int rts);
    int (*loopback)(void *ctx, unsigned port);
    /* Optional, asked for the 8250 ports (MX_PORT_COM1, MX_PORT_COM2) when the host is set: 1 when the port's end is
     * the serial mouse, which the machine runs itself from mx_machine_mouse; the host passes it no bytes. */
    int (*mouse)(void *ctx, unsigned port);
} mx_serial_host;
/* NULL: nothing connected (bytes sent are lost). */
void mx_machine_set_serial(mx_machine *m, const mx_serial_host *host);
size_t mx_machine_serial_receive(mx_machine *m, unsigned port, const uint8_t *data, size_t count);
void mx_machine_serial_modem(mx_machine *m, unsigned port, int dcd, int cts, int dsr, int ri);

/* --- checks --------------------------------------------------------------------- */

/* A trace of the bus for headless checks and diagnosis:
 * kind MX_TRACE_* with the address and value of each such cycle. NULL
 * stops it. */
enum { MX_TRACE_IO_READ, MX_TRACE_IO_WRITE, MX_TRACE_FETCH, MX_TRACE_INTERRUPT };
typedef void (*mx_trace_fn)(void *ctx, int kind, uint32_t address, uint32_t value);
void mx_machine_set_trace(mx_machine *m, mx_trace_fn fn, void *ctx);
/* A byte of the memory space as the CPU would read it (0xFF where nothing
 * answers; no side effects). 0 when the machine has no memory (the stub). */
int mx_machine_peek(mx_machine *m, uint32_t address, uint8_t *value);

/* The core assumes a little-endian host with 8-bit bytes: 1 when this host is one. The shell checks it at start, and
 * mx_machine_create refuses to run otherwise. */
int mx_machine_host_ok(void);

#endif /* MX_MACHINE_H */

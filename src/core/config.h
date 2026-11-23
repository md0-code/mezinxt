/* mezinxt.yaml: the settings of the emulated Junior and of its window.
 * libyaml reads the YAML syntax; this module enforces MezinXT's schema
 * (unknown keys, duplicates and bad values refused with their line) and
 * writes settings back through the comment-keeping editor (yamledit.h). */

#ifndef MX_CONFIG_H
#define MX_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define MX_CONFIG_PATH 1024
#define MX_CONFIG_DRIVES 4              /* drives.a to drives.d: the adapter drives 1 to 4 diskette drives */
#define MX_CONFIG_ROMS 3                /* machine.rom_c8000, rom_f0000, rom_f8000: the RAM module's EPROM sockets */
#define MX_CONFIG_HARD 2                /* drives.hd0, drives.hd1: the Winchester coupler's two fixed disks */
#define MX_CONFIG_PORTS 6               /* ports.sio_a, sio_b, lpt, com1, com2, lpt_mda */
#define MX_CONFIG_STICKS 2              /* controllers.joystick1, controllers.joystick2 */
#define MX_CONFIG_PADS 8                /* host game controllers numbered 1 to 8 */
#define MX_CONFIG_STAND_KEYS 6          /* controllers.keys: the keyboard stand-in's up, down, left, right, fire1 and fire2 */
#define MX_CONFIG_KEY_NAME 32           /* a host key's name (SDL's: "Left Ctrl") with its end */

/* machine.bios: the four archived FEPER BIOS versions (romdb.h). */
enum { MX_BIOS_3_0, MX_BIOS_4_0, MX_BIOS_5_0, MX_BIOS_5_0_ALT, MX_BIOS_COUNT };
/* machine.cpu */
enum { MX_CPU_8086, MX_CPU_8088 };
/* machine.display: the display coupler the DIP switches announce (SW5-6):
 * the CGA in 80- or 40-column mode, the monochrome coupler (MDA, 80 columns; its
 * monitor is the monochrome one in green phosphor, whatever video.monitor says),
 * or none (the BIOS then takes its no-display path). One coupler: the shell has one
 * screen, so the CGA and the MDA are not fitted together. */
enum { MX_DISPLAY_NONE, MX_DISPLAY_CGA40, MX_DISPLAY_CGA80, MX_DISPLAY_MDA };
/* machine.speed: the original 4.77 MHz (OSC/3), the non-original 7.16
 * (OSC/2) and 9.54 MHz (OSC/1.5), or as fast as the host runs. */
enum { MX_SPEED_4_77, MX_SPEED_7_16, MX_SPEED_9_54, MX_SPEED_UNTHROTTLED };
/* video.look */
enum { MX_LOOK_PLAIN, MX_LOOK_CRT };
/* video.monitor: the catalogue's monitors, one for each of
 * the CGA card's outputs (video_out.h): RGBI colour (the nine-pin connector),
 * RGB without intensity steps (the RCA jacks), composite monochrome in green
 * phosphor (the composite jack). */
enum { MX_MONITOR_RGBI, MX_MONITOR_RGB, MX_MONITOR_MONO_GREEN };
/* video.aspect, video.scale, video.filter */
enum { MX_CONFIG_ASPECT_ORIGINAL, MX_CONFIG_ASPECT_SQUARE };
enum { MX_CONFIG_SCALE_FIT, MX_CONFIG_SCALE_INTEGER };
enum { MX_CONFIG_FILTER_NEAREST, MX_CONFIG_FILTER_LINEAR, MX_CONFIG_FILTER_SHARP };
/* ports.*: the host end of a guest port: nothing, a host serial port, a
 * local pipe, an output file, a plug that wires the channel's TxD to its RxD
 * (loopback, the serial ports only), a TCP connection made to HOST:PORT or a
 * TCP server waiting on [HOST:]PORT (the local interface unless HOST is
 * given), or the Microsoft serial mouse fed by the host mouse (the 8250
 * ports com1 and com2 only). */
enum { MX_END_NONE, MX_END_SERIAL, MX_END_PIPE, MX_END_OUTPUT, MX_END_LOOPBACK, MX_END_TCP, MX_END_TCP_LISTEN, MX_END_MOUSE };
enum { MX_CONFIG_PORT_SIO_A, MX_CONFIG_PORT_SIO_B, MX_CONFIG_PORT_LPT, MX_CONFIG_PORT_COM1, MX_CONFIG_PORT_COM2, MX_CONFIG_PORT_LPT_MDA };
/* ports.pc_serial: the PC/XT asynchronous adapter (a pair of 8250s at 3F8h and 2F8h, IRQ 4 and 3), fitted with
 * none, one (COM1) or both ports. COM1 uses IRQ 4, which the SIO adapter drives: they exclude each other. */
enum { MX_PCSERIAL_NONE, MX_PCSERIAL_ONE, MX_PCSERIAL_TWO };
/* ui.language: the language of the setup screen and of the messages shown
 * in the window (text.h); the command line's help and the diagnostics on
 * stderr stay English. */
enum { MX_LANG_EN, MX_LANG_RO, MX_LANG_COUNT };

typedef struct mx_drive_config {
    char image[MX_CONFIG_PATH];         /* empty: an empty drive */
    int write_protect;
    uint32_t tracks;                    /* 40 or 80: the drive's cylinders (an 80-track drive reads 720 KiB disks) */
    int fast;                           /* not original: the shell runs unthrottled while this drive works (its motor turns); the fixed disks use it too */
} mx_drive_config;

/* controllers.joystickN.device: what feeds a joystick of the game control
 * adapter: nothing, the keyboard stand-in (arrows and two keys) or a host
 * game controller by its number (the order the host reports them). */
enum { MX_STICK_NONE, MX_STICK_KEYBOARD, MX_STICK_CONTROLLER };

typedef struct mx_stick_config {
    uint32_t device;                    /* MX_STICK_* */
    uint32_t pad;                       /* MX_STICK_CONTROLLER: the controller, 1 to MX_CONFIG_PADS */
    int dead_zone;                      /* percent of the travel that counts as centred, 0 to 50 */
    int range;                          /* percent of the travel that gives full deflection, 50 to 100 */
    int centre_x, centre_y;             /* where the stick rests, percent of the travel, -50 to 50 */
} mx_stick_config;

typedef struct mx_port_config {
    uint32_t type;                      /* MX_END_* */
    char path[MX_CONFIG_PATH];          /* device, pipe, output file, HOST:PORT or [HOST:]PORT */
} mx_port_config;

typedef struct mx_config {
    /* machine: ram_kb is 64, 128, 192 or 256 (the board's banks, DIP switches SW3-4) or 640 (256 with the
     * 384 KiB module, which carries the EPROM sockets: rom[] are images for them, empty for none);
     * floppies is the number of diskette drives fitted (SW7-8) */
    uint32_t bios, cpu, speed, ram_kb, display, floppies;
    int fpu;                            /* the 8087 math processor is fitted (DIP switch SW2) */
    int winchester;                     /* the Winchester coupler (fixed disk adapter, its ROM at C8000h) is fitted */
    char rom[MX_CONFIG_ROMS][MX_CONFIG_PATH];
    /* drives */
    mx_drive_config drives[MX_CONFIG_DRIVES];
    mx_drive_config hard[MX_CONFIG_HARD];  /* the fixed disks' images and fast access (the other fields are not used) */
    /* video */
    uint32_t look, monitor, aspect, scale, filter;
    int fullscreen, vsync;
    int snow;                           /* the CGA's snow in 80-column text (the real card's behaviour; off is not original) */
    int drive_lights;                   /* not original: the drive activity lights drawn over the picture's bottom row */
    /* sound: enabled is the mute switch, volume 0 to 100 percent; drive_sound the synthesised sound of the disk drives
     * (hw/drivesnd.h) at drive_volume percent (0 to 100) */
    int sound, volume;
    int drive_sound, drive_volume;
    /* ports: the serial and parallel adapter in its slot, and what its connectors lead to */
    int sio_adapter;
    uint32_t pc_serial;                 /* MX_PCSERIAL_* */
    mx_port_config ports[MX_CONFIG_PORTS];
    /* controllers: the game control adapter (joystick coupler) fitted, the host mouse as a light pen on the CGA */
    int game_adapter, light_pen;
    mx_stick_config sticks[MX_CONFIG_STICKS];
    /* the keyboard stand-in's keys, as SDL names them (what --hostkey takes): up, down, left, right, fire 1, fire 2 */
    char stand_keys[MX_CONFIG_STAND_KEYS][MX_CONFIG_KEY_NAME];
    /* ui */
    uint32_t language;                  /* MX_LANG_*; the default is the host's own (mx_config_default_language) */
    /* the file this was read from (relative paths in it are relative to
     * its folder) */
    char source[MX_CONFIG_PATH];
} mx_config;

void mx_config_defaults(mx_config *config);
/* Reads a file; 0 with a message naming the line when it is not valid. */
int mx_config_load(const char *path, mx_config *config, char *error, size_t capacity);
/* The same from text; source is the file it stands for (relative paths). */
int mx_config_parse(const char *text, size_t length, const char *source, mx_config *config, char *error, size_t capacity);

/* Every setting of config written into doc (yamledit.h) at its key: only
 * values change, comments and order stay. Paths inside the configuration's
 * folder are written relative to it. 0 when out of memory. */
struct mx_ydoc;
int mx_config_store(const mx_config *config, struct mx_ydoc *doc);

/* The names the file uses for the choice values ("4.0", "8088", "mono-green"). */
const char *mx_config_bios_name(uint32_t bios);
const char *mx_config_cpu_name(uint32_t cpu);
const char *mx_config_speed_name(uint32_t speed);
const char *mx_config_display_name(uint32_t display);
const char *mx_config_look_name(uint32_t look);
const char *mx_config_monitor_name(uint32_t monitor);
const char *mx_config_aspect_name(uint32_t aspect);
const char *mx_config_scale_name(uint32_t scale);
const char *mx_config_filter_name(uint32_t filter);
const char *mx_config_pc_serial_name(uint32_t pc_serial);   /* "two" */
const char *mx_config_port_key(unsigned port);          /* "sio_a" */
/* 1 for the two parallel ports (the adapter's, the monochrome coupler's): they take none or an output file only. */
int mx_config_port_is_parallel(unsigned port);
/* 1 when `target` suits a port of that type (a TCP end needs HOST:PORT, a
 * listener [HOST:]PORT, the port 1 to 65535, the others a name); else 0 with
 * the reason. */
int mx_config_port_check(uint32_t type, const char *target, char *error, size_t capacity);
/* The same as a code, for a message in the user's language: MX_PORT_OK or what is wrong. */
enum { MX_PORT_OK, MX_PORT_NEEDS_HOST_PORT, MX_PORT_BAD_TCP_PORT, MX_PORT_LISTEN_FORM, MX_PORT_EMPTY };
int mx_config_port_problem(uint32_t type, const char *target);
/* A port as the file words it: "none", "loopback", "COM3", "pipe:NAME",
 * "tcp:HOST:PORT", "tcp-listen:PORT", "file:PATH" (a path inside the
 * configuration's folder relative to it). */
const char *mx_config_port_text(const mx_config *config, const mx_port_config *port, char *buffer, size_t capacity);
/* 1 when the settings can be on one board; else 0 with the reason in the words of the file's keys: the 8250 adapter
 * and the SIO adapter both drive IRQ 4, and the host has one mouse. */
int mx_config_check(const mx_config *config, char *error, size_t capacity);
/* The fixed disks' keys ("hd0"). */
const char *mx_config_hard_key(unsigned drive);
/* 1 when a drive at work has fast access (`fast`): the shell then runs unthrottled. The masks are machine.h's drive activity: bit n of
 * floppy_active a diskette drive's motor turning, bit n of hard_active a fixed disk working. */
int mx_config_fast_active(const mx_config *config, unsigned floppy_active, unsigned hard_active);
/* 1 when a serial mouse is on a port that is on the board (ports.pc_serial fits it, ports.com1 or com2 is mouse). */
int mx_config_mouse_attached(const mx_config *config);
/* The EPROM sockets of the RAM module: the key ("rom_c8000") and the address ("C8000h" as a number). */
const char *mx_config_rom_key(unsigned socket);
uint32_t mx_config_rom_address(unsigned socket);
/* 1 when `kb` is a memory size the board takes: 64, 128, 192, 256 or 640. */
int mx_config_ram_valid(uint32_t kb);
const char *mx_config_language_name(uint32_t language); /* "en" */
/* The language a host locale name stands for ("ro-RO", "ro_RO.UTF-8"):
 * Romanian for ro, English for any other. */
uint32_t mx_config_language_for_locale(const char *locale);
/* The language of the host's user locale (host.h); English when it is unknown. */
uint32_t mx_config_default_language(void);
/* The keyboard stand-in's keys (controllers.keys): the file's key of each ("up", "fire2") and its default key name ("Up", "Left Shift"). */
const char *mx_config_stand_key_key(unsigned function);
const char *mx_config_stand_key_default(unsigned function);
/* 1 while the four directions are the arrow keys. */
int mx_config_stand_arrows(const mx_config *config);
/* The reader cannot know the host's key names (src/core has no SDL): the shell gives it a way to ask, which refuses a name the
 * host has no key of; without one any name is taken. */
void mx_config_set_key_check(int (*known)(const char *name));
/* Whether `name` can be the key of a function of the stand-in: MX_KEY_OK; MX_KEY_BAD when it is no name the host knows; MX_KEY_RESERVED
 * when the window keeps it (F11, F12, Pause, Print Screen: *reserved is how it is written); MX_KEY_USED when another function has it
 * (*other is that one). */
enum { MX_KEY_OK, MX_KEY_BAD, MX_KEY_RESERVED, MX_KEY_USED };
int mx_config_stand_key_problem(const mx_config *config, unsigned function, const char *name, const char **reserved, unsigned *other);
/* The device as the file words it: "none", "keyboard", "controller 2". */
const char *mx_config_stick_device_name(const mx_stick_config *stick, char *buffer, size_t capacity);

#endif /* MX_CONFIG_H */

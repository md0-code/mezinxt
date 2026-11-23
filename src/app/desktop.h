/* The window: runs a machine paced to wall time (or, headless, a fixed
 * amount of guest time per frame, so a run is the same on any host), shows
 * its frames through the presenter, plays its sound and gives it the
 * keyboard.
 *
 * With the game control adapter fitted (controllers.adapter) a joystick is fed
 * by a host game controller (its stick or d-pad, A and B as the two buttons) or
 * by the keyboard stand-in: the arrow keys move the stick and Left Ctrl and Left
 * Shift are its buttons 1 and 2 (controllers.keys names the six keys); while the
 * stand-in is assigned these six keys are the stick's and no longer reach the
 * guest, until F11 gives them back to the computer (and again). With the light pen
 * enabled (controllers.light_pen) the pointer over the picture is the pen and
 * the left mouse button its switch. With a serial mouse on an 8250 port
 * (ports.com1 or com2: mouse, the port fitted by ports.pc_serial) a click on the
 * picture captures the host mouse: the pointer is hidden and held in the
 * window, its motion and its left and right buttons are the serial mouse's
 * (the click itself only captures), and Ctrl+F10 lets it go. The mouse is
 * released too when the setup screen opens, the machine is paused or the
 * window loses the keyboard. While it is captured it is not the light pen.
 *
 * Keys of the window (they never reach the guest): Alt+Enter full screen, F11 the keyboard
 * joystick's keys to the joystick or to the computer, F12 the setup screen, Shift+F12 exits MezinXT (at once; what was changed on the setup screen is saved),
 * Ctrl+F12 a power cycle (a cold reset), Alt+F12 a system reset, Pause stops and
 * starts the machine, Print Screen saves a screenshot (PNG, mezinxt-NNNN.png
 * in the working folder), Ctrl+Pause sends Ctrl-Break and Ctrl+Alt+End
 * sends Ctrl-Alt-Del (the host keeps the real one). Every other host key
 * is the XT key in the same place (keymap.c). A file dropped on the window
 * goes into drive A (drive B with Shift held; of two dropped together, the
 * second goes into B). */
#ifndef MX_DESKTOP_H
#define MX_DESKTOP_H

#include "../core/machine.h"
#include "../core/romdb.h"

#define MX_DESKTOP_KEYS 1024
#define MX_DESKTOP_SCRIPT 64

/* A step of a checks script, run at a frame: a host key pressed and released
 * (or only one of the two) with the modifiers held, a file dropped on the
 * window, the window losing the keyboard, a joystick set to a position and
 * buttons (it is plugged in from then on, whatever controllers.joystickN says:
 * the host devices no longer feed it) or unplugged, or the host pointer put on
 * a pixel of the frame with the pen's switch or taken off the picture (the
 * light pen must be enabled for it to reach the machine), or the host mouse
 * clicked on the picture (to capture it) or moved while it is captured. They
 * go through the same code as the host's events. */
enum { MX_SCRIPT_HOSTKEY, MX_SCRIPT_DROP, MX_SCRIPT_FOCUS_LOST, MX_SCRIPT_JOY, MX_SCRIPT_POINTER, MX_SCRIPT_MOUSE };
enum { MX_SCRIPT_SHIFT = 1, MX_SCRIPT_CTRL = 2, MX_SCRIPT_ALT = 4, MX_SCRIPT_PRESS = 8, MX_SCRIPT_RELEASE = 16 };
typedef struct mx_desktop_script {
    uint64_t at;
    int kind;
    int scancode;               /* HOSTKEY: an SDL scancode */
    unsigned mods;              /* MX_SCRIPT_* held */
    const char *path;           /* DROP: the file */
    unsigned stick;             /* JOY: the joystick, 0 or 1 */
    int x, y;                   /* JOY: the stick position (-32768 to 32767); POINTER: the pen's pixel of the frame */
    unsigned buttons;           /* JOY: bits 0 and 1; POINTER: bit 0, the pen's switch; MOUSE: left and right */
    int off;                    /* JOY: unplugged; POINTER: off the picture */
    int click;                  /* MOUSE: a click on the picture (x, y, buttons unused), else relative motion x, y with the buttons */
} mx_desktop_script;

typedef struct mx_desktop_options {
    int hidden;                 /* no visible window, no sound device: headless and deterministic */
    int realtime;               /* with hidden: paced to wall time as a window is (checks of the pacing) */
    uint64_t frames;            /* stop after this many frames; 0: run until closed */
    const char *screenshot;     /* save the last frame, as a PNG when the name ends in .png, else a BMP */
    const char *wav;            /* record the sound to this WAV file instead of playing it */
    /* Checks: key transitions (make codes, bit 7 a release) after given frames. */
    uint64_t key_at[MX_DESKTOP_KEYS];
    uint8_t key_code[MX_DESKTOP_KEYS];
    unsigned key_count;
    /* Checks: host keys and dropped files at given frames, in the order of their frames. */
    mx_desktop_script script[MX_DESKTOP_SCRIPT];
    unsigned script_count;
    /* The setup screen: the ROMs it offers and draws with, whether the
     * machine is the test screen (a rebuilt machine is too), the page to
     * open at the start (-1 none; the machine stays paused while it is
     * open) and the trace a rebuilt machine gets again. */
    const mx_rom_index *roms;
    int stub;
    int setup_page;
    mx_trace_fn trace;
    void *trace_ctx;
    /* The open disk images of the floppy drives (core/disk.h), the caller's
     * (NULL: none): the shell opens, replaces and closes them when the setup
     * screen changes a drive and puts them in a rebuilt machine again. */
    struct mx_disk **disks;
} mx_desktop_options;

/* 0 on a normal end; otherwise error says why. The machine may be replaced
 * while it runs (the setup screen's machine settings rebuild it): *machine
 * is the one to destroy afterwards. */
int mx_desktop_run(mx_machine **machine, const mx_config *config, const mx_desktop_options *options, char *error,
                   size_t capacity);

/* The 83 keys of the XT keyboard: the scan code set 1 make code, the host
 * key (an SDL scancode) in the same place, the key's name (keymap.c). */
typedef struct mx_key_def {
    uint8_t code;
    int scancode;
    const char *name;
} mx_key_def;
const mx_key_def *mx_desktop_keys(size_t *count);
/* The XT set 1 make code of an SDL key (the table and the host keys that do
 * the work of a key without sitting where it does), -1 for none. */
int mx_desktop_xt_key(int sdl_scancode);
/* 1 for Caps, Num and Scroll Lock, which a press gives the guest whole. */
int mx_desktop_key_is_lock(int sdl_scancode);
/* The XT key typing an ASCII character on the US layout, *shift whether Shift
 * is held; -1 for none (keymap.c). */
int mx_desktop_char_key(int ch, int *shift);

#endif /* MX_DESKTOP_H */

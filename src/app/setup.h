/* The setup screen: the pages Drives, Machine,
 * Display, Sound, Ports and Controllers, drawn with the text-mode toolkit
 * (tui.h) over a copy of the configuration. Changes are made to the copy;
 * on leaving, the copy is written back to mezinxt.yaml through the
 * comment-keeping editor (mx_config_store) and the shell applies what
 * changed (mx_setup_diff): the display at once, the machine and the ports
 * when the screen is left. No SDL and no OS call here beyond the host
 * layer's folders; the shell (desktop.c) feeds it keys and shows its
 * frame. MezinXT's own code. */
#ifndef MX_SETUP_H
#define MX_SETUP_H

#include "../core/config.h"
#include "../core/romdb.h"
#include "tui.h"

enum { MX_SETUP_DRIVES, MX_SETUP_MACHINE, MX_SETUP_DISPLAY, MX_SETUP_SOUND, MX_SETUP_PORTS, MX_SETUP_CONTROLLERS, MX_SETUP_PAGES };
/* "drives", "machine", ...; the number of a name, -1 for an unknown one. */
const char *mx_setup_page_name(int page);
int mx_setup_page_by_name(const char *name);

/* What a change of the configuration asks of the shell. */
enum {
    MX_CHANGE_RESTART = 1,      /* machine.*: the machine is rebuilt (a reset on leaving) */
    MX_CHANGE_VIDEO = 2,        /* video.*: applied at once (the drive lights too) */
    MX_CHANGE_PORTS = 4,        /* ports.*: the host ends are reopened on leaving */
    MX_CHANGE_DRIVES = 8,       /* drives.*: disk images */
    MX_CHANGE_CONTROLLERS = 16, /* controllers.* */
    MX_CHANGE_SOUND = 32,       /* sound.*: applied on leaving, the machine runs on */
    MX_CHANGE_LANGUAGE = 64,    /* ui.language: applied at once (text.h) */
    MX_CHANGE_DISK_SPEED = 128  /* drives.*.fast: copied into the live settings on leaving, the machine runs on */
};
unsigned mx_setup_diff(const mx_config *before, const mx_config *after);

typedef struct mx_setup mx_setup;

typedef struct mx_setup_env {
    void *ctx;
    const uint8_t *font;                /* the Junior's character ROM, 2048 bytes; NULL: MezinXT's fallback */
    const mx_rom_index *roms;           /* the BIOS ROMs present; NULL: all of them */
    const char *version;                /* for the title bar */
    unsigned drives;                    /* floppy drives the machine has */
    int fullscreen;                     /* the window is full screen now */
    int no_crt;                         /* the CRT shader is not available: the CRT look is shown plain, so the filter acts (0: it is there) */
    /* A drive's disk is changed by the user: `path` the image to put in it
     * (with its write protection), "" to take the disk out (machine.h's
     * mx_machine_insert and mx_machine_eject). Drives MX_CONFIG_DRIVES and
     * MX_CONFIG_DRIVES + 1 are the Winchester coupler's fixed disks 0 and 1
     * (disk.h's MX_DISK_SLOT_HARD); a change of them restarts the machine on
     * leaving. 0 with a message when it cannot be done. */
    int (*media)(void *ctx, unsigned drive, const char *path, int write_protect, char *error, size_t capacity);
} mx_setup_env;

mx_setup *mx_setup_create(const mx_config *config, const mx_setup_env *env);
void mx_setup_destroy(mx_setup *s);
void mx_setup_open_page(mx_setup *s, int page);
int mx_setup_page(const mx_setup *s);

/* The host's serial ports ("COM3"), as found when the Ports page opens
 * (mx_host_list_serial) unless the shell gives them here (tests): the
 * Ports page offers a host serial port only when there is one. */
void mx_setup_set_serial_ports(mx_setup *s, const char *const *names, unsigned count);

/* The host's game controllers as the shell sees them (names in the order
 * the host reports them), and where the first stick of controller `index`
 * (0-based) is now, -32768 to 32767, for the calibration. */
void mx_setup_set_controllers(mx_setup *s, const char *const *names, unsigned count);
void mx_setup_pad_axes(mx_setup *s, unsigned index, int x, int y);

/* Input: an event, or an XT key (set 1 make code, bit 7 a release; the
 * keys of the command line's --key and of a headless run). On a page,
 * Left and Right (and Tab) turn the pages, Up and Down select an option,
 * Page Up and Page Down change its value, Enter opens or flips it and
 * Escape leaves. */
void mx_setup_event(mx_setup *s, mx_tui_event event);
void mx_setup_xt_key(mx_setup *s, uint8_t code);
/* The user asked to leave (F12, a controller's Start): any pop-up closes
 * and the screen is left, after the questions leaving can raise. */
void mx_setup_request_close(mx_setup *s);

/* The Controllers page's Keys... pop-up (the keyboard joystick's six keys) waits for the next host key press: while it does, the shell
 * gives the setup screen every key (the window's own included) by its name, as SDL names it, with the modifiers held, and the
 * screen takes it, or refuses it with a message (a key the window keeps, or one another function has), or, for Escape, stops waiting. */
enum { MX_HOSTKEY_CTRL = 1, MX_HOSTKEY_ALT = 2 };
int mx_setup_wants_host_key(const mx_setup *s);
void mx_setup_host_key(mx_setup *s, const char *name, unsigned modifiers);
/* The four directions of the keyboard joystick as a message or a hint names them: "the arrow keys" (capital: "The arrow keys") while
 * they are the arrows, else the keys' names. */
const char *mx_setup_stand_directions(const mx_config *config, int capital, char *out, size_t capacity);

/* The screen as a frame; `cursor_on` blinks the text cursor. */
void mx_setup_frame(mx_setup *s, int cursor_on, mx_frame *frame);

/* The screen as it was last drawn (mx_setup_frame): its cells, and how many
 * texts were cut short in it (tui.h's clipped count; a path cut at its start
 * is not one), for the checks. */
const mx_tui *mx_setup_screen(const mx_setup *s);
unsigned mx_setup_clipped(const mx_setup *s);
/* 1 while a pop-up (list, question, input line, file picker, calibration) is in front of the page. */
int mx_setup_popup(const mx_setup *s);
/* The page's list as it is now, for the checks: its rows, the selected one and the first one shown (the window shows 18). */
void mx_setup_list_state(const mx_setup *s, size_t *count, size_t *selected, size_t *top);

/* The edited configuration (video options as they are now). */
const mx_config *mx_setup_config(const mx_setup *s);
/* 1 once the screen is left: the settings are saved and the shell may
 * apply them; mx_setup_reset says whether the user asked for a reset. */
int mx_setup_done(const mx_setup *s);
enum { MX_SETUP_RESET_NONE, MX_SETUP_RESET_WARM, MX_SETUP_RESET_POWER };
int mx_setup_reset(const mx_setup *s);
/* Exit MezinXT, at once: the Machine page's Exit item and the shell's Shift+F12 while the screen is open (any pop-up in front is
 * closed). The screen is left as ever, the settings changed in it saved, without the restart question (nothing is rebuilt), and
 * mx_setup_exit is then 1: the shell ends the program, which writes the disk images back. When the settings cannot be saved the
 * message comes first (and the program goes on). */
void mx_setup_request_exit(mx_setup *s);
int mx_setup_exit(const mx_setup *s);

/* Writes the edited configuration to the file it was read from, keeping
 * the file's comments (nothing to write when it has no file); 0 with a
 * message when that fails. */
int mx_setup_save(mx_setup *s, char *error, size_t capacity);

#endif /* MX_SETUP_H */

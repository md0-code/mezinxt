/* The window (desktop.h): the machine paced to wall time, its frames
 * through the presenter, its sound, the keyboard as XT scan codes, Alt+Enter for
 * full screen, F11 for the keyboard joystick and F12 for the setup screen, which pauses the machine and
 * shows its text-mode picture through the same presenter and monitor.
 * Headless (--hidden) each frame is exactly one frame period of guest time
 * and nothing depends on the host's clock, so the same command gives the
 * same frames anywhere (--realtime paces a hidden window to the clock too,
 * for checks of the pacing). Messages and the drive lights (video.drive_lights) are cells of the
 * Junior's own character set drawn into the frame, so they pass through
 * the monitor and the shader with the screen. */
#include "desktop.h"
#ifndef _WIN32
#include "icon_data.h"
#endif
#include "../core/disk.h"
#include "../core/stick.h"
#include "../hw/mda.h"
#include "../core/text.h"
#include "../host/host.h"
#include "pace.h"
#include "pads.h"
#include "setup.h"
#include "tui.h"
#include "../platform/audio.h"
#include "../platform/overlay.h"
#include "../platform/serial.h"
#include "../platform/video.h"
#include "mx_version.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TITLE "MezinXT"
/* Pacing: lags up to 50 ms of wall time are caught up; a longer one (a
 * stalled host) is dropped, so the machine never races to catch up. */
#define CATCH_UP_MS 100
#define STATUS_ROW (MX_TUI_ROWS - 1)
#define STATUS_ATTR MX_ATTR(MX_BLACK, MX_LIGHT_GRAY)
#define STATUS_HARD_ATTR MX_ATTR(MX_WHITE, MX_RED)      /* a fixed disk's light: its DOS letter, C: or D: */

typedef struct shell {
    mx_machine *m;
    mx_machine **machine_out;
    mx_config live;
    const mx_desktop_options *options;
    mx_video *video;
    mx_osd osd;
    int hidden, tint;           /* tint: a phosphor drawn here (no shader) */
    int det;                    /* hidden and not paced: each frame is one frame period of guest time */
    int paused;                 /* the Pause key */
    int quit;                   /* Shift+F12 or the setup screen's Exit item: the run ends, the settings changed in the setup screen saved */
    /* keys the machine has not taken yet (bit 7 a release) */
    uint8_t pending[256];
    unsigned read, write;
    uint8_t down[128];          /* host keys holding each XT key */
    uint8_t sent[SDL_NUM_SCANCODES];   /* the XT key each held host key gave */
    /* the machine's host ends */
    mx_host_audio audio;
    int audio_open;
    mx_wav wav;
    mx_host_serial serial;
    mx_serial_host serial_host;
    uint32_t *pixels;
    size_t pixels_size;
    uint8_t *copy;              /* the machine's frame with the status cells drawn in */
    size_t copy_size;
    uint32_t width, height;
    char error[256];
    /* the setup screen */
    mx_setup *setup;
    uint8_t font[2048];
    int have_font;
    mx_pads pads;
    int pads_open;
    uint64_t shown;             /* pictures shown: the setup screen's cursor blinks with them */
    int resync;                 /* the pacing starts again from now (after the machine was replaced) */
    /* the status cells and the screenshot asked for by a key */
    mx_tui *status;
    int shot_wait;
    int script_shift;           /* a scripted drop with Shift held */
    unsigned drop_count;        /* files dropped so far together */
    /* the game control adapter's joysticks and the light pen */
    uint8_t stand[MX_CONFIG_STAND_KEYS];   /* the keyboard stand-in's host keys held for the joystick: up, down, left, right, button 1, button 2 */
    int stand_sc[MX_CONFIG_STAND_KEYS];    /* the host keys of controllers.keys, as SDL scancodes */
    int keys_to_stick;          /* F11: the stand-in's six keys are the joystick's (1) or the computer's own (0) */
    int scripted[2];            /* a --joy step took this joystick: no host device feeds it */
    int pen_inside, pen_x, pen_y;
    unsigned pen_b;             /* the host pointer over the picture (frame pixels) and the left button */
    int pen_told, pen_told_inside, pen_told_x, pen_told_y;
    unsigned pen_told_b;        /* what the machine was last told of it */
    /* the serial mouse: the host mouse captured, its motion since the last frame and its buttons */
    int captured;
    int mouse_dx, mouse_dy;
    unsigned mouse_buttons;
    int mouse_told;
    unsigned mouse_told_buttons;
} shell;

static uint32_t now_ms(const shell *s) { return s->det ? (uint32_t)(s->shown * 1000u / 60u) : SDL_GetTicks(); }

static void say(shell *s, const char *format, ...) {
    char text[sizeof s->osd.text];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    mx_osd_show(&s->osd, now_ms(s), "%s", text);
}

/* A message of the string table (text.h), in the language in use, with its values. */
static void say_id(shell *s, int id, ...) {
    char text[sizeof s->osd.text];
    va_list args;
    va_start(args, id);
    vsnprintf(text, sizeof text, mx_t(id), args);
    va_end(args);
    mx_osd_show(&s->osd, now_ms(s), "%s", text);
}

/* --- the keyboard ----------------------------------------------------------------------- */

static int queue_key(shell *s, uint8_t code, int down) {
    if (((s->write + 1) & 255u) == s->read) return 1;          /* full: dropped, as a real buffer overruns */
    s->pending[s->write] = (uint8_t)(code | (down ? 0 : 0x80));
    s->write = (s->write + 1) & 255u;
    return 1;
}

/* A host key goes down or up for the guest. Each host key gives up the XT
 * key it pressed, whatever the modifiers have become; two host keys on one
 * XT key (the two Ctrl keys) release it when both are up; a lock key is
 * pressed and released at once, so a host that reports a toggle with one
 * event still gives the BIOS a make and a break. */
static void guest_key(shell *s, int sc, int down, unsigned mod) {
    if (sc < 0 || sc >= SDL_NUM_SCANCODES) return;
    if (down) {
        if (s->sent[sc]) return;
        int code = mx_desktop_xt_key(sc);
        if (sc == SDL_SCANCODE_END && (mod & KMOD_CTRL) && (mod & KMOD_ALT)) code = 0x53;   /* Ctrl-Alt-Del, which the host keeps for itself */
        if (code <= 0) return;
        if (mx_desktop_key_is_lock(sc)) {
            queue_key(s, (uint8_t)code, 1);
            queue_key(s, (uint8_t)code, 0);
            return;
        }
        s->sent[sc] = (uint8_t)code;
        if (s->down[code]++ == 0) queue_key(s, (uint8_t)code, 1);
    } else {
        const int code = s->sent[sc];
        if (!code) return;
        s->sent[sc] = 0;
        if (--s->down[code] == 0) queue_key(s, (uint8_t)code, 0);
    }
}

static void release_all(shell *s) {
    memset(s->sent, 0, sizeof s->sent);
    for (unsigned k = 0; k < 128; ++k)
        if (s->down[k]) { s->down[k] = 0; queue_key(s, (uint8_t)k, 0); }
}

/* --- the joysticks and the light pen --------------------------------------------------------- */

/* The stand-in's keys: slots 0-5 are up, down, left, right, fire 1 and fire 2, each a host key named by controllers.keys (SDL's
 * key name, resolved here; a name SDL does not know gives the default key). -1 for any other key. */
static const int STAND_DEFAULT[MX_CONFIG_STAND_KEYS] = {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
                                                        SDL_SCANCODE_LCTRL, SDL_SCANCODE_LSHIFT};

static void stand_resolve(shell *s) {
    for (int i = 0; i < MX_CONFIG_STAND_KEYS; ++i) {
        const SDL_Scancode sc = SDL_GetScancodeFromName(s->live.stand_keys[i]);
        s->stand_sc[i] = sc != SDL_SCANCODE_UNKNOWN ? (int)sc : STAND_DEFAULT[i];
    }
}

static int stand_slot(const shell *s, int sc) {
    for (int i = 0; i < MX_CONFIG_STAND_KEYS; ++i) if (s->stand_sc[i] == sc) return i;
    return -1;
}

/* A joystick is assigned to the keyboard, whether or not the adapter is fitted. */
static int stand_in_assigned(const shell *s) {
    return s->live.sticks[0].device == MX_STICK_KEYBOARD || s->live.sticks[1].device == MX_STICK_KEYBOARD;
}

/* The stand-in exists while it is assigned to a joystick of an adapter that is fitted. */
static int stand_in_active(const shell *s) { return s->live.game_adapter && stand_in_assigned(s); }

/* The six keys are the joystick's (and no longer reach the guest) while the stand-in exists and F11 has them there. */
static int keys_are_stick(const shell *s) { return stand_in_active(s) && s->keys_to_stick; }

/* F11: the stand-in's keys go to the joystick or back to the computer. A key held at the switch is let go on the side it was
 * pressed (the stick centres, or the guest gets the key's release), so none stays down. With no stand-in it says what is needed. */
static void stand_toggle(shell *s) {
    if (s->setup) return;
    if (!s->live.game_adapter) { say_id(s, MX_T_SAY_STICK_NO_ADAPTER); return; }
    if (!stand_in_assigned(s)) { say_id(s, MX_T_SAY_STICK_NONE); return; }
    s->keys_to_stick = !s->keys_to_stick;
    if (s->keys_to_stick) {
        for (int i = 0; i < MX_CONFIG_STAND_KEYS; ++i) guest_key(s, s->stand_sc[i], 0, 0);
    } else memset(s->stand, 0, sizeof s->stand);
    /* The message names the keys as they are assigned. */
    char keys[MX_CONFIG_KEY_NAME * 4 + 8];
    if (s->keys_to_stick) say_id(s, MX_T_SAY_STICK_KEYS, mx_setup_stand_directions(&s->live, 0, keys, sizeof keys), s->live.stand_keys[4], s->live.stand_keys[5]);
    else say_id(s, MX_T_SAY_STICK_OFF, mx_setup_stand_directions(&s->live, 1, keys, sizeof keys));
}

/* What each joystick of the adapter has: plugged in when a device feeds it, and its position and buttons. A
 * joystick a --joy step took is left alone. Before the machine runs on. */
static void feed_sticks(shell *s) {
    if (s->setup) return;
    for (unsigned i = 0; i < MX_CONFIG_STICKS; ++i) {
        if (s->scripted[i]) continue;
        const mx_stick_config *st = &s->live.sticks[i];
        int plugged = 0, x = 0, y = 0;
        unsigned buttons = 0;
        if (s->live.game_adapter && st->device == MX_STICK_KEYBOARD) {
            plugged = 1;
            if (s->stand[3] && !s->stand[2]) x = 32767; else if (s->stand[2] && !s->stand[3]) x = -32768;
            if (s->stand[1] && !s->stand[0]) y = 32767; else if (s->stand[0] && !s->stand[1]) y = -32768;
            buttons = (s->stand[4] ? 1u : 0u) | (s->stand[5] ? 2u : 0u);
        } else if (s->live.game_adapter && st->device == MX_STICK_CONTROLLER && s->pads_open && st->pad >= 1 && st->pad <= s->pads.count) {
            int raw_x, raw_y, dpad_x, dpad_y;
            plugged = 1;
            mx_pads_joystick(&s->pads, st->pad - 1, &raw_x, &raw_y, &dpad_x, &dpad_y, &buttons);
            mx_stick_map(st, raw_x, raw_y, &x, &y);
            if (dpad_x) x = dpad_x < 0 ? -32768 : 32767;
            if (dpad_y) y = dpad_y < 0 ? -32768 : 32767;
        }
        mx_machine_joystick_plug(s->m, i, plugged);
        if (plugged) mx_machine_joystick(s->m, i, x, y, buttons);
    }
}

/* The pointer as the light pen: told to the machine when it changes (and again to a new machine). */
static void feed_pointer(shell *s) {
    const int on = s->live.light_pen && (s->live.display == MX_DISPLAY_CGA80 || s->live.display == MX_DISPLAY_CGA40) && !s->setup && !s->paused &&
                   s->pen_inside && !s->captured;
    const int x = on ? s->pen_x : 0, y = on ? s->pen_y : 0;
    const unsigned b = on ? s->pen_b : 0;
    if (s->pen_told && s->pen_told_inside == on && s->pen_told_x == x && s->pen_told_y == y && s->pen_told_b == b) return;
    s->pen_told = 1;
    s->pen_told_inside = on;
    s->pen_told_x = x;
    s->pen_told_y = y;
    s->pen_told_b = b;
    mx_machine_pointer(s->m, x, y, on, b);
}

/* The host pointer at window position (wx, wy): over the picture or not. */
static void pointer_moved(shell *s, int wx, int wy) {
    int x = 0, y = 0;
    s->pen_inside = mx_video_to_screen(s->video, wx, wy, &x, &y) ? 1 : 0;
    s->pen_x = x;
    s->pen_y = y;
}

/* --- the serial mouse ---------------------------------------------------------------------- */

/* The host mouse is the serial mouse's (captured) or the desktop's again. A captured mouse is hidden and held in
 * the window (relative mode; a hidden window leaves the host's pointer alone). `announce` says so in the status line. */
static void mouse_capture(shell *s, int on, int announce) {
    if ((on != 0) == (s->captured != 0)) return;
    s->captured = on != 0;
    s->mouse_dx = s->mouse_dy = 0;
    s->mouse_buttons = 0;
    if (!s->hidden) SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
    if (announce) say_id(s, on ? MX_T_SAY_MOUSE_CAPTURED : MX_T_SAY_MOUSE_RELEASED);
}

/* What the machine hears of the host mouse: its motion since the last time and its buttons while it is captured. */
static void feed_mouse(shell *s) {
    if (s->captured && !mx_config_mouse_attached(&s->live)) mouse_capture(s, 0, 0);      /* the port is gone */
    int dx = 0, dy = 0;
    if (s->captured && !s->setup && !s->paused) {
        dx = s->mouse_dx;
        dy = s->mouse_dy;
    }
    s->mouse_dx = s->mouse_dy = 0;
    const unsigned buttons = s->captured ? s->mouse_buttons : 0;
    if (!dx && !dy && s->mouse_told && buttons == s->mouse_told_buttons) return;
    s->mouse_told = 1;
    s->mouse_told_buttons = buttons;
    mx_machine_mouse(s->m, dx, dy, buttons);
}

/* A left click at window position (wx, wy): it captures the mouse when a serial mouse is there to take it and
 * the click is on the picture. 1 when it did. */
static int mouse_click_capture(shell *s, int wx, int wy) {
    if (s->captured || s->setup || s->paused || !mx_config_mouse_attached(&s->live)) return 0;
    pointer_moved(s, wx, wy);
    if (!s->pen_inside) return 0;
    s->pen_b = 0;
    mouse_capture(s, 1, 1);
    return 1;
}

/* --- the picture ------------------------------------------------------------------------ */

/* The monitor in use: the configured one, but the monochrome coupler's is the monochrome monitor in green
 * phosphor whatever video.monitor says (the setting waits for a CGA again); sharp, as it is no composite monitor. */
static uint32_t monitor_of(const shell *s) { return s->live.display == MX_DISPLAY_MDA ? MX_MONITOR_MDA : s->live.monitor; }

/* The configuration's look through the presenter; a phosphor without the
 * shader is drawn into the frame here instead. */
static void apply_look(shell *s) {
    static const int filters[] = {MX_FILTER_NEAREST, MX_FILTER_LINEAR, MX_FILTER_SHARP};
    static const int phosphors[] = {MX_VIDEO_PHOSPHOR_COLOR, MX_VIDEO_PHOSPHOR_COLOR, MX_VIDEO_PHOSPHOR_GREEN, MX_VIDEO_PHOSPHOR_GREEN};
    mx_video_set_filter(s->video, filters[s->live.filter]);
    /* integer: the largest whole multiple that fits the window. */
    mx_video_set_integer_scale(s->video, s->live.scale == MX_CONFIG_SCALE_INTEGER ? 1 << 16 : 0);
    mx_video_set_aspect(s->video, s->live.aspect == MX_CONFIG_ASPECT_SQUARE ? MX_ASPECT_SQUARE : MX_ASPECT_ORIGINAL);
    /* A hidden window never reaches a display, and an unthrottled machine would be held to its refresh: nothing to wait for. */
    mx_video_set_vsync(s->video, s->live.vsync && !s->hidden && s->live.speed != MX_SPEED_UNTHROTTLED);
    /* The monochrome coupler in the plain look is tinted here, not by the shader: its screenshot is then the frame itself
     * (720x350, the green levels as the window shows them); the CRT look has the shader's green phosphor as the CGA's
     * monochrome monitor has. */
    const int soft = s->live.display == MX_DISPLAY_MDA && s->live.look != MX_LOOK_CRT;
    s->tint = soft;
    if (!mx_video_set_look(s->video, s->live.look == MX_LOOK_CRT ? MX_VIDEO_LOOK_CRT : MX_VIDEO_LOOK_PLAIN,
                           soft ? MX_VIDEO_PHOSPHOR_COLOR : phosphors[monitor_of(s)])) {
        fputs("video.look needs OpenGL, which is unavailable: the picture is shown plain\n", stderr);
        mx_video_set_look(s->video, MX_VIDEO_LOOK_PLAIN, MX_VIDEO_PHOSPHOR_COLOR);
        s->tint = monitor_of(s) >= MX_MONITOR_MONO_GREEN;
    }
}

/* Gray levels in a phosphor's colour (only without the shader). */
static void tint(shell *s, size_t n) {
    const uint32_t ink = 0x33FF33;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t y = s->pixels[i] & 255u;
        s->pixels[i] = 0xFF000000u | ((ink >> 16 & 255u) * y / 255u) << 16 | ((ink >> 8 & 255u) * y / 255u) << 8 | (ink & 255u) * y / 255u;
    }
}

/* The status cells over the bottom row of a 640x200 frame (the machine's
 * colour indices), or of a 720x350 frame of the monochrome coupler (`mono`: its
 * 9x14 cells, the same glyphs in the card's 8x14 font: the floppy lights as lit cells with
 * dark letters, the fixed disks' as intense letters on dark): the message, or PAUSED, from the
 * left, and at the right a light for each floppy drive whose motor runs, then one for each fixed disk
 * at work (white on red, its DOS letter). Only the cells in use are drawn, so
 * the guest's screen shows round them. 1 when anything is. */
static int status_cells(shell *s, uint8_t *pix, int mono) {
    const int paused = s->paused;
    const int message = paused || (s->osd.text[0] && (int32_t)(s->osd.until_ms - now_ms(s)) > 0);
    static const char *const LETTERS[6] = {"A", "B", "C", "D", "C", "D"};
    /* The lights are not original: with video.drive_lights off there are none (drives = hard = 0) and the message has the row. */
    unsigned drives = s->live.drive_lights ? mx_machine_drives(s->m, MX_MEDIA_FLOPPY) : 0u;
    unsigned hard = s->live.drive_lights ? mx_machine_drives(s->m, MX_MEDIA_HARD_DISK) : 0u;
    if (drives > 4) drives = 4;
    if (hard > 2) hard = 2;
    const int first = MX_TUI_COLS - (int)(drives + hard);   /* the lights are the last cells: one for each drive */
    const unsigned lights = (mx_machine_drive_activity(s->m, MX_MEDIA_FLOPPY) & ((1u << drives) - 1u)) |
                            (mx_machine_drive_activity(s->m, MX_MEDIA_HARD_DISK) & ((1u << hard) - 1u)) << drives;
    if (!message && !lights) return 0;
    if (!s->status) {
        s->status = malloc(sizeof *s->status);
        if (!s->status) return 0;
        mx_tui_init(s->status, s->have_font ? s->font : NULL);
    }
    mx_tui *t = s->status;
    mx_tui_clear(t, STATUS_ATTR);
    char text[sizeof s->osd.text + 40];
    int used = 0;
    if (message) {
        snprintf(text, sizeof text, " %s ", paused ? MX_TXT(SAY_PAUSED) : s->osd.text);
        used = (int)strlen(text);
        if (used > first - 2) used = first - 2;
        mx_tui_put_fit(t, STATUS_ROW, 0, used, STATUS_ATTR, text, 0);
    }
    for (unsigned d = 0; d < drives + hard; ++d)
        if (lights >> d & 1u)
            mx_tui_put(t, STATUS_ROW, first + (int)d, d < drives ? STATUS_ATTR : STATUS_HARD_ATTR, LETTERS[d < drives ? d : 4u + d - drives]);
    mx_tui_render(t, 0);
    for (int col = 0; col < MX_TUI_COLS; ++col) {
        const int mine = col < used || (col >= first && (lights >> (col - first) & 1u));
        if (!mine) continue;
        if (!mono) {
            for (int y = 0; y < 8; ++y)
                memcpy(pix + (size_t)(STATUS_ROW * 8 + y) * MX_TUI_WIDTH + (size_t)col * 8, t->pixels + (size_t)(STATUS_ROW * 8 + y) * MX_TUI_WIDTH + (size_t)col * 8, 8);
            continue;
        }
        /* A message or floppy cell is lit with dark letters; a fixed disk's (white on red) is its letter alone, intense. */
        const int hard_cell = col >= first + (int)drives;
        const unsigned paper = (unsigned)((hard_cell ? STATUS_HARD_ATTR : STATUS_ATTR) >> 4);
        for (unsigned line = 0; line < MX_MDA_CELL_H; ++line) {
            const int row = mx_mda_font_row(' ', line);          /* a letter's rows: the text map */
            uint8_t *d = pix + (size_t)(STATUS_ROW * MX_MDA_CELL_H + line) * MX_MDA_FRAME_W + (size_t)col * MX_MDA_CELL_W;
            const uint8_t *src = row < 0 ? NULL : t->pixels + (size_t)(STATUS_ROW * 8 + row) * MX_TUI_WIDTH + (size_t)col * 8;
            for (unsigned x = 0; x < 8; ++x) {
                const int background = !src || src[x] == paper;
                d[x] = (uint8_t)(hard_cell ? (background ? MX_MDA_DARK : MX_MDA_INTENSE) : (background ? MX_MDA_NORMAL : MX_MDA_DARK));
            }
            d[8] = hard_cell ? MX_MDA_DARK : MX_MDA_NORMAL;
        }
    }
    return 1;
}

/* The picture: the setup screen's while it is open, else the machine's with
 * its status cells, converted for the monitor into s->pixels. `cells` 0
 * leaves the cells out (the screenshot of a command line). */
static int convert(shell *s, int cells) {
    mx_frame f;
    memset(&f, 0, sizeof f);
    if (s->setup) mx_setup_frame(s->setup, !((s->shown >> 3) & 1u), &f);
    else if (!mx_machine_frame(s->m, &f) || !f.pixels) {
        f.width = s->width ? s->width : 640;
        f.height = s->height ? s->height : 200;
        f.aspect_w = 4;
        f.aspect_h = 3;
        f.pixels = NULL;
    }
    ++s->shown;
    const size_t n = (size_t)f.width * f.height;
    if (n > s->pixels_size) {
        uint32_t *p = realloc(s->pixels, n * sizeof *p);
        if (!p) { snprintf(s->error, sizeof s->error, "Out of memory"); return 0; }
        s->pixels = p;
        s->pixels_size = n;
    }
    const int mono = f.format == MX_FRAME_MONO && f.width == MX_MDA_FRAME_W && f.height == MX_MDA_FRAME_H;
    if (cells && !s->setup && f.pitch == f.width && (mono || (f.width == MX_TUI_WIDTH && f.height == MX_TUI_HEIGHT))) {
        if (n > s->copy_size) {
            uint8_t *c = realloc(s->copy, n);
            if (!c) { snprintf(s->error, sizeof s->error, "Out of memory"); return 0; }
            s->copy = c;
            s->copy_size = n;
        }
        if (f.pixels) memcpy(s->copy, f.pixels, n); else memset(s->copy, 0, n);
        if (status_cells(s, s->copy, mono)) f.pixels = s->copy;
    }
    if (f.pixels) mx_frame_argb(&f, monitor_of(s), s->pixels);
    else for (size_t i = 0; i < n; ++i) s->pixels[i] = 0xFF000000u;
    if (s->tint) tint(s, n);
    s->width = f.width;
    s->height = f.height;
    mx_video_set_geometry(s->video, (int)f.width, (int)f.height, (int)f.aspect_w, (int)f.aspect_h);
    return 1;
}

/* The screenshot as the window shows it: the frame itself when no look applies. */
static int shot_raw(const shell *s) {
    const uint32_t monitor = monitor_of(s);
    return s->live.look == MX_LOOK_PLAIN && (monitor == MX_MONITOR_RGBI || monitor == MX_MONITOR_RGB || s->tint);
}

/* The Print Screen key: the next picture shown goes to the first free mezinxt-NNNN.png. */
static void shot_key(shell *s) {
    char name[64];
    for (unsigned n = 1; n < 10000; ++n) {
        snprintf(name, sizeof name, "mezinxt-%04u.png", n);
        if (!mx_host_file_exists(name)) break;
    }
    mx_video_request_shot(s->video, name, shot_raw(s) != 0);
    s->shot_wait = 1;
}

/* After a picture was shown: the screenshot a key asked for is on its way to the file. */
static void shot_done(shell *s) {
    char saved[1024];
    if (!s->shot_wait || !mx_video_take_shot_result(s->video, saved, sizeof saved)) return;
    s->shot_wait = 0;
    if (saved[0]) say_id(s, MX_T_SAY_SCREENSHOT, saved);
    else say_id(s, MX_T_SAY_SCREENSHOT_FAILED);
}

/* --- sound ------------------------------------------------------------------------------ */

/* The sound device opened or closed to match the configuration (a muted or
 * headless run holds none); the volume applies at every pump. */
static void audio_apply(shell *s) {
    const int want = !s->hidden && !s->options->wav && s->live.sound && mx_machine_audio_rate(s->m);
    if (want && !s->audio_open) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) || !mx_host_audio_open(&s->audio)) {
            fprintf(stderr, "Sound output unavailable: %s\n", SDL_GetError());
            memset(&s->audio, 0, sizeof s->audio);
        } else s->audio_open = 1;
    } else if (!want && s->audio_open) {
        mx_host_audio_close(&s->audio);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        s->audio_open = 0;
    }
}

/* The machine's drive sound follows the configuration (sound.drives and sound.drives_volume). A run that is muted, or
 * headless without a WAV file to record, has nobody to hear it: the machine is told to make none. */
static void drive_sound_apply(shell *s) {
    const int heard = (s->live.sound && !s->hidden) || s->options->wav != NULL;
    mx_machine_set_drive_sound(s->m, s->live.drive_sound && heard, (unsigned)s->live.drive_volume);
}

/* --- the setup screen ------------------------------------------------------------------- */

/* A drive's disk changed by the user (setup.h): the old image is ejected
 * (written back), the one at `path` (as the reader of the configuration gives
 * it back) read and put in with write_protect as its tab, "" leaves the drive empty. The open images are options->disks'. When
 * the new one cannot go in, the old one stays. */
static int hard_change(shell *s, unsigned n, const char *path, char *error, size_t capacity);

static int media_change(void *ctx, unsigned drive, const char *path, int write_protect, char *error, size_t capacity) {
    shell *s = ctx;
    mx_disk **disks = s->options->disks;
    if (drive >= MX_CONFIG_DRIVES && drive < MX_DISK_SLOTS) return hard_change(s, drive - MX_CONFIG_DRIVES, path, error, capacity);
    if (!disks || drive >= MX_CONFIG_DRIVES) {
        mx_machine_eject(s->m, MX_MEDIA_FLOPPY, drive);
        return 1;
    }
    mx_disk *old = disks[drive], *d = NULL;
    char ignored[128];
    for (unsigned k = 0; path[0] && k < MX_DISK_SLOTS; ++k)
        if (k != drive && disks[k] && mx_disk_is_file(disks[k], path)) {
            if (k < MX_CONFIG_DRIVES) mx_text_format(error, capacity, MX_T_SAY_IMAGE_IN_DRIVE, (char)('A' + k));
            else mx_text_format(error, capacity, MX_T_SAY_IMAGE_ON_HARD, k - MX_CONFIG_DRIVES);
            return 0;
        }
    if (old) mx_machine_eject(s->m, MX_MEDIA_FLOPPY, drive);
    if (path[0]) {
        d = mx_disk_open(path, write_protect, error, capacity);
        if (d && !mx_machine_insert(s->m, MX_MEDIA_FLOPPY, drive, d, error, capacity)) {
            mx_disk_close(d);
            d = NULL;
        }
        if (!d) {
            if (old) mx_machine_insert(s->m, MX_MEDIA_FLOPPY, drive, old, ignored, sizeof ignored);
            return 0;
        }
        if (error[0]) say(s, "%s", error);       /* a warning */
    }
    mx_disk_close(old);
    disks[drive] = d;
    return 1;
}

/* A fixed disk's image changed by the user (setup.h; drive MX_DISK_SLOT_HARD(n)): opened as a fixed disk and put
 * on the coupler when the machine has one, else kept for the machine the setup screen's restart builds (the BIOS
 * counts the fixed disks at power-on, so a change of them restarts the machine on leaving). "" detaches it. */
static int hard_change(shell *s, unsigned n, const char *path, char *error, size_t capacity) {
    mx_disk **disks = s->options->disks;
    const unsigned slot = MX_DISK_SLOT_HARD(n);
    if (!disks) return 1;
    for (unsigned k = 0; path[0] && k < MX_DISK_SLOTS; ++k)
        if (k != slot && disks[k] && mx_disk_is_file(disks[k], path)) {
            if (k < MX_CONFIG_DRIVES) mx_text_format(error, capacity, MX_T_SAY_IMAGE_IN_DRIVE, (char)('A' + k));
            else mx_text_format(error, capacity, MX_T_SAY_IMAGE_ON_HARD, k - MX_CONFIG_DRIVES);
            return 0;
        }
    mx_disk *d = NULL;
    if (path[0] && !(d = mx_disk_open_hard(path, error, capacity))) return 0;
    const int fitted = n < mx_machine_drives(s->m, MX_MEDIA_HARD_DISK);
    if (fitted && disks[slot]) mx_machine_eject(s->m, MX_MEDIA_HARD_DISK, n);
    mx_disk_close(disks[slot]);
    disks[slot] = d;
    char ignored[128];
    if (d && fitted) mx_machine_insert(s->m, MX_MEDIA_HARD_DISK, n, d, ignored, sizeof ignored);
    return 1;
}

/* A file dropped on the window goes into drive A, or drive B with Shift
 * held (the second of several dropped together goes into B too). A file that
 * is not a diskette image is refused with the reason, and the disk in the
 * drive stays. The setup screen (which shows the drives itself) leaves
 * drops alone. */
static void drop_file(shell *s, const char *path, int shift) {
    if (s->setup || !path || !*path) return;
    const unsigned drive = shift || s->drop_count++ ? 1u : 0u;
    const char *name = path;
    for (const char *p = path; *p; ++p) if (*p == '/' || *p == '\\') name = p + 1;
    if (drive >= mx_machine_drives(s->m, MX_MEDIA_FLOPPY) || drive >= MX_CONFIG_DRIVES) {
        say_id(s, MX_T_SAY_NO_DRIVE, (char)('A' + drive));
        return;
    }
    char error[256] = "";
    if (!media_change(s, drive, path, 0, error, sizeof error)) {
        /* The reasons name the file by its whole path: the name is enough here. */
        const size_t n = strlen(path);
        if (!error[0]) say_id(s, MX_T_SAY_NOT_A_DISK, name);
        else if (!strncmp(error, path, n)) say(s, "%s%s", name, error + n);
        else say(s, "%s: %s", name, error);
        return;
    }
    snprintf(s->live.drives[drive].image, sizeof s->live.drives[drive].image, "%s", path);
    s->live.drives[drive].write_protect = 0;
    mx_disk *d = mx_machine_media(s->m, MX_MEDIA_FLOPPY, drive);
    if (!d) say_id(s, MX_T_SAY_DRIVE, (char)('A' + drive), name);
    else say_id(s, mx_disk_write_protected(d) ? MX_T_SAY_DRIVE_KIB_PROTECTED : MX_T_SAY_DRIVE_KIB, (char)('A' + drive), name, mx_disk_kib(d));
}

static void setup_controllers(shell *s) {
    if (!s->setup) return;
    const char *names[MX_CONFIG_PADS];
    for (unsigned i = 0; i < s->pads.count; ++i) names[i] = mx_pads_name(&s->pads, i);
    mx_setup_set_controllers(s->setup, names, s->pads_open ? s->pads.count : 0);
}

static int setup_open(shell *s, int page) {
    if (s->setup) return 1;
    mx_setup_env env;
    memset(&env, 0, sizeof env);
    env.ctx = s;
    env.font = s->have_font ? s->font : NULL;
    env.roms = s->options->roms;
    env.version = MX_VERSION;
    env.drives = mx_machine_drives(s->m, MX_MEDIA_FLOPPY);
    env.fullscreen = !s->hidden && mx_video_fullscreen(s->video);
    env.no_crt = !mx_video_crt_available(s->video);
    env.media = media_change;
    s->setup = mx_setup_create(&s->live, &env);
    if (!s->setup) { snprintf(s->error, sizeof s->error, "Out of memory"); return 0; }
    if (page >= 0) mx_setup_open_page(s->setup, page);
    setup_controllers(s);
    release_all(s);
    mouse_capture(s, 0, 0);             /* the setup screen has the mouse's keys to itself */
    if (!s->hidden) SDL_StartTextInput();
    return 1;
}

/* The picture options the setup screen changed, at once. */
static void sync_video(shell *s) {
    const mx_config *e = mx_setup_config(s->setup);
    if (!(mx_setup_diff(&s->live, e) & MX_CHANGE_VIDEO)) return;
    s->live.look = e->look;
    s->live.monitor = e->monitor;
    s->live.aspect = e->aspect;
    s->live.scale = e->scale;
    s->live.filter = e->filter;
    s->live.vsync = e->vsync;
    s->live.snow = e->snow;
    s->live.drive_lights = e->drive_lights;
    s->live.fullscreen = e->fullscreen;
    mx_machine_set_snow(s->m, e->snow);
    apply_look(s);
    if (!s->hidden && (mx_video_fullscreen(s->video) != 0) != (e->fullscreen != 0)) mx_video_set_fullscreen(s->video, e->fullscreen != 0);
}

static void say_speed(shell *s) {
    static const int messages[] = {MX_T_SAY_SPEED_477, MX_T_SAY_SPEED_716, MX_T_SAY_SPEED_954, MX_T_SAY_SPEED_FREE};
    if (s->live.speed < sizeof messages / sizeof *messages) say_id(s, messages[s->live.speed]);
}

/* The machine again from the live configuration (a new BIOS, processor,
 * memory, speed or display coupler). */
static int rebuild_machine(shell *s) {
    char error[256] = "";
    static mx_rom_index none;
    const mx_rom_index *roms = s->options->roms ? s->options->roms : &none;
    mx_machine *m = s->options->stub ? mx_machine_create_stub(&s->live, roms, error, sizeof error)
                                     : mx_machine_create(&s->live, roms, error, sizeof error);
    if (!m) {
        say(s, "%s", error[0] ? error : MX_TXT(SAY_CANNOT_REBUILD));
        return 0;
    }
    if (s->options->trace) mx_machine_set_trace(m, s->options->trace, s->options->trace_ctx);
    mx_machine_set_serial(s->m, NULL);
    mx_machine_set_trace(s->m, NULL, NULL);
    mx_machine_destroy(s->m);
    s->m = m;
    *s->machine_out = m;
    mx_machine_set_serial(m, &s->serial_host);
    drive_sound_apply(s);
    mx_host_serial_resend(&s->serial);
    s->pen_told = 0;                    /* the new machine learns where the pen is */
    s->mouse_told = 0;
    /* The images were let go of with the old machine: into the new one. */
    if (s->options->disks && !mx_disk_insert_all(m, s->options->disks, error, sizeof error)) say(s, "%s", error);
    return 1;
}

static void reopen_ports(shell *s) {
    char error[256] = "";
    mx_machine_set_serial(s->m, NULL);
    mx_host_serial_close(&s->serial);
    if (!mx_host_serial_open(&s->serial, &s->live, error, sizeof error))
        say(s, "%s", error[0] ? error : MX_TXT(SAY_CANNOT_PORTS));
    s->serial_host = mx_host_serial_callbacks(&s->serial);
    mx_machine_set_serial(s->m, &s->serial_host);
}

/* The setup screen is left: the changes it made are applied, the machine runs again. */
static void setup_leave(shell *s) {
    const mx_config edit = *mx_setup_config(s->setup);
    const int reset = mx_setup_reset(s->setup);
    const mx_config before = s->live;
    const int had_stand_in = stand_in_active(s);
    const unsigned changes = mx_setup_diff(&before, &edit);
    mx_setup_destroy(s->setup);
    s->setup = NULL;
    if (!s->hidden) SDL_StopTextInput();
    s->live = edit;
    stand_resolve(s);
    if (!had_stand_in) s->keys_to_stick = 1;       /* a stand-in new to the machine starts with the keys on the joystick */
    s->resync = 1;
    if (changes & MX_CHANGE_VIDEO) {
        mx_machine_set_snow(s->m, s->live.snow);
        apply_look(s);
    }
    if (changes & MX_CHANGE_PORTS) reopen_ports(s);
    if (changes & MX_CHANGE_RESTART) {
        if (!rebuild_machine(s)) s->live = before;
        else if (s->live.speed != before.speed) say_speed(s);
        apply_look(s);          /* an unthrottled machine is not held to the display's refresh */
    } else if (reset) {
        mx_machine_reset(s->m, reset == MX_SETUP_RESET_POWER);
        say_id(s, reset == MX_SETUP_RESET_POWER ? MX_T_SAY_POWER_CYCLE : MX_T_SAY_RESET);
    }
    if (changes & MX_CHANGE_SOUND) {
        audio_apply(s);
        drive_sound_apply(s);
    }
}

/* The language the setup screen chose, at once (the messages after it are in it). */
static void sync_language(shell *s) {
    const mx_config *e = mx_setup_config(s->setup);
    if (!(mx_setup_diff(&s->live, e) & MX_CHANGE_LANGUAGE)) return;
    s->live.language = e->language;
    mx_text_set_language(s->live.language);
}

/* After the setup screen took input. */
static void setup_after(shell *s) {
    if (!s->setup) return;
    if (mx_setup_done(s->setup)) {
        if (mx_setup_exit(s->setup)) s->quit = 1;       /* saved as leaving does; the machine is not rebuilt, the program ends */
        else setup_leave(s);
    } else {
        sync_language(s);
        sync_video(s);
    }
}

static void setup_event(shell *s, mx_tui_event e) {
    if (!s->setup) return;
    mx_setup_event(s->setup, e);
    setup_after(s);
}

/* Exit MezinXT at once; with the setup screen open what was changed in it is saved first. */
static void exit_now(shell *s) {
    if (!s->setup) { s->quit = 1; return; }
    mx_setup_request_exit(s->setup);
    setup_after(s);
}

static void setup_toggle(shell *s) {
    if (!s->setup) { setup_open(s, -1); return; }
    mx_setup_request_close(s->setup);
    setup_after(s);
}

/* A host key as an event of the setup screen. */
static int ui_key(const SDL_Keysym *k, mx_tui_event *e) {
    e->ch = 0;
    switch (k->sym) {
    case SDLK_UP: e->key = MX_KEY_UP; return 1;
    case SDLK_DOWN: e->key = MX_KEY_DOWN; return 1;
    case SDLK_LEFT: e->key = MX_KEY_LEFT; return 1;
    case SDLK_RIGHT: e->key = MX_KEY_RIGHT; return 1;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: e->key = MX_KEY_ENTER; return 1;
    case SDLK_ESCAPE: e->key = MX_KEY_ESCAPE; return 1;
    case SDLK_PAGEUP: e->key = MX_KEY_PAGE_UP; return 1;
    case SDLK_PAGEDOWN: e->key = MX_KEY_PAGE_DOWN; return 1;
    case SDLK_HOME: e->key = MX_KEY_HOME; return 1;
    case SDLK_END: e->key = MX_KEY_END; return 1;
    case SDLK_TAB: e->key = k->mod & KMOD_SHIFT ? MX_KEY_BACKTAB : MX_KEY_TAB; return 1;
    case SDLK_BACKSPACE: e->key = MX_KEY_BACKSPACE; return 1;
    case SDLK_DELETE: e->key = MX_KEY_DELETE; return 1;
    default: return 0;
    }
}

/* A key for the machine or the setup screen: the XT key code, bit 7 a release. */
static void deliver_key(shell *s, uint8_t code) {
    if (s->setup) {
        mx_setup_xt_key(s->setup, code);
        setup_after(s);
    } else queue_key(s, code & 0x7Fu, !(code & 0x80u));
}

/* --- host keys and events ---------------------------------------------------------------- */

/* Ctrl+F12 switches the machine off and on; Alt+F12 is its reset line. */
static void reset_key(shell *s, int power) {
    if (s->setup) return;
    release_all(s);
    mx_machine_reset(s->m, power);
    s->resync = 1;
    say_id(s, power ? MX_T_SAY_POWER_CYCLE : MX_T_SAY_RESET);
}

static void toggle_pause(shell *s) {
    if (s->setup) return;
    s->paused = !s->paused;
    if (s->paused) {
        release_all(s);
        mouse_capture(s, 0, 0);
    }
    s->resync = 1;
}

/* Full screen, on and off (a hidden window has none). */
static void fullscreen_toggle(shell *s) {
    if (!s->hidden) mx_video_set_fullscreen(s->video, !mx_video_fullscreen(s->video));
}

/* The keys that are the window's, not the guest's. Only a press acts. */
static int window_key(shell *s, int sc, unsigned mod) {
    switch (sc) {
    case SDL_SCANCODE_F11:
        stand_toggle(s);
        return 1;
    case SDL_SCANCODE_F12:
        if (mod & KMOD_CTRL) reset_key(s, 1);
        else if (mod & KMOD_ALT) reset_key(s, 0);
        else if (mod & KMOD_SHIFT) exit_now(s);
        else setup_toggle(s);
        return 1;
    case SDL_SCANCODE_PAUSE:
        if (mod & KMOD_CTRL) {          /* Ctrl-Break: the Scroll Lock key with Ctrl held, pressed whole */
            queue_key(s, 0x46, 1);
            queue_key(s, 0x46, 0);
        } else toggle_pause(s);
        return 1;
    case SDL_SCANCODE_PRINTSCREEN:
        shot_key(s);
        return 1;
    default: return 0;
    }
}

/* An event; 0 ends the run. */
static int event(shell *s, const SDL_Event *e) {
    if (s->pads_open && (e->type == SDL_CONTROLLERDEVICEADDED || e->type == SDL_CONTROLLERDEVICEREMOVED ||
                         e->type == SDL_CONTROLLERBUTTONDOWN)) {
        mx_tui_event ui;
        switch (mx_pads_event(&s->pads, e, &ui)) {
        case MX_PADS_CHANGED: setup_controllers(s); break;
        case MX_PADS_SETUP: setup_toggle(s); break;
        case MX_PADS_EVENT: setup_event(s, ui); break;
        default: break;
        }
        return 1;
    }
    switch (e->type) {
    case SDL_QUIT:
        /* Closing the window leaves the setup screen too: what was changed in it is kept. */
        if (s->setup) {
            char message[MX_CONFIG_PATH + 80];
            if (!mx_setup_save(s->setup, message, sizeof message)) fprintf(stderr, "%s\n", message);
        }
        return 0;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        const int down = e->type == SDL_KEYDOWN;
        const SDL_Scancode sc = e->key.keysym.scancode;
        const unsigned mod = e->key.keysym.mod;
        /* The setup screen waiting for a key to assign (the keyboard joystick's keys) takes every key, the window's included; Print
         * Screen gives Windows no press, only a release. */
        if (s->setup && mx_setup_wants_host_key(s->setup)) {
            if ((down && !e->key.repeat) || (!down && sc == SDL_SCANCODE_PRINTSCREEN)) {
                const char *name = SDL_GetScancodeName(sc);
                mx_setup_host_key(s->setup, name ? name : "", ((mod & KMOD_CTRL) ? MX_HOSTKEY_CTRL : 0u) | ((mod & KMOD_ALT) ? MX_HOSTKEY_ALT : 0u));
                setup_after(s);
            }
            return 1;
        }
        /* Alt+Enter is full screen; its release stays here as well. */
        static int alt_enter;
        if (sc == SDL_SCANCODE_RETURN && (down ? (mod & KMOD_ALT) != 0 : alt_enter)) {
            if (down && !e->key.repeat) fullscreen_toggle(s);
            alt_enter = down;
            return 1;
        }
        /* Ctrl+F10 lets the captured mouse go (and is nobody's key: not the guest's). */
        if (sc == SDL_SCANCODE_F10 && (mod & KMOD_CTRL) && s->captured) {
            if (down && !e->key.repeat) mouse_capture(s, 0, 1);
            return 1;
        }
        /* A stand-in key held for the joystick is the joystick's until it is let go; held for the computer, it is the computer's. */
        const int slot = stand_slot(s, sc), stick_key = slot >= 0 && keys_are_stick(s);
        if (slot >= 0 && !e->key.repeat) s->stand[slot] = (uint8_t)(down && stick_key);
        if (s->setup && sc != SDL_SCANCODE_F11 && sc != SDL_SCANCODE_F12 && sc != SDL_SCANCODE_PRINTSCREEN) {
            /* the setup screen takes the keyboard, its repeats too */
            mx_tui_event ui;
            if (down && ui_key(&e->key.keysym, &ui)) setup_event(s, ui);
            return 1;
        }
        const int mine = sc == SDL_SCANCODE_F11 || sc == SDL_SCANCODE_F12 || sc == SDL_SCANCODE_PAUSE || sc == SDL_SCANCODE_PRINTSCREEN;
        if (e->key.repeat) return 1;    /* the keyboard repeats by itself */
        if (mine) {
            /* Only a press acts; Windows sends no press for Print Screen, only its release. */
            if (down != (sc == SDL_SCANCODE_PRINTSCREEN)) window_key(s, sc, mod);
            return 1;
        }
        if (down && stick_key) return 1;        /* the stick's key: not the guest's */
        guest_key(s, sc, down, mod);
        return 1;
    }
    case SDL_MOUSEMOTION:
        if (s->captured) {
            s->mouse_dx += e->motion.xrel;
            s->mouse_dy += e->motion.yrel;
            return 1;
        }
        pointer_moved(s, e->motion.x, e->motion.y);
        return 1;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (s->captured) {          /* the serial mouse's buttons */
            const unsigned bit = e->button.button == SDL_BUTTON_LEFT ? 1u : e->button.button == SDL_BUTTON_RIGHT ? 2u : 0u;
            if (e->type == SDL_MOUSEBUTTONDOWN) s->mouse_buttons |= bit;
            else s->mouse_buttons &= ~bit;
            return 1;
        }
        if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT && mouse_click_capture(s, e->button.x, e->button.y)) return 1;
        if (e->button.button == SDL_BUTTON_LEFT) {
            s->pen_b = e->type == SDL_MOUSEBUTTONDOWN ? 1u : 0u;
            pointer_moved(s, e->button.x, e->button.y);
        }
        return 1;
    case SDL_TEXTINPUT:
        if (s->setup) {
            for (const char *c = e->text.text; *c && s->setup; ++c) {
                mx_tui_event ui = {MX_KEY_CHAR, (unsigned char)*c};
                if (ui.ch >= 0x20 && ui.ch < 0x7F) setup_event(s, ui);
            }
        }
        return 1;
    case SDL_DROPBEGIN:
        s->drop_count = 0;
        return 1;
    case SDL_DROPFILE:
        drop_file(s, e->drop.file, s->script_shift || (SDL_GetModState() & KMOD_SHIFT) != 0);
        SDL_free(e->drop.file);
        return 1;
    case SDL_WINDOWEVENT:
        /* A key held when the window loses the keyboard is never released by the host. */
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST || e->window.event == SDL_WINDOWEVENT_MINIMIZED ||
            e->window.event == SDL_WINDOWEVENT_HIDDEN) {
            release_all(s);
            memset(s->stand, 0, sizeof s->stand);
            s->pen_b = 0;
            mouse_capture(s, 0, 0);
        }
        if (e->window.event == SDL_WINDOWEVENT_LEAVE || e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) s->pen_inside = 0;
        return 1;
    default: return 1;
    }
}

/* A step of the checks script goes through event() as the host's would. */
static int script_step(shell *s, const mx_desktop_script *step) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    if (step->kind == MX_SCRIPT_DROP) {
        e.type = SDL_DROPFILE;
        e.drop.file = SDL_strdup(step->path);
        s->script_shift = (step->mods & MX_SCRIPT_SHIFT) != 0;
        s->drop_count = 0;
        const int r = event(s, &e);
        s->script_shift = 0;
        return r;
    }
    if (step->kind == MX_SCRIPT_FOCUS_LOST) {
        e.type = SDL_WINDOWEVENT;
        e.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
        return event(s, &e);
    }
    if (step->kind == MX_SCRIPT_JOY) {
        s->scripted[step->stick] = 1;
        mx_machine_joystick_plug(s->m, step->stick, !step->off);
        if (!step->off) mx_machine_joystick(s->m, step->stick, step->x, step->y, step->buttons);
        return 1;
    }
    if (step->kind == MX_SCRIPT_MOUSE) {
        if (step->click) {
            /* a click on the picture, as the host's would be */
            s->pen_inside = 1;
            if (!s->captured && !s->setup && !s->paused && mx_config_mouse_attached(&s->live)) mouse_capture(s, 1, 1);
        } else if (s->captured) {
            s->mouse_dx += step->x;
            s->mouse_dy += step->y;
            s->mouse_buttons = step->buttons & 3u;
        }
        return 1;
    }
    if (step->kind == MX_SCRIPT_POINTER) {
        s->pen_inside = !step->off;
        s->pen_x = step->x;
        s->pen_y = step->y;
        s->pen_b = step->buttons & 1u;
        return 1;
    }
    e.type = SDL_KEYDOWN;
    e.key.keysym.scancode = (SDL_Scancode)step->scancode;
    e.key.keysym.mod = (Uint16)((step->mods & MX_SCRIPT_SHIFT ? KMOD_SHIFT : 0) | (step->mods & MX_SCRIPT_CTRL ? KMOD_CTRL : 0) |
                                (step->mods & MX_SCRIPT_ALT ? KMOD_ALT : 0));
    e.key.keysym.sym = SDL_GetKeyFromScancode((SDL_Scancode)step->scancode);
    int r = 1;
    if (!(step->mods & MX_SCRIPT_RELEASE)) r = event(s, &e);
    e.type = SDL_KEYUP;
    return r && ((step->mods & MX_SCRIPT_PRESS) || event(s, &e));
}

/* After each run of the machine: keys in, sound and ports out. `free_running`: the machine runs faster than the clock, so
 * its own sound is dropped; with drive sounds on, the drives' summary is played at the pace of the clock instead (the
 * machine's frames cover only a fraction of a second of crowded guest time, and the disk works exactly then). */
static int pump(shell *s, int free_running) {
    const int discard = free_running;
    while (s->read != s->write) {
        const uint8_t k = s->pending[s->read];
        if (!mx_machine_key(s->m, k & 0x7Fu, !(k & 0x80u))) break;
        s->read = (s->read + 1) & 255u;
    }
    if (s->options->wav) mx_wav_pump(&s->wav, s->m);         /* the recording is all of guest time, free running or not */
    else if (free_running && s->live.sound && s->live.drive_sound && s->live.drive_volume) {
        if (!mx_host_audio_pump_free(&s->audio, s->m, (unsigned)s->live.volume)) {
            snprintf(s->error, sizeof s->error, "Sound output: %s", SDL_GetError());
            return 0;
        }
    } else if (!mx_host_audio_pump(&s->audio, s->m, (unsigned)s->live.volume, discard || !s->live.sound)) {
        snprintf(s->error, sizeof s->error, "Sound output: %s", SDL_GetError());
        return 0;
    }
    mx_host_serial_pump(&s->serial, s->m);
    return 1;
}

static int run_status(shell *s, mx_run_status r) {
    if (r == MX_RUN_ERROR) { snprintf(s->error, sizeof s->error, "%s", mx_machine_error(s->m)); return 0; }
    return 1;
}

/* Game controller input to the setup screen: held directions repeat, the
 * sticks feed the calibration. */
static void pads_input(shell *s) {
    mx_tui_event ui;
    if (mx_pads_repeat(&s->pads, SDL_GetTicks(), &ui)) setup_event(s, ui);
    for (unsigned i = 0; s->setup && i < s->pads.count; ++i) {
        int x, y;
        mx_pads_axes(&s->pads, i, &x, &y);
        mx_setup_pad_axes(s->setup, i, x, y);
    }
}

int mx_desktop_run(mx_machine **machine, const mx_config *config, const mx_desktop_options *options, char *error, size_t capacity) {
    shell *s = calloc(1, sizeof *s);
    if (!s) { snprintf(error, capacity, "Out of memory"); return 1; }
    s->m = *machine;
    s->machine_out = machine;
    s->live = *config;
    s->options = options;
    s->hidden = options->hidden;
    s->det = options->hidden && !options->realtime;
    s->keys_to_stick = 1;
    stand_resolve(s);
    const mx_rom_file *chargen = options->roms ? mx_rom_index_find(options->roms, mx_romdb_find("chargen")) : NULL;
    if (chargen) { memcpy(s->font, chargen->data, sizeof s->font); s->have_font = 1; }
    mx_text_set_font(s->have_font ? s->font : NULL);
    mx_text_set_language(config->language);
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER)) {
        snprintf(error, capacity, "SDL: %s", SDL_GetError());
        free(s);
        return 1;
    }
    mx_frame f;
    memset(&f, 0, sizeof f);
    mx_machine_frame(s->m, &f);
    s->width = f.width ? f.width : 640;
    s->height = f.height ? f.height : 200;
    mx_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.window_scale = 2;
    settings.aspect = config->aspect == MX_CONFIG_ASPECT_SQUARE ? MX_ASPECT_SQUARE : MX_ASPECT_ORIGINAL;
    settings.fullscreen = config->fullscreen && !s->hidden;
    /* A hidden window never reaches a display: nothing to wait for. */
    settings.vsync = config->vsync && !s->hidden;
    s->video = mx_video_open(TITLE, &settings, (int)s->width, (int)s->height, f.aspect_w ? (int)f.aspect_w : 4,
                             f.aspect_h ? (int)f.aspect_h : 3, s->hidden != 0, error, capacity);
    if (!s->video) {
        SDL_Quit();
        free(s);
        return 1;
    }
#ifndef _WIN32                              /* on Windows the window has the program file's own icon */
    if (!s->hidden) mx_video_set_icon(s->video, mx_icon_pixels, MX_ICON_SIZE);
#endif
    printf("Presenter: %s\n", mx_video_backend(s->video));
    fflush(stdout);
    apply_look(s);
    int status = 0;
    if (options->wav && !mx_wav_open(&s->wav, options->wav)) { snprintf(s->error, sizeof s->error, "Cannot write %s", options->wav); status = 1; }
    /* Headless runs make no sound on the host: the samples are discarded. */
    if (!status) audio_apply(s);
    drive_sound_apply(s);
    if (!status && !mx_host_serial_open(&s->serial, config, s->error, sizeof s->error)) status = 1;
    if (!status) {
        s->serial_host = mx_host_serial_callbacks(&s->serial);
        mx_machine_set_serial(s->m, &s->serial_host);
    }
    /* Game controllers only in a real window: a headless run is the same on any host. */
    if (!status && !s->hidden) s->pads_open = mx_pads_open(&s->pads);
    if (!status && options->setup_page >= 0 && !setup_open(s, options->setup_page)) status = 1;
    if (!status && s->live.speed != MX_SPEED_4_77) say_speed(s);

    const uint64_t freq = SDL_GetPerformanceFrequency();
    mx_pace pace;
    mx_pace_init(&pace, freq, CATCH_UP_MS);
    mx_pace_reset(&pace, SDL_GetPerformanceCounter(), mx_machine_time(s->m));
    uint64_t frames = 0;
    unsigned key_next = 0, script_next = 0;
    int running = !status;
    while (running) {
        SDL_Event e;
        while (running && SDL_PollEvent(&e)) running = event(s, &e) && !s->quit;
        if (!running || s->quit) break;
        if (s->setup && s->pads_open) pads_input(s);
        if (s->quit) break;
        feed_sticks(s);
        feed_pointer(s);
        feed_mouse(s);
        const int paused = s->setup != NULL || s->paused;
        const uint64_t period = mx_machine_frame_period(s->m);
        const uint64_t now = s->det ? 0 : SDL_GetPerformanceCounter();
        if (s->resync) {
            mx_pace_reset(&pace, now, mx_machine_time(s->m));
            s->resync = 0;
        }
        if (s->det) {
            /* A frame period of guest time (the machine stands still while the setup screen is open or the machine is
             * paused), then the frame. */
            if (!paused && (!run_status(s, mx_machine_run(s->m, period)) || !pump(s, 0))) { status = 1; break; }
        } else {
            /* Unthrottled by choice, or for as long as a drive with fast access (drives.*.fast) works. */
            const int free_running = !paused && (s->live.speed == MX_SPEED_UNTHROTTLED ||
                                                 mx_config_fast_active(&s->live, mx_machine_drive_activity(s->m, MX_MEDIA_FLOPPY),
                                                                       mx_machine_drive_activity(s->m, MX_MEDIA_HARD_DISK)));
            /* Back to the clock: the time gained is kept, not waited out. */
            static int was_free;
            if (was_free && !free_running) mx_pace_reset(&pace, now, mx_machine_time(s->m));
            was_free = free_running;
            if (paused) mx_pace_hold(&pace, now, mx_machine_time(s->m));
            const mx_pace_step step = mx_pace_next(&pace, free_running, now, mx_machine_time(s->m), period);
            if (step.action == MX_PACE_RUN) {
                if (!run_status(s, mx_machine_run(s->m, step.ticks)) || !pump(s, free_running)) { status = 1; break; }
                continue;
            }
            if (step.action == MX_PACE_WAIT) {
                /* A sleep shorter than 100 us is not worth the call: the loop polls again. */
                const uint64_t us = step.ticks * 1000000u / freq;
                if (us >= 100u) mx_host_sleep_us((uint32_t)us);
                continue;
            }
        }
        /* Checks: keys and host events at given frames. */
        while (!s->quit && key_next < options->key_count && frames + 1 >= options->key_at[key_next]) deliver_key(s, options->key_code[key_next++]);
        while (running && !s->quit && script_next < options->script_count && frames + 1 >= options->script[script_next].at)
            running = script_step(s, &options->script[script_next++]);
        if (!running || s->quit) break;
        if (!convert(s, 1)) { status = 1; break; }
        mx_video_present(s->video, s->pixels, (int)s->width, (int)s->height);
        shot_done(s);
        if (options->frames && ++frames >= options->frames) running = 0;
        else if (!options->frames) ++frames;
    }
    if (status == 0 && options->screenshot) {
        /* The frame itself when no look applies; else the picture as shown. */
        if (!convert(s, 0)) status = 1;
        else {
            mx_video_request_shot(s->video, options->screenshot, shot_raw(s) != 0);
            mx_video_present(s->video, s->pixels, (int)s->width, (int)s->height);
            char saved[1024];
            if (!mx_video_take_shot_result(s->video, saved, sizeof saved) || !saved[0]) {
                snprintf(s->error, sizeof s->error, "Cannot save the screenshot %s", options->screenshot);
                status = 1;
            }
        }
    }
    if (options->wav && s->wav.file && !mx_wav_close(&s->wav) && !status) {
        snprintf(s->error, sizeof s->error, "Cannot write %s", options->wav);
        status = 1;
    }
    if (status) snprintf(error, capacity, "%s", s->error[0] ? s->error : "The run failed");
    mx_setup_destroy(s->setup);
    mx_machine_set_serial(s->m, NULL);
    mx_host_serial_close(&s->serial);
    mx_host_audio_close(&s->audio);
    if (s->pads_open) mx_pads_close(&s->pads);
    free(s->pixels);
    free(s->copy);
    free(s->status);
    mx_video_close(s->video);
    SDL_Quit();
    free(s);
    mx_text_set_language(MX_LANG_EN);       /* what the program says after the window is gone is a diagnostic: English */
    return status;
}

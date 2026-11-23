/* The setup screen (setup.h). MezinXT's own code. */
#include "setup.h"
#include "../core/yamledit.h"
#include "../core/text.h"
#include "../host/host.h"
#include "../hw/hdc.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROWS 64
#define VALUE_LEN (MX_CONFIG_PATH + 32)
#define VALUE_COL 27                    /* where the values start in the page's list */
#define LIST_ROW 3                      /* the page's list: rows 3 to 20 */
#define LIST_ROWS 18
#define MAX_OPTIONS 16
#define MAX_SERIAL 16                   /* host serial ports offered */
#define PORT_IDS (10 * MX_CONFIG_PORTS)  /* the ids of the port rows: ID_PORT + 10 * port + 0 type, 1 target */
#define POPUP_TOP 2                     /* pop-ups are centred between these rows */
#define POPUP_BOTTOM 21

static const char *const PAGE_NAMES[MX_SETUP_PAGES] = {"drives", "machine", "display", "sound", "ports", "controllers"};
static const int PAGE_TITLES[MX_SETUP_PAGES] = {MX_T_PAGE_DRIVES, MX_T_PAGE_MACHINE, MX_T_PAGE_DISPLAY, MX_T_PAGE_SOUND, MX_T_PAGE_PORTS,
                                                MX_T_PAGE_CONTROLLERS};

const char *mx_setup_page_name(int page) { return page >= 0 && page < MX_SETUP_PAGES ? PAGE_NAMES[page] : "?"; }
int mx_setup_page_by_name(const char *name) {
    for (int i = 0; i < MX_SETUP_PAGES; ++i) if (!strcmp(name, PAGE_NAMES[i])) return i;
    return -1;
}

/* --- what changed ---------------------------------------------------------------------- */

unsigned mx_setup_diff(const mx_config *a, const mx_config *b) {
    unsigned f = 0;
    if (a->bios != b->bios || a->cpu != b->cpu || a->speed != b->speed || a->ram_kb != b->ram_kb || a->display != b->display || a->fpu != b->fpu ||
        a->sio_adapter != b->sio_adapter || a->pc_serial != b->pc_serial || a->floppies != b->floppies || a->game_adapter != b->game_adapter ||
        a->winchester != b->winchester)
        f |= MX_CHANGE_RESTART;
    /* The BIOS counts the fixed disks at power-on: another one is another machine. */
    for (unsigned i = 0; i < MX_CONFIG_HARD; ++i)
        if (strcmp(a->hard[i].image, b->hard[i].image)) f |= MX_CHANGE_RESTART | MX_CHANGE_DRIVES;
    if (a->pc_serial != b->pc_serial) f |= MX_CHANGE_PORTS;                 /* the ports to open are another set */
    for (unsigned i = 0; i < MX_CONFIG_ROMS; ++i)
        if (strcmp(a->rom[i], b->rom[i])) f |= MX_CHANGE_RESTART;
    if (a->sound != b->sound || a->volume != b->volume || a->drive_sound != b->drive_sound || a->drive_volume != b->drive_volume) f |= MX_CHANGE_SOUND;
    if (a->language != b->language) f |= MX_CHANGE_LANGUAGE;
    for (unsigned i = 0; i < MX_CONFIG_DRIVES; ++i)
        if (a->drives[i].fast != b->drives[i].fast) f |= MX_CHANGE_DISK_SPEED;
    for (unsigned i = 0; i < MX_CONFIG_HARD; ++i)
        if (a->hard[i].fast != b->hard[i].fast) f |= MX_CHANGE_DISK_SPEED;
    if (a->look != b->look || a->monitor != b->monitor || a->aspect != b->aspect || a->scale != b->scale || a->filter != b->filter ||
        a->fullscreen != b->fullscreen || a->vsync != b->vsync || a->snow != b->snow || a->drive_lights != b->drive_lights) f |= MX_CHANGE_VIDEO;
    for (unsigned i = 0; i < MX_CONFIG_PORTS; ++i)
        if (a->ports[i].type != b->ports[i].type || (a->ports[i].type != MX_END_NONE && strcmp(a->ports[i].path, b->ports[i].path))) f |= MX_CHANGE_PORTS;
    for (unsigned i = 0; i < MX_CONFIG_DRIVES; ++i) {
        if (strcmp(a->drives[i].image, b->drives[i].image) || a->drives[i].write_protect != b->drives[i].write_protect) f |= MX_CHANGE_DRIVES;
        if (a->drives[i].tracks != b->drives[i].tracks) f |= MX_CHANGE_RESTART;         /* the drive is another one */
    }
    if (a->light_pen != b->light_pen) f |= MX_CHANGE_CONTROLLERS;
    for (unsigned i = 0; i < MX_CONFIG_STAND_KEYS; ++i)
        if (strcmp(a->stand_keys[i], b->stand_keys[i])) f |= MX_CHANGE_CONTROLLERS;
    for (unsigned i = 0; i < MX_CONFIG_STICKS; ++i) {
        const mx_stick_config *x = &a->sticks[i], *y = &b->sticks[i];
        if (x->device != y->device || (x->device == MX_STICK_CONTROLLER && x->pad != y->pad) || x->dead_zone != y->dead_zone ||
            x->range != y->range || x->centre_x != y->centre_x || x->centre_y != y->centre_y) f |= MX_CHANGE_CONTROLLERS;
    }
    return f;
}

/* --- the state ------------------------------------------------------------------------- */

typedef enum { MODAL_NONE, MODAL_MENU, MODAL_DIALOG, MODAL_INPUT, MODAL_PICKER, MODAL_CALIBRATE, MODAL_KEYS } modal_kind;
typedef enum {
    TAG_NONE, TAG_CHOICE, TAG_BLANK_SIZE, TAG_BLANK_NAME, TAG_OVERWRITE, TAG_INSERT_BLANK, TAG_PORT_TARGET, TAG_LEAVE,
    TAG_RESET, TAG_POWER, TAG_SAVE_ERROR, TAG_PICK_IMAGE, TAG_NOTE, TAG_PORT_PICK, TAG_ROM_MENU, TAG_PICK_ROM,
    TAG_HARD_SIZE, TAG_HARD_NAME, TAG_HARD_OVERWRITE, TAG_HARD_ATTACH, TAG_PICK_HARD
} modal_tag;

/* Row ids: what a row is, whatever its place on the page. */
enum {
    ID_DRIVE = 10,                      /* + 10 * drive + PART_* */
    ID_HARD = 60,                       /* + 10 * fixed disk + HARD_* */
    ID_BIOS = 100, ID_CPU, ID_SPEED, ID_RAM, ID_DISPLAY, ID_RESET, ID_POWER, ID_FPU, ID_FLOPPIES, ID_WINCHESTER, ID_EXIT,
    ID_ROM = 120,                       /* + socket: the RAM module's EPROM sockets */
    ID_LOOK = 200, ID_MONITOR, ID_ASPECT, ID_SCALE, ID_FILTER, ID_FULLSCREEN, ID_VSYNC, ID_LANGUAGE, ID_DRIVE_LIGHTS, ID_SNOW,
    ID_SOUND = 300, ID_VOLUME, ID_DRIVE_SOUND, ID_DRIVE_VOLUME,
    ID_ADAPTER = 390, ID_PC_SERIAL,
    ID_PORT = 400,                      /* + 10 * port + 0 type, 1 target */
    ID_GAME_ADAPTER = 490, ID_LIGHT_PEN, ID_MOUSE, ID_STAND_KEYS,
    ID_STICK = 500,                     /* + 10 * stick + STICK_* */
    ID_PADS = 590
};
enum { PART_HEAD, PART_DISK, PART_SIZE, PART_INSERT, PART_EJECT, PART_PROTECT, PART_BLANK, PART_TRACKS, PART_FAST };
enum { HARD_HEAD, HARD_IMAGE, HARD_SIZE, HARD_ATTACH, HARD_DETACH, HARD_BLANK, HARD_FAST };
enum { STICK_HEAD, STICK_DEVICE, STICK_DEAD, STICK_RANGE, STICK_CENTRE, STICK_CALIBRATE };

static const unsigned BLANK_KIB[5] = {160, 180, 320, 360, 720};        /* 720 KiB: only for an 80-track drive */

struct mx_setup {
    mx_setup_env env;
    mx_config edit, initial, running;
    int saved_fullscreen, fullscreen_touched;
    mx_tui tui;
    int page, shift;
    /* the page's rows */
    mx_tui_item items[MAX_ROWS];
    int ids[MAX_ROWS];
    char labels[MAX_ROWS][64];
    char values[MAX_ROWS][VALUE_LEN];
    char hints[MAX_ROWS][256];
    size_t rows;
    mx_tui_list list;
    int selected_id[MX_SETUP_PAGES];
    /* the pop-up in front */
    modal_kind modal;
    modal_tag tag;
    int arg;
    mx_tui_item menu_items[MAX_OPTIONS];
    char menu_labels[MAX_OPTIONS][72];
    char menu_title[48];
    mx_tui_list menu;
    mx_tui_dialog dialog;
    mx_tui_input input;
    char input_title[48], input_prompt[80];
    int picker_open;
    mx_tui_picker picker;
    int cal_step, cal_pad, cal_deviation;
    /* the keys of the keyboard joystick: a list of the six functions, one of which may be waiting for its key */
    mx_tui_item keys_items[MX_CONFIG_STAND_KEYS];
    char keys_labels[MX_CONFIG_STAND_KEYS][32];
    char keys_values[MX_CONFIG_STAND_KEYS][MX_CONFIG_KEY_NAME + 16];
    mx_tui_list keys_list;
    int keys_wait;
    char keys_message[192];
    int blank_index;                    /* the size chosen for a new blank disk */
    char blank_path[MX_CONFIG_PATH];    /* the file of a new blank disk, for the questions after its name */
    /* host game controllers */
    char pad_names[MX_CONFIG_PADS][64];
    unsigned pad_count;
    int pad_x[MX_CONFIG_PADS], pad_y[MX_CONFIG_PADS];
    char last_folder[MX_CONFIG_PATH];
    /* the host's serial ports, as found or as the shell says (tests) */
    char serial_names[MAX_SERIAL][40];
    unsigned serial_count;
    int serial_given;
    int done, reset;
    int want_exit, exit;                /* the program is to end once the screen is left (the Exit item): want_exit asked for, exit saved and left */
};

/* --- the file system for the picker -------------------------------------------------------- */

static int is_root(const char *p) {
    const size_t n = strlen(p);
    if (mx_host_separator() == '/') return n == 1 && p[0] == '/';
    return n == 3 && p[1] == ':' && (p[2] == '\\' || p[2] == '/');
}

static int is_separator(char c) { return c == '/' || (mx_host_separator() == '\\' && c == '\\'); }

/* No separator at the end, except for a root. */
static void trim_folder(char *p) {
    size_t n = strlen(p);
    while (n > 1 && is_separator(p[n - 1]) && !is_root(p)) p[--n] = 0;
}

static int fs_list(void *ctx, const char *folder, mx_tui_entry_fn fn, void *fn_ctx) { (void)ctx; return mx_host_list_folder(folder, fn, fn_ctx); }
static int fs_roots(void *ctx, mx_tui_entry_fn fn, void *fn_ctx) { (void)ctx; return mx_host_list_roots(fn, fn_ctx); }

static int fs_parent(void *ctx, const char *folder, char *out, size_t capacity) {
    (void)ctx;
    char tmp[MX_CONFIG_PATH];
    if (is_root(folder)) return 0;
    snprintf(tmp, sizeof tmp, "%s", folder);
    trim_folder(tmp);
    char *cut = NULL;
    for (char *c = tmp; *c; ++c) if (is_separator(*c)) cut = c;
    if (!cut) return 0;
    if (cut == tmp) tmp[1] = 0;                                                  /* "/x" -> "/" */
    else if (mx_host_separator() == '\\' && cut == tmp + 2 && tmp[1] == ':') cut[1] = 0;     /* "C:\x" -> "C:\" */
    else *cut = 0;
    snprintf(out, capacity, "%s", tmp);
    return 1;
}

static void fs_join(void *ctx, const char *folder, const char *name, char *out, size_t capacity) {
    (void)ctx;
    const size_t n = strlen(folder);
    snprintf(out, capacity, "%s%s%s", folder, n && is_separator(folder[n - 1]) ? "" : (const char[]){mx_host_separator(), 0}, name);
}

static mx_tui_fs host_fs(void) {
    mx_tui_fs fs = {NULL, fs_list, fs_roots, fs_parent, fs_join};
    return fs;
}

/* The folder of a file's path ("" when it has none). */
static void folder_of(const char *path, char *out, size_t capacity) {
    snprintf(out, capacity, "%s", path);
    char *cut = NULL;
    for (char *c = out; *c; ++c) if (is_separator(*c)) cut = c;
    if (cut) cut[1] = 0; else out[0] = 0;
}

/* A path as the configuration holds it: relative to the configuration's
 * folder when it lies inside it (as the reader would give it back). */
static void config_path(const mx_setup *s, const char *absolute, char *out, size_t capacity) {
    char base[MX_CONFIG_PATH], full_base[MX_CONFIG_PATH];
    folder_of(s->edit.source, base, sizeof base);
    if (!mx_host_full_path(base[0] ? base : ".", full_base, sizeof full_base)) { snprintf(out, capacity, "%s", absolute); return; }
    trim_folder(full_base);
    const size_t n = strlen(full_base);
    if (!strncmp(absolute, full_base, n) && is_separator(absolute[n]) && !is_root(full_base)) snprintf(out, capacity, "%s%s", base, absolute + n + 1);
    else snprintf(out, capacity, "%s", absolute);
}

/* A name typed for an output file, as the configuration's reader would give
 * it back: a relative one belongs to the configuration's folder. */
static void output_path(const mx_setup *s, const char *typed, char *out, size_t capacity) {
    const int absolute = typed[0] == '/' || typed[0] == '\\' || (typed[0] && typed[1] == ':');
    char base[MX_CONFIG_PATH];
    folder_of(s->edit.source, base, sizeof base);
    if (absolute) snprintf(out, capacity, "%s", typed);
    else snprintf(out, capacity, "%s%s", base, typed);
}

/* Where a disk picker starts: the last folder used, the drive's image's
 * folder, disks/ beside the configuration, the configuration's folder. */
static void disk_folder(const mx_setup *s, unsigned drive, char *out, size_t capacity) {
    char path[MX_CONFIG_PATH], base[MX_CONFIG_PATH];
    path[0] = 0;
    if (s->last_folder[0]) snprintf(path, sizeof path, "%s", s->last_folder);
    else if (s->edit.drives[drive].image[0]) folder_of(s->edit.drives[drive].image, path, sizeof path);
    if (!path[0]) {
        folder_of(s->edit.source, base, sizeof base);
        char disks[MX_CONFIG_PATH];
        snprintf(disks, sizeof disks, "%sdisks", base);
        snprintf(path, sizeof path, "%s", mx_host_folder_exists(disks) ? disks : base[0] ? base : ".");
    }
    if (!mx_host_full_path(path, out, capacity)) snprintf(out, capacity, "%s", path);
    trim_folder(out);
}

/* --- the options of the choices -------------------------------------------------------------- */

typedef struct options {
    int count, current;
    char label[MAX_OPTIONS][64];
    int enabled[MAX_OPTIONS];
    const char *title;
} options;

/* The percentages of a setting in steps from lo to hi, with the file's own
 * value among them when it is between two steps. */
static int percent_list(int lo, int hi, int step, int current, int values[MAX_OPTIONS]) {
    int n = 0, placed = 0;
    for (int v = lo; v <= hi && n < MAX_OPTIONS - 1; v += step) {
        if (!placed && current < v) { values[n++] = current; placed = 1; }
        if (v == current) placed = 1;
        values[n++] = v;
    }
    return n;
}

/* The percentage a volume row sets: the overall volume or the drives'. */
static int *volume_field(mx_setup *s, int id) { return id == ID_DRIVE_VOLUME ? &s->edit.drive_volume : &s->edit.volume; }
static int is_volume(int id) { return id == ID_VOLUME || id == ID_DRIVE_VOLUME; }

typedef struct choice {
    int id;
    size_t offset;                      /* of a uint32_t in mx_config */
    unsigned count;
    const char *const *labels;          /* as written (a version, a chip's name) ... */
    const int *texts;                   /* ... or the table's texts: one of the two */
    const uint32_t *values;
    int title;                          /* the table's text */
} choice;

static const char *const LIT_BIOS[] = {"3.0", "4.0", "5.0", "5.0-alt"};
static const uint32_t V_BIOS[] = {MX_BIOS_3_0, MX_BIOS_4_0, MX_BIOS_5_0, MX_BIOS_5_0_ALT};
static const char *const LIT_CPU[] = {"Intel 8086", "Intel 8088"};
static const uint32_t V_CPU[] = {MX_CPU_8086, MX_CPU_8088};
static const int TXT_SPEED[] = {MX_T_V_SPEED_477, MX_T_V_SPEED_716, MX_T_V_SPEED_954, MX_T_V_SPEED_FREE};
static const uint32_t V_SPEED[] = {MX_SPEED_4_77, MX_SPEED_7_16, MX_SPEED_9_54, MX_SPEED_UNTHROTTLED};
static const int TXT_RAM[] = {MX_T_V_RAM_640, MX_T_V_RAM_256, MX_T_V_RAM_192, MX_T_V_RAM_128, MX_T_V_RAM_64};
static const uint32_t V_RAM[] = {640, 256, 192, 128, 64};
static const int TXT_FLOPPIES[] = {MX_T_V_FLOPPIES_1, MX_T_V_FLOPPIES_2, MX_T_V_FLOPPIES_3, MX_T_V_FLOPPIES_4};
static const uint32_t V_FLOPPIES[] = {1, 2, 3, 4};
static const int TXT_PC_SERIAL[] = {MX_T_V_PC_SERIAL_NONE, MX_T_V_PC_SERIAL_ONE, MX_T_V_PC_SERIAL_TWO};
static const uint32_t V_PC_SERIAL[] = {MX_PCSERIAL_NONE, MX_PCSERIAL_ONE, MX_PCSERIAL_TWO};
static const int TXT_DISPLAY[] = {MX_T_V_DISPLAY_80, MX_T_V_DISPLAY_40, MX_T_V_DISPLAY_MDA, MX_T_V_NO_DISPLAY};
static const uint32_t V_DISPLAY[] = {MX_DISPLAY_CGA80, MX_DISPLAY_CGA40, MX_DISPLAY_MDA, MX_DISPLAY_NONE};
static const int TXT_LOOK[] = {MX_T_V_LOOK_CRT, MX_T_V_LOOK_PLAIN};
static const uint32_t V_LOOK[] = {MX_LOOK_CRT, MX_LOOK_PLAIN};
static const int TXT_MONITOR[] = {MX_T_V_MONITOR_RGBI, MX_T_V_MONITOR_RGB, MX_T_V_MONITOR_COMPOSITE};
static const uint32_t V_MONITOR[] = {MX_MONITOR_RGBI, MX_MONITOR_RGB, MX_MONITOR_MONO_GREEN};
static const int TXT_ASPECT[] = {MX_T_V_ASPECT_ORIGINAL, MX_T_V_ASPECT_SQUARE};
static const uint32_t V_ASPECT[] = {MX_CONFIG_ASPECT_ORIGINAL, MX_CONFIG_ASPECT_SQUARE};
static const int TXT_SCALE[] = {MX_T_V_SCALE_FIT, MX_T_V_SCALE_INTEGER};
static const uint32_t V_SCALE[] = {MX_CONFIG_SCALE_FIT, MX_CONFIG_SCALE_INTEGER};
static const int TXT_FILTER[] = {MX_T_V_FILTER_NEAREST, MX_T_V_FILTER_LINEAR, MX_T_V_FILTER_SHARP};
static const uint32_t V_FILTER[] = {MX_CONFIG_FILTER_NEAREST, MX_CONFIG_FILTER_LINEAR, MX_CONFIG_FILTER_SHARP};
static const int TXT_LANGUAGE[] = {MX_T_V_LANGUAGE_EN, MX_T_V_LANGUAGE_RO};
static const uint32_t V_LANGUAGE[] = {MX_LANG_EN, MX_LANG_RO};

#define CHOICE(id, field, labels, values, title) {id, offsetof(mx_config, field), sizeof labels / sizeof *labels, labels, NULL, values, title}
#define CHOICE_T(id, field, texts, values, title) {id, offsetof(mx_config, field), sizeof texts / sizeof *texts, NULL, texts, values, title}
static const choice CHOICES[] = {
    CHOICE(ID_BIOS, bios, LIT_BIOS, V_BIOS, MX_T_L_BIOS),
    CHOICE(ID_CPU, cpu, LIT_CPU, V_CPU, MX_T_L_CPU),
    CHOICE_T(ID_SPEED, speed, TXT_SPEED, V_SPEED, MX_T_L_SPEED),
    CHOICE_T(ID_RAM, ram_kb, TXT_RAM, V_RAM, MX_T_L_RAM),
    CHOICE_T(ID_DISPLAY, display, TXT_DISPLAY, V_DISPLAY, MX_T_L_DISPLAY),
    CHOICE_T(ID_FLOPPIES, floppies, TXT_FLOPPIES, V_FLOPPIES, MX_T_L_FLOPPIES),
    CHOICE_T(ID_PC_SERIAL, pc_serial, TXT_PC_SERIAL, V_PC_SERIAL, MX_T_L_PC_SERIAL_TITLE),
    CHOICE_T(ID_LOOK, look, TXT_LOOK, V_LOOK, MX_T_L_LOOK),
    CHOICE_T(ID_MONITOR, monitor, TXT_MONITOR, V_MONITOR, MX_T_L_MONITOR),
    CHOICE_T(ID_ASPECT, aspect, TXT_ASPECT, V_ASPECT, MX_T_L_ASPECT),
    CHOICE_T(ID_SCALE, scale, TXT_SCALE, V_SCALE, MX_T_L_SCALE),
    CHOICE_T(ID_FILTER, filter, TXT_FILTER, V_FILTER, MX_T_L_FILTER),
    CHOICE_T(ID_LANGUAGE, language, TXT_LANGUAGE, V_LANGUAGE, MX_T_L_LANGUAGE),
};

typedef struct toggle { int id; size_t offset; } toggle;
static const toggle TOGGLES[] = {
    {ID_FULLSCREEN, offsetof(mx_config, fullscreen)}, {ID_VSYNC, offsetof(mx_config, vsync)}, {ID_SOUND, offsetof(mx_config, sound)}, {ID_DRIVE_SOUND, offsetof(mx_config, drive_sound)},
    {ID_DRIVE_LIGHTS, offsetof(mx_config, drive_lights)}, {ID_SNOW, offsetof(mx_config, snow)},
    {ID_ADAPTER, offsetof(mx_config, sio_adapter)}, {ID_FPU, offsetof(mx_config, fpu)}, {ID_GAME_ADAPTER, offsetof(mx_config, game_adapter)},
    {ID_WINCHESTER, offsetof(mx_config, winchester)},
    {ID_LIGHT_PEN, offsetof(mx_config, light_pen)},
};

/* A drive's Tracks row: the drive, or -1 for any other id. */
static int tracks_drive(int id) {
    if (id < ID_DRIVE || id >= ID_DRIVE + 10 * MX_CONFIG_DRIVES || (id - ID_DRIVE) % 10 != PART_TRACKS) return -1;
    return (id - ID_DRIVE) / 10;
}

static const choice *find_choice(int id) {
    for (size_t i = 0; i < sizeof CHOICES / sizeof *CHOICES; ++i) if (CHOICES[i].id == id) return &CHOICES[i];
    return NULL;
}
/* A drive's Fast access row: the diskette drive, or the fixed disk, it belongs to (-1 for any other id). */
static int fast_drive(int id) {
    if (id < ID_DRIVE || id >= ID_DRIVE + 10 * MX_CONFIG_DRIVES || (id - ID_DRIVE) % 10 != PART_FAST) return -1;
    return (id - ID_DRIVE) / 10;
}
static int fast_hard(int id) {
    if (id < ID_HARD || id >= ID_HARD + 10 * MX_CONFIG_HARD || (id - ID_HARD) % 10 != HARD_FAST) return -1;
    return (id - ID_HARD) / 10;
}
static const toggle *find_toggle(int id) {
    for (size_t i = 0; i < sizeof TOGGLES / sizeof *TOGGLES; ++i) if (TOGGLES[i].id == id) return &TOGGLES[i];
    return NULL;
}
/* The setting a toggle row switches. */
static int is_toggle(int id) { return find_toggle(id) || fast_drive(id) >= 0 || fast_hard(id) >= 0; }
static int *toggle_field(mx_setup *s, int id) {
    if (fast_drive(id) >= 0) return &s->edit.drives[fast_drive(id)].fast;
    if (fast_hard(id) >= 0) return &s->edit.hard[fast_hard(id)].fast;
    return (int *)((char *)&s->edit + find_toggle(id)->offset);
}
static uint32_t *choice_field(mx_setup *s, const choice *c) { return (uint32_t *)((char *)&s->edit + c->offset); }
static const char *choice_label(const choice *c, unsigned i) { return c->texts ? mx_t(c->texts[i]) : c->labels[i]; }

static int bios_present(const mx_setup *s, uint32_t bios) {
    return !s->env.roms || mx_rom_index_find(s->env.roms, mx_romdb_bios(bios)) != NULL;
}

/* What a port can be connected to: the parallel port only drives a printer file. */
static unsigned port_types(unsigned port, uint32_t types[8], const char *labels[8]) {
    static const struct { uint32_t type; int text; } ALL[] = {
        {MX_END_NONE, MX_T_V_PORT_NONE}, {MX_END_LOOPBACK, MX_T_V_PORT_LOOPBACK}, {MX_END_SERIAL, MX_T_V_PORT_SERIAL},
        {MX_END_PIPE, MX_T_V_PORT_PIPE}, {MX_END_TCP, MX_T_V_PORT_TCP}, {MX_END_TCP_LISTEN, MX_T_V_PORT_TCP_LISTEN}, {MX_END_OUTPUT, MX_T_V_PORT_FILE},
        {MX_END_MOUSE, MX_T_V_PORT_MOUSE},
    };
    unsigned n = 0;
    for (size_t i = 0; i < sizeof ALL / sizeof *ALL; ++i) {
        if (mx_config_port_is_parallel(port) && ALL[i].type != MX_END_NONE && ALL[i].type != MX_END_OUTPUT) continue;
        if (ALL[i].type == MX_END_MOUSE && port != MX_CONFIG_PORT_COM1 && port != MX_CONFIG_PORT_COM2) continue;      /* the 8250 ports only */
        types[n] = ALL[i].type;
        labels[n++] = mx_t(ALL[i].text);
    }
    return n;
}

static const char *port_title(unsigned port);
static void note(mx_setup *s, const char *title, const char *text);

/* Text into a buffer of `capacity` bytes, cut where it does not fit. */
static void copy_text(char *to, size_t capacity, const char *from) {
    size_t n = strlen(from);
    if (n >= capacity) n = capacity - 1;
    memmove(to, from, n);
    to[n] = 0;
}

static void add_option(options *o, const char *label, int enabled) {
    if (o->count >= MAX_OPTIONS) return;
    copy_text(o->label[o->count], sizeof o->label[0], label);
    o->enabled[o->count++] = enabled;
}

static void pad_label(const mx_setup *s, unsigned pad, char *out, size_t capacity) {
    if (pad >= 1 && pad <= s->pad_count) snprintf(out, capacity, "%u: %s", pad, s->pad_names[pad - 1]);
    else mx_text_format(out, capacity, MX_T_V_PAD_GONE, pad);
}

/* The PC/XT serial adapter and the Junior's own serial and printer adapter both drive IRQ 4 (mx_config_check): the Ports page lets
 * the second go out before the first comes in, and so do the rows that fit the first (the PC serial ports, the Mouse): the Junior's
 * adapter board is taken out, with a note when it was there. */
static void take_out_adapter(mx_setup *s) {
    if (!s->edit.sio_adapter) return;
    s->edit.sio_adapter = 0;
    note(s, MX_TXT(D_ADAPTER_SWAP), MX_TXT(D_ADAPTER_SWAP_TEXT));
}

/* The Controllers page's Mouse row edits the Ports page's keys: the serial mouse is on COM1 or COM2 of the PC/XT adapter. */
enum { MOUSE_NONE, MOUSE_COM1, MOUSE_COM2, MOUSE_CHOICES };

/* Where the mouse is now: on the port of the 8250 pair that carries it, none when no fitted port does. */
static int mouse_choice(const mx_config *c) {
    if (!mx_config_mouse_attached(c)) return MOUSE_NONE;
    return c->ports[MX_CONFIG_PORT_COM1].type == MX_END_MOUSE ? MOUSE_COM1 : MOUSE_COM2;
}

/* The mouse on COM1 or COM2, or off: the port takes it and the other port lets it go (the host has one mouse). A port needs the
 * 8250 pair, and the pair and the SIO adapter both drive IRQ 4 (mx_config_check): as the Ports page has it, the SIO adapter goes
 * out when the pair comes in, one port for COM1 and two for COM2. None takes the mouse off its port and leaves the boards as they are. */
static void set_mouse(mx_setup *s, int where) {
    mx_config *c = &s->edit;
    for (int i = MOUSE_COM1; i <= MOUSE_COM2; ++i) {
        mx_port_config *port = &c->ports[MX_CONFIG_PORT_COM1 + (unsigned)(i - MOUSE_COM1)];
        if (i != where && port->type == MX_END_MOUSE) { port->type = MX_END_NONE; port->path[0] = 0; }
    }
    if (where != MOUSE_COM1 && where != MOUSE_COM2) return;
    const uint32_t needed = where == MOUSE_COM2 ? MX_PCSERIAL_TWO : MX_PCSERIAL_ONE;
    if (c->pc_serial < needed) c->pc_serial = needed;
    take_out_adapter(s);
    mx_port_config *port = &c->ports[MX_CONFIG_PORT_COM1 + (unsigned)(where - MOUSE_COM1)];
    port->type = MX_END_MOUSE;
    port->path[0] = 0;
}

/* The choices of a CHOICE row and which is current. */
static void options_of(const mx_setup *s, int id, options *o) {
    memset(o, 0, sizeof *o);
    if (id == ID_MOUSE) {
        o->title = MX_TXT(L_MOUSE);
        add_option(o, MX_TXT(V_PORT_NONE), 1);
        add_option(o, MX_TXT(L_COM1), 1);
        add_option(o, MX_TXT(L_COM2), 1);
        o->current = mouse_choice(&s->edit);
        return;
    }
    const choice *c = find_choice(id);
    if (c) {
        const uint32_t now = *(const uint32_t *)((const char *)&s->edit + c->offset);
        o->title = mx_t(c->title);
        for (unsigned i = 0; i < c->count; ++i) {
            char text[64];
            const int present = id != ID_BIOS || bios_present(s, c->values[i]);
            if (present) snprintf(text, sizeof text, "%s", choice_label(c, i));
            else mx_text_format(text, sizeof text, MX_T_V_NOT_IN_ROMS, choice_label(c, i));
            add_option(o, text, present);
            if (c->values[i] == now) o->current = (int)i;
        }
        return;
    }
    if (tracks_drive(id) >= 0) {
        o->title = MX_TXT(L_TRACKS);
        add_option(o, MX_TXT(V_TRACKS_40), 1);
        add_option(o, MX_TXT(V_TRACKS_80), 1);
        o->current = s->edit.drives[tracks_drive(id)].tracks == 80;
        return;
    }
    if (is_volume(id)) {
        int values[MAX_OPTIONS];
        const int volume = id == ID_DRIVE_VOLUME ? s->edit.drive_volume : s->edit.volume;
        const int n = percent_list(0, 100, 10, volume, values);
        o->title = id == ID_VOLUME ? MX_TXT(L_VOLUME) : MX_TXT(L_DRIVE_VOLUME);
        for (int i = 0; i < n; ++i) {
            char text[16];
            snprintf(text, sizeof text, "%d %%", values[i]);
            add_option(o, text, 1);
            if (values[i] == volume) o->current = i;
        }
        return;
    }
    if (id >= ID_PORT && id < ID_PORT + PORT_IDS && (id - ID_PORT) % 10 == 0) {
        const unsigned port = (unsigned)(id - ID_PORT) / 10;
        uint32_t types[8];
        const char *labels[8];
        const unsigned count = port_types(port, types, labels);
        o->title = MX_TXT(L_TARGET_CONNECT);
        for (unsigned i = 0; i < count; ++i) {
            /* The host's serial ports are offered when it has any (the one in use stays what it is). */
            const int found = types[i] != MX_END_SERIAL || s->serial_count || s->edit.ports[port].type == MX_END_SERIAL;
            /* The host has one mouse: it is on one 8250 port at most. */
            const unsigned other = port == MX_CONFIG_PORT_COM1 ? MX_CONFIG_PORT_COM2 : MX_CONFIG_PORT_COM1;
            const int taken = types[i] == MX_END_MOUSE && s->edit.ports[other].type == MX_END_MOUSE;
            char text[64];
            if (taken) mx_text_format(text, sizeof text, MX_T_V_PORT_ON, labels[i], port_title(other));
            else if (found) snprintf(text, sizeof text, "%s", labels[i]);
            else mx_text_format(text, sizeof text, MX_T_V_NONE_FOUND, labels[i]);
            add_option(o, text, found && !taken);
            if (types[i] == s->edit.ports[port].type) o->current = (int)i;
        }
        /* The Z80-SIO has no mouse: a Microsoft mouse driver needs an 8250 port. Said in the list, where it is looked for. */
        if (port == MX_CONFIG_PORT_SIO_A || port == MX_CONFIG_PORT_SIO_B) add_option(o, MX_TXT(V_PORT_MOUSE_PC), 0);
        return;
    }
    if (id >= ID_STICK && id < ID_STICK + 20) {
        const unsigned stick = (unsigned)(id - ID_STICK) / 10;
        const int part = (id - ID_STICK) % 10;
        const mx_stick_config *st = &s->edit.sticks[stick];
        if (part == STICK_DEVICE) {
            o->title = MX_TXT(L_JOYSTICK_FED_BY);
            add_option(o, MX_TXT(V_STICK_NONE), 1);
            add_option(o, MX_TXT(V_STICK_KEYBOARD), 1);
            for (unsigned p = 1; p <= s->pad_count; ++p) {
                char text[96];
                pad_label(s, p, text, sizeof text);
                add_option(o, text, 1);
            }
            if (st->device == MX_STICK_KEYBOARD) o->current = 1;
            else if (st->device == MX_STICK_CONTROLLER) {
                if (st->pad <= s->pad_count) o->current = 1 + (int)st->pad;
                else {          /* a controller that is not there now stays what it is until another is chosen */
                    char text[96];
                    pad_label(s, st->pad, text, sizeof text);
                    add_option(o, text, 0);
                    o->current = o->count - 1;
                }
            }
        } else if (part == STICK_DEAD) {
            int values[MAX_OPTIONS];
            const int n = percent_list(0, 50, 5, st->dead_zone, values);
            o->title = MX_TXT(L_DEAD_ZONE);
            for (int i = 0; i < n; ++i) {
                char text[16];
                snprintf(text, sizeof text, "%d %%", values[i]);
                add_option(o, text, 1);
                if (values[i] == st->dead_zone) o->current = i;
            }
        } else if (part == STICK_RANGE) {
            int values[MAX_OPTIONS];
            const int n = percent_list(50, 100, 5, st->range, values);
            o->title = MX_TXT(L_RANGE);
            for (int i = 0; i < n; ++i) {
                char text[16];
                snprintf(text, sizeof text, "%d %%", values[i]);
                add_option(o, text, 1);
                if (values[i] == st->range) o->current = i;
            }
        }
    }
}

/* --- default targets of the ports -------------------------------------------------------------- */

static void port_default(const mx_setup *s, unsigned port, uint32_t type, char *out, size_t capacity) {
    const int windows = mx_host_separator() == '\\';
    /* The host's first serial port for the first of a kind of port, the second for the second. */
    const unsigned second = port == MX_CONFIG_PORT_SIO_B || port == MX_CONFIG_PORT_COM2;
    const unsigned tcp = 2323u + (port == MX_CONFIG_PORT_SIO_B ? 1u : port == MX_CONFIG_PORT_COM1 ? 2u : port == MX_CONFIG_PORT_COM2 ? 3u : 0u);
    char name[16];
    if (port >= MX_CONFIG_PORT_COM1) snprintf(name, sizeof name, "com%u", port - MX_CONFIG_PORT_COM1 + 1u);
    else snprintf(name, sizeof name, "sio-%c", port == MX_CONFIG_PORT_SIO_B ? 'b' : 'a');
    if (type == MX_END_SERIAL) {
        if (s->serial_count) snprintf(out, capacity, "%s", s->serial_names[second && s->serial_count > 1 ? 1 : 0]);
        else if (windows) snprintf(out, capacity, "COM%u", second ? 2u : 1u);
        else snprintf(out, capacity, "/dev/ttyS%u", second ? 1u : 0u);
    } else if (type == MX_END_PIPE) {
        snprintf(out, capacity, windows ? "\\\\.\\pipe\\mezinxt-%s" : "/tmp/mezinxt-%s", name);
    } else if (type == MX_END_OUTPUT) {
        if (port == MX_CONFIG_PORT_LPT) snprintf(out, capacity, "lpt.out");
        else if (port == MX_CONFIG_PORT_LPT_MDA) snprintf(out, capacity, "lpt-mda.out");
        else snprintf(out, capacity, "%s.out", name);
    } else if (type == MX_END_TCP) {
        snprintf(out, capacity, "127.0.0.1:%u", tcp);
    } else if (type == MX_END_TCP_LISTEN) {
        snprintf(out, capacity, "%u", tcp);
    } else out[0] = 0;
}

/* --- choosing -------------------------------------------------------------------------------------- */

static void apply_option(mx_setup *s, int id, int index) {
    if (id == ID_MOUSE) {
        if (index >= 0 && index < MOUSE_CHOICES) set_mouse(s, index);
        return;
    }
    const choice *c = find_choice(id);
    if (c) {
        if (index < 0 || (unsigned)index >= c->count) return;
        *choice_field(s, c) = c->values[index];
        if (id == ID_LANGUAGE) mx_text_set_language(s->edit.language);       /* applied at once: the next frame is in it */
        if (id == ID_PC_SERIAL && s->edit.pc_serial != MX_PCSERIAL_NONE) take_out_adapter(s);
        return;
    }
    if (tracks_drive(id) >= 0) {
        if (index == 0 || index == 1) s->edit.drives[tracks_drive(id)].tracks = index ? 80u : 40u;
        return;
    }
    if (is_volume(id)) {
        int values[MAX_OPTIONS];
        int *volume = volume_field(s, id);
        const int n = percent_list(0, 100, 10, *volume, values);
        if (index >= 0 && index < n) *volume = values[index];
        return;
    }
    if (id >= ID_PORT && id < ID_PORT + PORT_IDS && (id - ID_PORT) % 10 == 0) {
        const unsigned port = (unsigned)(id - ID_PORT) / 10;
        uint32_t types[8];
        const char *labels[8];
        const unsigned count = port_types(port, types, labels);
        if (index < 0 || (unsigned)index >= count) return;
        const uint32_t type = types[index];
        if (type != s->edit.ports[port].type) {
            char name[MX_CONFIG_PATH];
            s->edit.ports[port].type = type;
            port_default(s, port, type, name, sizeof name);
            if (type == MX_END_OUTPUT) output_path(s, name, s->edit.ports[port].path, sizeof s->edit.ports[port].path);
            else snprintf(s->edit.ports[port].path, sizeof s->edit.ports[port].path, "%s", name);
        }
        return;
    }
    if (id >= ID_STICK && id < ID_STICK + 20) {
        const unsigned stick = (unsigned)(id - ID_STICK) / 10;
        const int part = (id - ID_STICK) % 10;
        mx_stick_config *st = &s->edit.sticks[stick];
        if (part == STICK_DEVICE) {
            mx_stick_config *other = &s->edit.sticks[1 - stick];
            if (index == 0) { st->device = MX_STICK_NONE; st->pad = 0; }
            else if (index == 1) {
                st->device = MX_STICK_KEYBOARD;
                st->pad = 0;
                if (other->device == MX_STICK_KEYBOARD) { other->device = MX_STICK_NONE; other->pad = 0; }
            } else {
                st->device = MX_STICK_CONTROLLER;
                st->pad = (uint32_t)(index - 1);
                if (other->device == MX_STICK_CONTROLLER && other->pad == st->pad) { other->device = MX_STICK_NONE; other->pad = 0; }
            }
        } else if (part == STICK_DEAD || part == STICK_RANGE) {
            int values[MAX_OPTIONS];
            const int n = part == STICK_DEAD ? percent_list(0, 50, 5, st->dead_zone, values) : percent_list(50, 100, 5, st->range, values);
            if (index >= 0 && index < n) *(part == STICK_DEAD ? &st->dead_zone : &st->range) = values[index];
        }
    }
}

/* Left or right on a choice: the next option that can be chosen. */
static void cycle(mx_setup *s, int id, int direction) {
    options o;
    options_of(s, id, &o);
    int index = o.current;
    for (int n = 0; n < o.count; ++n) {
        index = (index + direction + o.count) % o.count;
        if (o.enabled[index]) { apply_option(s, id, index); return; }
    }
}

/* --- pop-ups --------------------------------------------------------------------------------------- */

static void close_modal(mx_setup *s) {
    if (s->picker_open) { mx_tui_picker_close(&s->picker); s->picker_open = 0; }
    s->keys_wait = 0;
    s->keys_message[0] = 0;
    s->modal = MODAL_NONE;
    s->tag = TAG_NONE;
}

static void open_menu(mx_setup *s, modal_tag tag, int arg, const char *title, const options *o) {
    s->modal = MODAL_MENU;
    s->tag = tag;
    s->arg = arg;
    snprintf(s->menu_title, sizeof s->menu_title, "%s", title);
    for (int i = 0; i < o->count; ++i) {
        snprintf(s->menu_labels[i], sizeof s->menu_labels[0], "%s", o->label[i]);
        s->menu_items[i].kind = MX_ITEM_TEXT;
        s->menu_items[i].enabled = o->enabled[i];
        s->menu_items[i].on = 0;
        s->menu_items[i].label = s->menu_labels[i];
        s->menu_items[i].value = NULL;
    }
    memset(&s->menu, 0, sizeof s->menu);
    mx_tui_list_set(&s->menu, s->menu_items, (size_t)o->count, o->count, 0);
    mx_tui_list_select(&s->menu, (size_t)o->current);
}

static void open_dialog(mx_setup *s, modal_tag tag, int arg, const char *title, const char *text, const char *buttons, int selected, int cancel) {
    s->modal = MODAL_DIALOG;
    s->tag = tag;
    s->arg = arg;
    mx_tui_dialog_set(&s->dialog, title, text, buttons, selected, cancel);
}

static void note(mx_setup *s, const char *title, const char *text) { open_dialog(s, TAG_NOTE, 0, title, text, MX_TXT(BUTTON_OK), 0, 0); }

static void open_input(mx_setup *s, modal_tag tag, int arg, const char *title, const char *prompt, const char *text, size_t max) {
    s->modal = MODAL_INPUT;
    s->tag = tag;
    s->arg = arg;
    snprintf(s->input_title, sizeof s->input_title, "%s", title);
    snprintf(s->input_prompt, sizeof s->input_prompt, "%s", prompt);
    mx_tui_input_set(&s->input, text, max);
}

static int open_picker(mx_setup *s, modal_tag tag, int arg, const char *title, const char *folder, const char *filter) {
    const mx_tui_fs fs = host_fs();
    s->tag = tag;
    s->arg = arg;
    s->modal = MODAL_PICKER;
    s->picker_open = mx_tui_picker_open(&s->picker, &fs, title, folder, filter);
    if (!s->picker_open) { close_modal(s); note(s, MX_TXT(D_NO_MEMORY), MX_TXT(D_NO_MEMORY_TEXT)); return 0; }
    return 1;
}

/* --- saving and leaving --------------------------------------------------------------------------- */

/* The message of a failure in English (the diagnostics on stderr), or in the language in use (the pop-up). */
static int save(mx_setup *s, char *error, size_t capacity, int translated) {
    /* Nothing to write without a file, or when nothing changed: the file stays as the user left it. */
    if (!s->edit.source[0] || !mx_setup_diff(&s->initial, &s->edit)) return 1;
    mx_ydoc doc;
    memset(&doc, 0, sizeof doc);
    if (!mx_ydoc_load(&doc, s->edit.source)) {
        if (translated) mx_text_format(error, capacity, MX_T_D_CANNOT_READ_FILE, s->edit.source);
        else snprintf(error, capacity, "Cannot read %s", s->edit.source);
        return 0;
    }
    mx_config out = s->edit;
    if (!s->fullscreen_touched) out.fullscreen = s->saved_fullscreen;
    const int ok = mx_config_store(&out, &doc) && mx_ydoc_save(&doc, s->edit.source);
    mx_ydoc_free(&doc);
    if (!ok) {
        if (translated) mx_text_format(error, capacity, MX_T_D_CANNOT_WRITE_FILE, s->edit.source);
        else snprintf(error, capacity, "Cannot write %s", s->edit.source);
    }
    return ok;
}

int mx_setup_save(mx_setup *s, char *error, size_t capacity) { return save(s, error, capacity, 0); }

static void finish(mx_setup *s) {
    char error[MX_CONFIG_PATH + 64] = "";
    close_modal(s);
    if (!save(s, error, sizeof error, 1)) {
        open_dialog(s, TAG_SAVE_ERROR, 0, MX_TXT(D_CANNOT_SAVE), error, MX_TXT(BUTTON_OK), 0, 0);
        return;
    }
    s->done = 1;
    s->exit = s->want_exit;
}

void mx_setup_request_close(mx_setup *s) {
    close_modal(s);
    if (mx_setup_diff(&s->running, &s->edit) & MX_CHANGE_RESTART)
        open_dialog(s, TAG_LEAVE, 0, MX_TXT(D_LEAVING), MX_TXT(D_LEAVING_TEXT), MX_TXT(D_RESTART_BUTTONS), 0, 1);
    else finish(s);
}

/* --- the drives ---------------------------------------------------------------------------------------- */

/* A drive's disk changed: tell the shell, then keep it in the configuration. */
static int set_media(mx_setup *s, unsigned drive, const char *image, int protect) {
    char error[256] = "";
    if (s->env.media && !s->env.media(s->env.ctx, drive, image, protect, error, sizeof error)) {
        note(s, MX_TXT(D_DISK_REFUSED), error[0] ? error : MX_TXT(D_DRIVE_REFUSED));
        return 0;
    }
    copy_text(s->edit.drives[drive].image, sizeof s->edit.drives[drive].image, image);   /* image may be this very field */
    s->edit.drives[drive].write_protect = protect;
    return 1;
}

static int write_blank(const char *path, unsigned kib) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    static uint8_t zeros[4096];
    int ok = 1;
    for (unsigned i = 0; ok && i < kib / 4; ++i) ok = fwrite(zeros, 1, sizeof zeros, f) == sizeof zeros;
    return fclose(f) == 0 && ok;
}

static void blank_name(mx_setup *s, unsigned drive, unsigned kib, char *out, size_t capacity) {
    char folder[MX_CONFIG_PATH], name[40];
    disk_folder(s, drive, folder, sizeof folder);
    snprintf(name, sizeof name, "blank-%u.img", kib);
    fs_join(NULL, folder, name, out, capacity);
}

/* --- the rows of the pages ----------------------------------------------------------------------------- */

static mx_tui_item *add_row(mx_setup *s, int kind, int id, int enabled, const char *label, const char *hint) {
    if (s->rows >= MAX_ROWS) return &s->items[MAX_ROWS - 1];
    const size_t i = s->rows++;
    s->ids[i] = id;
    copy_text(s->labels[i], sizeof s->labels[i], label);
    s->values[i][0] = 0;
    copy_text(s->hints[i], sizeof s->hints[i], hint ? hint : "");
    s->items[i].kind = kind;
    s->items[i].enabled = enabled;
    s->items[i].on = 0;
    s->items[i].label = s->labels[i];
    s->items[i].value = s->values[i];
    return &s->items[i];
}

static void add_gap(mx_setup *s) { add_row(s, MX_ITEM_INFO, 0, 1, "", NULL); }

static void set_value(mx_setup *s, mx_tui_item *item, const char *text) {
    const size_t i = (size_t)(item - s->items);
    snprintf(s->values[i], sizeof s->values[i], "%s", text);
}

static void add_choice(mx_setup *s, int id, const char *label, int enabled, const char *hint) {
    options o;
    options_of(s, id, &o);
    mx_tui_item *item = add_row(s, MX_ITEM_CHOICE, id, enabled, label, hint);
    set_value(s, item, o.current >= 0 && o.current < o.count ? o.label[o.current] : "?");
}

static void add_toggle(mx_setup *s, int id, const char *label, int enabled, const char *hint) {
    mx_tui_item *item = add_row(s, MX_ITEM_TOGGLE, id, enabled, label, hint);
    item->on = *toggle_field(s, id) != 0;
}

/* The Winchester coupler's two fixed disks: an image attached or not, a new blank one. Their rows act only when the
 * coupler is fitted; the second disk needs the first. */
static void rows_hard(mx_setup *s) {
    const int fitted = s->edit.winchester != 0;
    for (unsigned n = 0; n < MX_CONFIG_HARD; ++n) {
        const int base = ID_HARD + 10 * (int)n;
        const char *image = s->edit.hard[n].image;
        const int has = image[0] != 0;
        const int first_needed = n > 0 && !s->edit.hard[0].image[0];
        const int second_attached = n == 0 && s->edit.hard[1].image[0];
        char text[MX_CONFIG_PATH + 16];
        add_gap(s);
        mx_text_format(text, sizeof text, MX_T_HARD_HEADING, n, 'C' + (int)n);
        add_row(s, MX_ITEM_HEADING, base + HARD_HEAD, 1, text, NULL);
        set_value(s, add_row(s, MX_ITEM_INFO, base + HARD_IMAGE, 1, MX_TXT(HARD_IMAGE), NULL), has ? image : MX_TXT(HARD_NONE));
        if (has) {
            uint64_t size = 0;
            uint32_t cylinders = 0, heads = 0;
            if (!mx_host_file_size(image, &size)) snprintf(text, sizeof text, "%s", MX_TXT(DRIVE_FILE_NOT_FOUND));
            else if (!mx_hdc_geometry(size, NULL, &cylinders, &heads)) snprintf(text, sizeof text, "%s", MX_TXT(HARD_NOT_IMAGE));
            else {
                const unsigned tenths = (unsigned)((size + 50000u) / 100000u);
                mx_text_format(text, sizeof text, MX_T_HARD_SIZE_VALUE, tenths / 10u, tenths % 10u, (unsigned)cylinders, (unsigned)heads);
            }
            set_value(s, add_row(s, MX_ITEM_INFO, base + HARD_SIZE, 1, MX_TXT(DRIVE_SIZE), NULL), text);
        }
        const char *why = !fitted ? MX_TXT(HINT_HARD_NO_COUPLER) : first_needed ? MX_TXT(HINT_HARD_NEEDS_FIRST) : NULL;
        add_row(s, MX_ITEM_ACTION, base + HARD_ATTACH, !why, MX_TXT(HARD_ATTACH), why ? why : MX_TXT(HINT_HARD_ATTACH));
        const char *detach_why = !fitted ? MX_TXT(HINT_HARD_NO_COUPLER) : !has ? MX_TXT(HINT_HARD_EMPTY) : second_attached ? MX_TXT(HINT_HARD_SECOND_FIRST) : NULL;
        add_row(s, MX_ITEM_ACTION, base + HARD_DETACH, !detach_why, MX_TXT(HARD_DETACH), detach_why ? detach_why : MX_TXT(HINT_HARD_DETACH));
        add_row(s, MX_ITEM_ACTION, base + HARD_BLANK, !why, MX_TXT(HARD_NEW_BLANK), why ? why : MX_TXT(HINT_HARD_NEW_BLANK));
        add_toggle(s, base + HARD_FAST, MX_TXT(L_FAST_ACCESS), !why, why ? why : MX_TXT(HINT_FAST_ACCESS));
    }
}

static void rows_drives(mx_setup *s) {
    const unsigned count = s->env.drives < MX_CONFIG_DRIVES ? s->env.drives : MX_CONFIG_DRIVES;
    for (unsigned d = 0; d < count; ++d) {
        const int base = ID_DRIVE + 10 * (int)d;
        const mx_drive_config *dc = &s->edit.drives[d];
        const int has = dc->image[0] != 0;
        char text[MX_CONFIG_PATH + 16];
        mx_text_format(text, sizeof text, MX_T_DRIVE_HEADING, 'A' + (int)d);
        if (d) add_gap(s);
        add_row(s, MX_ITEM_HEADING, base + PART_HEAD, 1, text, NULL);
        mx_tui_item *disk = add_row(s, MX_ITEM_INFO, base + PART_DISK, 1, MX_TXT(DRIVE_DISK), NULL);
        set_value(s, disk, has ? dc->image : MX_TXT(DRIVE_EMPTY));
        if (has) {
            uint64_t size = 0;
            if (mx_host_file_size(dc->image, &size)) mx_text_format(text, sizeof text, MX_T_DRIVE_SIZE_KIB, (unsigned)((size + 1023) / 1024));
            else snprintf(text, sizeof text, "%s", MX_TXT(DRIVE_FILE_NOT_FOUND));
            set_value(s, add_row(s, MX_ITEM_INFO, base + PART_SIZE, 1, MX_TXT(DRIVE_SIZE), NULL), text);
        }
        add_row(s, MX_ITEM_ACTION, base + PART_INSERT, 1, MX_TXT(DRIVE_INSERT), MX_TXT(HINT_DRIVE_INSERT));
        add_row(s, MX_ITEM_ACTION, base + PART_EJECT, has, MX_TXT(DRIVE_EJECT), has ? MX_TXT(HINT_DRIVE_EJECT) : MX_TXT(HINT_DRIVE_EMPTY));
        mx_tui_item *protect = add_row(s, MX_ITEM_TOGGLE, base + PART_PROTECT, has, MX_TXT(DRIVE_PROTECT),
                                       has ? MX_TXT(HINT_DRIVE_PROTECT) : MX_TXT(HINT_DRIVE_EMPTY));
        protect->on = dc->write_protect != 0;
        add_toggle(s, base + PART_FAST, MX_TXT(L_FAST_ACCESS), 1, MX_TXT(HINT_FAST_ACCESS));
        add_row(s, MX_ITEM_ACTION, base + PART_BLANK, 1, MX_TXT(DRIVE_NEW_BLANK), MX_TXT(HINT_DRIVE_NEW_BLANK));
        add_choice(s, base + PART_TRACKS, MX_TXT(L_TRACKS), !has, has ? MX_TXT(HINT_TRACKS_EJECT) : MX_TXT(HINT_TRACKS));
    }
    rows_hard(s);
}

static void rows_machine(mx_setup *s) {
    add_row(s, MX_ITEM_HEADING, 0, 1, MX_TXT(HEAD_BOARD), NULL);
    add_choice(s, ID_BIOS, MX_TXT(L_BIOS), 1, MX_TXT(HINT_BIOS));
    add_choice(s, ID_CPU, MX_TXT(L_CPU), 1, MX_TXT(HINT_CPU));
    add_choice(s, ID_SPEED, MX_TXT(L_SPEED), 1, MX_TXT(HINT_SPEED));
    add_choice(s, ID_RAM, MX_TXT(L_RAM), 1, MX_TXT(HINT_RAM));
    add_choice(s, ID_DISPLAY, MX_TXT(L_DISPLAY), 1, MX_TXT(HINT_DISPLAY));
    add_toggle(s, ID_FPU, MX_TXT(L_FPU), 1, MX_TXT(HINT_FPU));
    add_gap(s);
    add_row(s, MX_ITEM_HEADING, 0, 1, MX_TXT(HEAD_MACHINE), NULL);
    add_row(s, MX_ITEM_ACTION, ID_RESET, 1, MX_TXT(L_RESET), MX_TXT(HINT_RESET));
    add_row(s, MX_ITEM_ACTION, ID_POWER, 1, MX_TXT(L_POWER), MX_TXT(HINT_POWER));
    add_row(s, MX_ITEM_ACTION, ID_EXIT, 1, MX_TXT(L_EXIT), MX_TXT(HINT_EXIT));
    add_gap(s);
    add_row(s, MX_ITEM_HEADING, 0, 1, MX_TXT(HEAD_OPTIONS), NULL);
    /* The coupler's BIOS and the module's C8000h socket exclude each other (mx_config_check). */
    const int socket_used = s->edit.rom[0][0] != 0 && !s->edit.winchester;
    add_toggle(s, ID_WINCHESTER, MX_TXT(L_WINCHESTER), !socket_used, socket_used ? MX_TXT(HINT_WINCHESTER_ROM) : MX_TXT(HINT_WINCHESTER));
    add_choice(s, ID_FLOPPIES, MX_TXT(L_FLOPPIES), 1, MX_TXT(HINT_FLOPPIES));
    const int module = s->edit.ram_kb == 640;
    for (unsigned r = 0; r < MX_CONFIG_ROMS; ++r) {
        char label[64], hint[256];
        mx_text_format(label, sizeof label, MX_T_L_ROM, (unsigned)mx_config_rom_address(r));
        const int taken = r == 0 && s->edit.winchester;     /* the Winchester coupler's BIOS is there */
        if (taken) snprintf(hint, sizeof hint, "%s", MX_TXT(HINT_ROM_WINCHESTER));
        else if (module) mx_text_format(hint, sizeof hint, MX_T_HINT_ROM, mx_config_rom_address(r) == 0xF8000u ? MX_TXT(ROM_LIMIT_16) : MX_TXT(ROM_LIMIT_32));
        else snprintf(hint, sizeof hint, "%s", MX_TXT(HINT_ROM_NO_MODULE));
        set_value(s, add_row(s, MX_ITEM_FIELD, ID_ROM + (int)r, module && !taken, label, hint), s->edit.rom[r][0] ? s->edit.rom[r] : MX_TXT(V_ROM_NONE));
    }
}

static void rows_display(mx_setup *s) {
    add_choice(s, ID_LOOK, MX_TXT(L_LOOK), 1, MX_TXT(HINT_LOOK));
    /* The monochrome coupler's monitor is the monochrome one, whatever is set here. */
    const int mda = s->edit.display == MX_DISPLAY_MDA;
    add_choice(s, ID_MONITOR, MX_TXT(L_MONITOR), !mda, mda ? MX_TXT(HINT_MONITOR_MDA) : MX_TXT(HINT_MONITOR));
    if (mda) set_value(s, &s->items[s->rows - 1], MX_TXT(V_MONITOR_MONO));
    add_choice(s, ID_ASPECT, MX_TXT(L_ASPECT), 1, MX_TXT(HINT_ASPECT));
    add_choice(s, ID_SCALE, MX_TXT(L_SCALE), 1, MX_TXT(HINT_SCALE));
    /* The CRT shader smooths the picture itself whatever the filter says (video.c): the row acts for the plain look only, and
     * says why in its place (a dimmed row cannot be selected, so its hint is not seen: the value is). */
    const int crt = s->edit.look == MX_LOOK_CRT && !s->env.no_crt;
    add_choice(s, ID_FILTER, MX_TXT(L_FILTER), !crt, crt ? MX_TXT(HINT_FILTER_CRT) : MX_TXT(HINT_FILTER));
    if (crt) set_value(s, &s->items[s->rows - 1], MX_TXT(V_FILTER_CRT));
    add_toggle(s, ID_FULLSCREEN, MX_TXT(L_FULLSCREEN), 1, MX_TXT(HINT_FULLSCREEN));
    add_toggle(s, ID_VSYNC, MX_TXT(L_VSYNC), 1, MX_TXT(HINT_VSYNC));
    /* The snow is the CGA card's: the monochrome coupler's store never waits or snows, and without a coupler there is no picture. */
    add_toggle(s, ID_SNOW, MX_TXT(L_SNOW), !mda && s->edit.display != MX_DISPLAY_NONE, !mda && s->edit.display != MX_DISPLAY_NONE ? MX_TXT(HINT_SNOW) : MX_TXT(HINT_SNOW_NO_CGA));
    add_toggle(s, ID_DRIVE_LIGHTS, MX_TXT(L_DRIVE_LIGHTS), 1, MX_TXT(HINT_DRIVE_LIGHTS));
    add_gap(s);
    add_row(s, MX_ITEM_HEADING, 0, 1, MX_TXT(HEAD_INTERFACE), NULL);
    add_choice(s, ID_LANGUAGE, MX_TXT(L_LANGUAGE), 1, MX_TXT(HINT_LANGUAGE));
}

static void rows_sound(mx_setup *s) {
    add_toggle(s, ID_SOUND, MX_TXT(L_SOUND), 1, MX_TXT(HINT_SOUND));
    add_choice(s, ID_VOLUME, MX_TXT(L_VOLUME), s->edit.sound, s->edit.sound ? MX_TXT(HINT_VOLUME) : MX_TXT(HINT_SOUND_OFF));
    add_toggle(s, ID_DRIVE_SOUND, MX_TXT(L_DRIVE_SOUND), s->edit.sound, s->edit.sound ? MX_TXT(HINT_DRIVE_SOUND) : MX_TXT(HINT_SOUND_OFF));
    const int drives_heard = s->edit.sound && s->edit.drive_sound;
    add_choice(s, ID_DRIVE_VOLUME, MX_TXT(L_DRIVE_VOLUME), drives_heard,
               !s->edit.sound ? MX_TXT(HINT_SOUND_OFF) : s->edit.drive_sound ? MX_TXT(HINT_DRIVE_VOLUME) : MX_TXT(HINT_DRIVE_SOUND_OFF));
    add_gap(s);
    add_row(s, MX_ITEM_HEADING, 0, 1, MX_TXT(HEAD_OUTPUT), NULL);
    set_value(s, add_row(s, MX_ITEM_INFO, 0, 1, MX_TXT(L_SOURCE), NULL), MX_TXT(V_SPEAKER));
    set_value(s, add_row(s, MX_ITEM_INFO, 0, 1, MX_TXT(L_SOURCE_DRIVES), NULL), MX_TXT(V_DRIVES_SOURCE));
    set_value(s, add_row(s, MX_ITEM_INFO, 0, 1, MX_TXT(L_FORMAT), NULL), MX_TXT(V_AUDIO_FORMAT));
}

static const char *port_title(unsigned port) {
    static const int names[MX_CONFIG_PORTS] = {MX_T_L_SIO_A, MX_T_L_SIO_B, MX_T_L_LPT, MX_T_L_COM1, MX_T_L_COM2, MX_T_L_LPT_MDA};
    return mx_t(names[port]);
}

/* The two rows of a port: what it is connected to and the target's name. `fitted`: the board it is on is there. */
static void port_rows(mx_setup *s, unsigned p, int fitted) {
    const int base = ID_PORT + 10 * (int)p;
    const mx_port_config *pc = &s->edit.ports[p];
    const int pc_port = p >= MX_CONFIG_PORT_COM1;
    add_choice(s, base, port_title(p), fitted,
               !fitted ? MX_TXT(HINT_NO_ADAPTER) : p == MX_CONFIG_PORT_LPT ? MX_TXT(HINT_LPT) : p == MX_CONFIG_PORT_LPT_MDA ? MX_TXT(HINT_LPT_MDA)
               : pc_port ? MX_TXT(HINT_SIO_PC) : MX_TXT(HINT_SIO));
    const int what = pc->type == MX_END_SERIAL ? MX_T_L_TARGET_PORT : pc->type == MX_END_PIPE ? MX_T_L_TARGET_PIPE
                   : pc->type == MX_END_TCP ? MX_T_L_TARGET_CONNECT : pc->type == MX_END_TCP_LISTEN ? MX_T_L_TARGET_LISTEN : MX_T_L_TARGET_FILE;
    const int named = pc->type != MX_END_NONE && pc->type != MX_END_LOOPBACK && pc->type != MX_END_MOUSE;
    char label[64];
    snprintf(label, sizeof label, "  %s", mx_t(named ? what : MX_T_L_TARGET));
    mx_tui_item *target = add_row(s, MX_ITEM_FIELD, base + 1, fitted && named, label,
                                  !fitted ? MX_TXT(HINT_NO_ADAPTER)
                                  : pc->type == MX_END_LOOPBACK ? MX_TXT(HINT_LOOPBACK)
                                  : pc->type == MX_END_MOUSE ? MX_TXT(HINT_MOUSE)
                                  : !named ? MX_TXT(HINT_NOTHING)
                                  : pc->type == MX_END_SERIAL && s->serial_count ? MX_TXT(HINT_PICK_SERIAL)
                                                                                  : MX_TXT(HINT_EDIT_NAME));
    set_value(s, target, named ? pc->path : "");
}

static void rows_ports(mx_setup *s) {
    const int fitted = s->edit.sio_adapter != 0;
    for (unsigned p = 0; p <= MX_CONFIG_PORT_LPT; ++p) {
        if (p) add_gap(s);
        port_rows(s, p, fitted);
    }
    add_gap(s);
    add_row(s, MX_ITEM_HEADING, 0, 1, MX_TXT(HEAD_ADAPTER), NULL);
    /* COM1 of the 8250 adapter and the SIO adapter both drive IRQ 4: the board takes one of them. */
    const int free_irq = fitted || s->edit.pc_serial == MX_PCSERIAL_NONE;
    add_toggle(s, ID_ADAPTER, MX_TXT(L_FITTED), free_irq, free_irq ? MX_TXT(HINT_ADAPTER) : MX_TXT(HINT_ADAPTER_IRQ));
    add_gap(s);
    add_row(s, MX_ITEM_HEADING, 0, 1, MX_TXT(HEAD_PC_SERIAL), NULL);
    add_choice(s, ID_PC_SERIAL, MX_TXT(L_PC_PORTS), 1, MX_TXT(HINT_PC_SERIAL));
    for (unsigned k = 0; k < 2; ++k) {
        if (s->edit.pc_serial <= k) break;
        add_gap(s);
        port_rows(s, MX_CONFIG_PORT_COM1 + k, 1);
    }
    /* The monochrome coupler's printer port is on the coupler: there only when it is the display coupler. */
    if (s->edit.display == MX_DISPLAY_MDA) {
        add_gap(s);
        add_row(s, MX_ITEM_HEADING, 0, 1, MX_TXT(HEAD_MDA), NULL);
        port_rows(s, MX_CONFIG_PORT_LPT_MDA, 1);
    }
}

static void rows_controllers(mx_setup *s) {
    const int fitted = s->edit.game_adapter != 0;
    add_row(s, MX_ITEM_HEADING, 0, 1, MX_TXT(HEAD_GAME_ADAPTER), NULL);
    add_toggle(s, ID_GAME_ADAPTER, MX_TXT(L_FITTED), 1, MX_TXT(HINT_GAME_ADAPTER));
    /* The stand-in's keys can be set while a joystick is fed by the keyboard. */
    const int keyboard = fitted && (s->edit.sticks[0].device == MX_STICK_KEYBOARD || s->edit.sticks[1].device == MX_STICK_KEYBOARD);
    add_row(s, MX_ITEM_ACTION, ID_STAND_KEYS, keyboard, MX_TXT(L_STAND_KEYS),
            !fitted ? MX_TXT(HINT_NO_GAME_ADAPTER) : keyboard ? MX_TXT(HINT_STAND_KEYS) : MX_TXT(HINT_KEYS_NEEDS));
    add_gap(s);
    for (unsigned j = 0; j < MX_CONFIG_STICKS; ++j) {
        const int base = ID_STICK + 10 * (int)j;
        const mx_stick_config *st = &s->edit.sticks[j];
        const int pad = fitted && st->device == MX_STICK_CONTROLLER;
        const int connected = pad && st->pad >= 1 && st->pad <= s->pad_count;
        char text[64], fed[256], directions[4 * MX_CONFIG_KEY_NAME + 8];
        mx_text_format(text, sizeof text, MX_T_HEAD_JOYSTICK, j + 1);
        mx_text_format(fed, sizeof fed, MX_T_HINT_FED_KEYBOARD, mx_setup_stand_directions(&s->edit, 0, directions, sizeof directions),
                       s->edit.stand_keys[4], s->edit.stand_keys[5]);
        if (j) add_gap(s);
        add_row(s, MX_ITEM_HEADING, base + STICK_HEAD, 1, text, NULL);
        add_choice(s, base + STICK_DEVICE, MX_TXT(L_FED_BY), fitted,
                   !fitted ? MX_TXT(HINT_NO_GAME_ADAPTER)
                   : st->device == MX_STICK_KEYBOARD ? fed : MX_TXT(HINT_FED_OTHER));
        add_choice(s, base + STICK_DEAD, MX_TXT(L_DEAD_ZONE), pad, pad ? MX_TXT(HINT_DEAD_ZONE) : MX_TXT(HINT_FOR_PAD));
        add_choice(s, base + STICK_RANGE, MX_TXT(L_RANGE), pad, pad ? MX_TXT(HINT_RANGE) : MX_TXT(HINT_FOR_PAD));
        snprintf(text, sizeof text, "x %+d %%   y %+d %%", st->centre_x, st->centre_y);
        set_value(s, add_row(s, MX_ITEM_INFO, base + STICK_CENTRE, 1, MX_TXT(L_CENTRE), NULL), pad ? text : "-");
        add_row(s, MX_ITEM_ACTION, base + STICK_CALIBRATE, connected, MX_TXT(L_CALIBRATE),
                connected ? MX_TXT(HINT_CALIBRATE) : MX_TXT(HINT_CALIBRATE_NEEDS));
    }
    add_gap(s);
    add_row(s, MX_ITEM_HEADING, ID_PADS, 1, MX_TXT(HEAD_PADS), NULL);
    if (!s->pad_count) set_value(s, add_row(s, MX_ITEM_INFO, ID_PADS + 1, 1, MX_TXT(V_NO_PADS), NULL), "");
    for (unsigned p = 0; p < s->pad_count && p < 3; ++p) {
        char label[16];
        snprintf(label, sizeof label, "%u", p + 1);
        set_value(s, add_row(s, MX_ITEM_INFO, ID_PADS + 1 + (int)p, 1, label, NULL), s->pad_names[p]);
    }
    add_gap(s);
    add_row(s, MX_ITEM_HEADING, 0, 1, MX_TXT(HEAD_MOUSE), NULL);
    add_choice(s, ID_MOUSE, MX_TXT(L_MOUSE), 1, MX_TXT(HINT_MOUSE_ROW));
    add_gap(s);
    add_row(s, MX_ITEM_HEADING, 0, 1, MX_TXT(HEAD_LIGHT_PEN), NULL);
    const int cga = s->edit.display == MX_DISPLAY_CGA80 || s->edit.display == MX_DISPLAY_CGA40;      /* the pen is the CGA's */
    add_toggle(s, ID_LIGHT_PEN, MX_TXT(L_ENABLED), cga,
               cga ? MX_TXT(HINT_LIGHT_PEN) : s->edit.display == MX_DISPLAY_MDA ? MX_TXT(HINT_NO_LIGHT_PEN_MDA) : MX_TXT(HINT_NO_LIGHT_PEN));
}

/* The selected row's id, remembered per page. */
static void remember(mx_setup *s) {
    if (s->rows && s->list.selected < s->rows) s->selected_id[s->page] = s->ids[s->list.selected];
}

static int collect_serial(void *ctx, const char *name) {
    mx_setup *s = ctx;
    if (s->serial_count < MAX_SERIAL) snprintf(s->serial_names[s->serial_count++], sizeof s->serial_names[0], "%s", name);
    return 1;
}

/* The host's serial ports now (a USB adapter may have come or gone), unless the shell gave them. */
static void scan_serial(mx_setup *s) {
    if (s->serial_given) return;
    s->serial_count = 0;
    mx_host_list_serial(collect_serial, s);
}

static void rebuild(mx_setup *s) {
    remember(s);                        /* the row selected now, whatever moved it */
    s->rows = 0;
    switch (s->page) {
    case MX_SETUP_DRIVES: rows_drives(s); break;
    case MX_SETUP_MACHINE: rows_machine(s); break;
    case MX_SETUP_DISPLAY: rows_display(s); break;
    case MX_SETUP_SOUND: rows_sound(s); break;
    case MX_SETUP_PORTS: rows_ports(s); break;
    default: rows_controllers(s); break;
    }
    mx_tui_list_set(&s->list, s->items, s->rows, LIST_ROWS, VALUE_COL);
    for (size_t i = 0; i < s->rows; ++i)
        if (s->ids[i] == s->selected_id[s->page] && s->selected_id[s->page]) mx_tui_list_select(&s->list, i);
    remember(s);
}

static void set_page(mx_setup *s, int page) {
    remember(s);
    s->page = (page + MX_SETUP_PAGES) % MX_SETUP_PAGES;
    if (s->page == MX_SETUP_PORTS) scan_serial(s);
    s->rows = 0;
    s->list.selected = 0;
    s->list.top = 0;
    rebuild(s);
}

/* --- what the rows do --------------------------------------------------------------------------------------- */

static const int KEY_FUNCTIONS[MX_CONFIG_STAND_KEYS] = {MX_T_L_KEY_UP, MX_T_L_KEY_DOWN, MX_T_L_KEY_LEFT, MX_T_L_KEY_RIGHT, MX_T_L_KEY_FIRE1, MX_T_L_KEY_FIRE2};

/* The keys pop-up's list: the six functions and the key each has (the one waiting for its key says so). */
static void keys_refresh(mx_setup *s) {
    for (int i = 0; i < MX_CONFIG_STAND_KEYS; ++i) {
        snprintf(s->keys_labels[i], sizeof s->keys_labels[i], "%s", mx_t(KEY_FUNCTIONS[i]));
        snprintf(s->keys_values[i], sizeof s->keys_values[i], "%s", s->keys_wait && (int)s->keys_list.selected == i ? MX_TXT(V_KEY_WAIT) : s->edit.stand_keys[i]);
        s->keys_items[i].kind = MX_ITEM_TEXT;
        s->keys_items[i].enabled = 1;
        s->keys_items[i].on = 0;
        s->keys_items[i].label = s->keys_labels[i];
        s->keys_items[i].value = s->keys_values[i];
    }
    mx_tui_list_set(&s->keys_list, s->keys_items, MX_CONFIG_STAND_KEYS, MX_CONFIG_STAND_KEYS, 0);
}

static void start_blank(mx_setup *s, unsigned drive) {
    options o;
    memset(&o, 0, sizeof o);
    static const int labels[5] = {MX_T_BLANK_160, MX_T_BLANK_180, MX_T_BLANK_320, MX_T_BLANK_360, MX_T_BLANK_720};
    const int sizes = s->edit.drives[drive].tracks == 80 ? 5 : 4;
    for (int i = 0; i < sizes; ++i) add_option(&o, mx_t(labels[i]), 1);
    o.current = sizes == 5 ? 4 : 3;
    open_menu(s, TAG_BLANK_SIZE, (int)drive, MX_TXT(D_NEW_BLANK), &o);
}

/* A fixed disk's image changed: the shell opens it (or lets it go), then the configuration keeps it. */
static int set_hard(mx_setup *s, unsigned n, const char *image) {
    char error[256] = "";
    if (s->env.media && !s->env.media(s->env.ctx, MX_CONFIG_DRIVES + n, image, 0, error, sizeof error)) {
        note(s, MX_TXT(D_DISK_REFUSED), error[0] ? error : MX_TXT(D_DRIVE_REFUSED));
        return 0;
    }
    snprintf(s->edit.hard[n].image, sizeof s->edit.hard[n].image, "%s", image);
    return 1;
}

/* Where a fixed disk picker starts: as for a diskette, from the disk's own image when it has one. */
static void hard_folder(const mx_setup *s, unsigned n, char *out, size_t capacity) {
    if (!s->last_folder[0] && s->edit.hard[n].image[0]) {
        char path[MX_CONFIG_PATH];
        folder_of(s->edit.hard[n].image, path, sizeof path);
        if (path[0] && mx_host_full_path(path, out, capacity)) { trim_folder(out); return; }
    }
    disk_folder(s, 0, out, capacity);
}

static void hard_action(mx_setup *s, unsigned n, int part) {
    if (part == HARD_ATTACH) {
        char folder[MX_CONFIG_PATH], title[64];
        mx_text_format(title, sizeof title, MX_T_PICK_HARD_TITLE, n);
        hard_folder(s, n, folder, sizeof folder);
        open_picker(s, TAG_PICK_HARD, (int)n, title, folder, ".img;.ima;.hdd;.hdi;.dsk;.bin");
    } else if (part == HARD_DETACH) set_hard(s, n, "");
    else if (part == HARD_BLANK) {
        options o;
        memset(&o, 0, sizeof o);
        for (unsigned i = 0; i < MX_HDC_OFFERED; ++i) {
            const mx_hdc_type *t = mx_hdc_offered(i);
            char text[64];
            mx_text_format(text, sizeof text, MX_T_BLANK_HARD, t->megabytes, (unsigned)t->cylinders, (unsigned)t->heads);
            add_option(&o, text, 1);
        }
        open_menu(s, TAG_HARD_SIZE, (int)n, MX_TXT(D_NEW_HARD), &o);
    }
}

static void drive_action(mx_setup *s, unsigned drive, int part) {
    char folder[MX_CONFIG_PATH], title[48];
    if (part == PART_INSERT) {
        mx_text_format(title, sizeof title, MX_T_PICK_TITLE, 'A' + (int)drive);
        disk_folder(s, drive, folder, sizeof folder);
        open_picker(s, TAG_PICK_IMAGE, (int)drive, title, folder, ".img;.ima;.dsk;.vfd;.flp;.bin");
    } else if (part == PART_EJECT) set_media(s, drive, "", 0);
    else if (part == PART_PROTECT) set_media(s, drive, s->edit.drives[drive].image, !s->edit.drives[drive].write_protect);
    else if (part == PART_BLANK) start_blank(s, drive);
}

static void activate(mx_setup *s, int id) {
    if (find_choice(id) || tracks_drive(id) >= 0 || is_volume(id) || id == ID_MOUSE || (id >= ID_PORT && id < ID_PORT + PORT_IDS && (id - ID_PORT) % 10 == 0) ||
        (id >= ID_STICK && id < ID_STICK + 20 && ((id - ID_STICK) % 10 == STICK_DEVICE || (id - ID_STICK) % 10 == STICK_DEAD || (id - ID_STICK) % 10 == STICK_RANGE))) {
        options o;
        options_of(s, id, &o);
        open_menu(s, TAG_CHOICE, id, o.title ? o.title : MX_TXT(MENU_CHOOSE), &o);
    } else if (is_toggle(id)) {
        int *field = toggle_field(s, id);
        *field = !*field;
        if (id == ID_FULLSCREEN) s->fullscreen_touched = 1;
    } else if (id >= ID_ROM && id < ID_ROM + MX_CONFIG_ROMS) {
        options o;
        memset(&o, 0, sizeof o);
        add_option(&o, MX_TXT(M_ROM_NONE), 1);
        add_option(&o, MX_TXT(M_ROM_CHOOSE), 1);
        o.current = s->edit.rom[id - ID_ROM][0] ? 1 : 0;
        char title[64];
        mx_text_format(title, sizeof title, MX_T_L_ROM, (unsigned)mx_config_rom_address((unsigned)(id - ID_ROM)));
        open_menu(s, TAG_ROM_MENU, id - ID_ROM, title, &o);
    } else if (id >= ID_DRIVE && id < ID_DRIVE + 10 * MX_CONFIG_DRIVES) drive_action(s, (unsigned)(id - ID_DRIVE) / 10, (id - ID_DRIVE) % 10);
    else if (id >= ID_HARD && id < ID_HARD + 10 * MX_CONFIG_HARD) hard_action(s, (unsigned)(id - ID_HARD) / 10, (id - ID_HARD) % 10);
    else if (id == ID_STAND_KEYS) {
        s->modal = MODAL_KEYS;
        s->tag = TAG_NONE;
        s->keys_wait = 0;
        s->keys_message[0] = 0;
        memset(&s->keys_list, 0, sizeof s->keys_list);
        keys_refresh(s);
    } else if (id == ID_RESET) open_dialog(s, TAG_RESET, 0, MX_TXT(D_RESET), MX_TXT(D_RESET_TEXT), MX_TXT(D_RESET_BUTTONS), 0, 1);
    else if (id == ID_POWER) open_dialog(s, TAG_POWER, 0, MX_TXT(D_POWER), MX_TXT(D_POWER_TEXT), MX_TXT(D_RESTART_BUTTONS), 0, 1);
    else if (id == ID_EXIT) mx_setup_request_exit(s);
    else if (id >= ID_PORT && id < ID_PORT + PORT_IDS && (id - ID_PORT) % 10 == 1) {
        const unsigned port = (unsigned)(id - ID_PORT) / 10;
        const uint32_t type = s->edit.ports[port].type;
        if (type == MX_END_SERIAL && s->serial_count) {
            options o;
            memset(&o, 0, sizeof o);
            o.title = port_title(port);
            for (unsigned i = 0; i < s->serial_count; ++i) {
                add_option(&o, s->serial_names[i], 1);
                if (!strcmp(s->serial_names[i], s->edit.ports[port].path)) o.current = (int)i;
            }
            open_menu(s, TAG_PORT_PICK, (int)port, MX_TXT(V_PORT_SERIAL), &o);
        } else open_input(s, TAG_PORT_TARGET, (int)port, port_title(port),
                   type == MX_END_SERIAL ? MX_TXT(PROMPT_SERIAL) : type == MX_END_PIPE ? MX_TXT(PROMPT_PIPE)
                   : type == MX_END_TCP ? MX_TXT(PROMPT_TCP)
                   : type == MX_END_TCP_LISTEN ? MX_TXT(PROMPT_TCP_LISTEN)
                   : MX_TXT(PROMPT_OUTPUT),
                   s->edit.ports[port].path, sizeof s->edit.ports[port].path - 1);
    } else if (id >= ID_STICK && id < ID_STICK + 20 && (id - ID_STICK) % 10 == STICK_CALIBRATE) {
        const mx_stick_config *st = &s->edit.sticks[(id - ID_STICK) / 10];
        if (st->device == MX_STICK_CONTROLLER && st->pad >= 1 && st->pad <= s->pad_count) {
            s->modal = MODAL_CALIBRATE;
            s->arg = (id - ID_STICK) / 10;
            s->cal_step = 0;
            s->cal_pad = (int)st->pad - 1;
            s->cal_deviation = 0;
        }
    }
}

static void change(mx_setup *s, int id, int direction) {
    if (is_toggle(id)) {
        activate(s, id);
        return;
    }
    if (direction) cycle(s, id, direction);
}

/* --- pop-up results ------------------------------------------------------------------------------------------ */

static void picked_image(mx_setup *s, unsigned drive, const char *path) {
    char stored[MX_CONFIG_PATH], folder[MX_CONFIG_PATH];
    config_path(s, path, stored, sizeof stored);
    if (fs_parent(NULL, path, folder, sizeof folder)) snprintf(s->last_folder, sizeof s->last_folder, "%s", folder);
    set_media(s, drive, stored, 0);
}

/* Where an image picker starts for a ROM: the folder of the image in the socket, the last folder used, roms/
 * beside the configuration, the configuration's folder. */
static void rom_folder(const mx_setup *s, unsigned socket, char *out, size_t capacity) {
    char path[MX_CONFIG_PATH], base[MX_CONFIG_PATH];
    path[0] = 0;
    if (s->edit.rom[socket][0]) folder_of(s->edit.rom[socket], path, sizeof path);
    else if (s->last_folder[0]) snprintf(path, sizeof path, "%s", s->last_folder);
    if (!path[0]) {
        folder_of(s->edit.source, base, sizeof base);
        char roms[MX_CONFIG_PATH];
        snprintf(roms, sizeof roms, "%sroms", base);
        snprintf(path, sizeof path, "%s", mx_host_folder_exists(roms) ? roms : base[0] ? base : ".");
    }
    if (!mx_host_full_path(path, out, capacity)) snprintf(out, capacity, "%s", path);
    trim_folder(out);
}

static void picked_rom(mx_setup *s, unsigned socket, const char *path) {
    char folder[MX_CONFIG_PATH];
    if (fs_parent(NULL, path, folder, sizeof folder)) snprintf(s->last_folder, sizeof s->last_folder, "%s", folder);
    config_path(s, path, s->edit.rom[socket], sizeof s->edit.rom[socket]);
}

static void menu_done(mx_setup *s, int tag, int arg, int index) {
    if (tag == TAG_ROM_MENU) {
        if (index == 0) s->edit.rom[arg][0] = 0;
        else {
            char folder[MX_CONFIG_PATH], title[64];
            mx_text_format(title, sizeof title, MX_T_PICK_ROM_TITLE, (unsigned)mx_config_rom_address((unsigned)arg));
            rom_folder(s, (unsigned)arg, folder, sizeof folder);
            open_picker(s, TAG_PICK_ROM, arg, title, folder, "");
        }
    } else if (tag == TAG_CHOICE) apply_option(s, arg, index);
    else if (tag == TAG_PORT_PICK) {
        if (index >= 0 && (unsigned)index < s->serial_count)
            copy_text(s->edit.ports[arg].path, sizeof s->edit.ports[arg].path, s->serial_names[index]);
    }
    else if (tag == TAG_HARD_SIZE) {
        const mx_hdc_type *t = mx_hdc_offered((unsigned)index);
        if (!t) return;
        char folder[MX_CONFIG_PATH], name[40], path[MX_CONFIG_PATH];
        hard_folder(s, (unsigned)arg, folder, sizeof folder);
        snprintf(name, sizeof name, "fixed-%umb.img", t->megabytes);
        fs_join(NULL, folder, name, path, sizeof path);
        s->blank_index = index;
        open_input(s, TAG_HARD_NAME, arg, MX_TXT(D_NEW_HARD), MX_TXT(D_HARD_NAME), path, MX_CONFIG_PATH - 1);
    }
    else if (tag == TAG_BLANK_SIZE) {
        char path[MX_CONFIG_PATH];
        blank_name(s, (unsigned)arg, BLANK_KIB[index], path, sizeof path);
        s->blank_index = index;
        open_input(s, TAG_BLANK_NAME, arg, MX_TXT(D_NEW_BLANK), MX_TXT(D_BLANK_NAME), path, MX_CONFIG_PATH - 1);
    }
}

static void create_blank(mx_setup *s, unsigned drive, unsigned kib, const char *path) {
    if (!write_blank(path, kib)) { note(s, MX_TXT(D_FILE_NOT_WRITTEN), path); return; }
    char text[MX_CONFIG_PATH + 80];
    mx_text_format(text, sizeof text, MX_T_D_BLANK_WRITTEN, kib, 'A' + (int)drive);
    char folder[MX_CONFIG_PATH], full[MX_CONFIG_PATH];
    snprintf(full, sizeof full, "%s", path);
    mx_host_full_path(path, full, sizeof full);
    if (fs_parent(NULL, full, folder, sizeof folder)) snprintf(s->last_folder, sizeof s->last_folder, "%s", folder);
    snprintf(s->blank_path, sizeof s->blank_path, "%s", full);
    open_dialog(s, TAG_INSERT_BLANK, (int)drive, MX_TXT(D_NEW_BLANK), text, MX_TXT(D_BLANK_BUTTONS), 0, 1);
}

/* A new blank fixed disk: a file of the size that reads as zero (sparse where the host can), then the question. */
static void create_hard(mx_setup *s, unsigned n, const char *path) {
    const mx_hdc_type *t = mx_hdc_offered((unsigned)s->blank_index);
    char error[256] = "";
    if (!t || !mx_host_file_create(path, mx_hdc_image_size(t->cylinders, t->heads), 1, error, sizeof error)) {
        note(s, MX_TXT(D_FILE_NOT_WRITTEN), path);
        return;
    }
    char text[MX_CONFIG_PATH + 80], folder[MX_CONFIG_PATH], full[MX_CONFIG_PATH];
    mx_text_format(text, sizeof text, MX_T_D_HARD_WRITTEN, t->megabytes, n);
    snprintf(full, sizeof full, "%s", path);
    mx_host_full_path(path, full, sizeof full);
    if (fs_parent(NULL, full, folder, sizeof folder)) snprintf(s->last_folder, sizeof s->last_folder, "%s", folder);
    snprintf(s->blank_path, sizeof s->blank_path, "%s", full);
    open_dialog(s, TAG_HARD_ATTACH, (int)n, MX_TXT(D_NEW_HARD), text, MX_TXT(D_HARD_BUTTONS), 0, 1);
}

static void dialog_done(mx_setup *s, int tag, int arg, int result) {
    switch (tag) {
    case TAG_LEAVE: if (result == 0) finish(s); break;
    case TAG_RESET: if (result == 0) { s->reset = MX_SETUP_RESET_WARM; finish(s); } break;
    case TAG_POWER: if (result == 0) { s->reset = MX_SETUP_RESET_POWER; finish(s); } break;
    case TAG_SAVE_ERROR: s->want_exit = 0; s->done = 1; break;
    case TAG_OVERWRITE:
        if (result == 0) create_blank(s, (unsigned)arg, BLANK_KIB[s->blank_index], s->blank_path);
        break;
    case TAG_HARD_OVERWRITE:
        if (result == 0) create_hard(s, (unsigned)arg, s->blank_path);
        break;
    case TAG_HARD_ATTACH:
        if (result == 0) {
            char stored[MX_CONFIG_PATH];
            config_path(s, s->blank_path, stored, sizeof stored);
            set_hard(s, (unsigned)arg, stored);
        }
        break;
    case TAG_INSERT_BLANK:
        if (result == 0) {
            char stored[MX_CONFIG_PATH];
            config_path(s, s->blank_path, stored, sizeof stored);
            set_media(s, (unsigned)arg, stored, 0);
        }
        break;
    default: break;
    }
}

static void input_done(mx_setup *s, int tag, int arg) {
    if (tag == TAG_PORT_TARGET) {
        if (!s->input.length) { note(s, MX_TXT(D_EMPTY_NAME), MX_TXT(D_TARGET_NEEDS_NAME)); return; }
        static const int problems[] = {0, MX_T_PORT_PROBLEM_HOST_PORT, MX_T_PORT_PROBLEM_TCP_PORT, MX_T_PORT_PROBLEM_LISTEN, MX_T_PORT_PROBLEM_EMPTY};
        const int problem = mx_config_port_problem(s->edit.ports[arg].type, s->input.text);
        if (problem) { note(s, MX_TXT(D_CANNOT_USE_THAT), mx_t(problems[problem])); return; }
        if (s->edit.ports[arg].type == MX_END_OUTPUT) output_path(s, s->input.text, s->edit.ports[arg].path, sizeof s->edit.ports[arg].path);
        else snprintf(s->edit.ports[arg].path, sizeof s->edit.ports[arg].path, "%s", s->input.text);
    } else if (tag == TAG_HARD_NAME) {
        if (!s->input.length) { note(s, MX_TXT(D_EMPTY_NAME), MX_TXT(D_IMAGE_NEEDS_NAME)); return; }
        uint64_t size;
        snprintf(s->blank_path, sizeof s->blank_path, "%s", s->input.text);
        if (mx_host_file_size(s->blank_path, &size))
            open_dialog(s, TAG_HARD_OVERWRITE, arg, MX_TXT(D_FILE_EXISTS), MX_TXT(D_HARD_EXISTS_TEXT), MX_TXT(D_FILE_EXISTS_BUTTONS), 1, 1);
        else create_hard(s, (unsigned)arg, s->blank_path);
    } else if (tag == TAG_BLANK_NAME) {
        if (!s->input.length) { note(s, MX_TXT(D_EMPTY_NAME), MX_TXT(D_IMAGE_NEEDS_NAME)); return; }
        uint64_t size;
        snprintf(s->blank_path, sizeof s->blank_path, "%s", s->input.text);
        if (mx_host_file_size(s->blank_path, &size)) open_dialog(s, TAG_OVERWRITE, arg, MX_TXT(D_FILE_EXISTS), MX_TXT(D_FILE_EXISTS_TEXT), MX_TXT(D_FILE_EXISTS_BUTTONS), 1, 1);
        else create_blank(s, (unsigned)arg, BLANK_KIB[s->blank_index], s->blank_path);
    }
}

/* --- the calibration ------------------------------------------------------------------------------------------- */

static int percent(int raw) { return (int)((long)raw * 100 / 32767); }
static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void calibration_axes(mx_setup *s) {
    if (s->modal != MODAL_CALIBRATE) return;
    const mx_stick_config *st = &s->edit.sticks[s->arg];
    const int dx = abs(percent(s->pad_x[s->cal_pad]) - st->centre_x), dy = abs(percent(s->pad_y[s->cal_pad]) - st->centre_y);
    if (s->cal_step == 1) {
        if (dx > s->cal_deviation) s->cal_deviation = dx;
        if (dy > s->cal_deviation) s->cal_deviation = dy;
    }
}

static void calibrate_next(mx_setup *s) {
    mx_stick_config *st = &s->edit.sticks[s->arg];
    if (s->cal_step == 0) {
        st->centre_x = clamp(percent(s->pad_x[s->cal_pad]), -50, 50);
        st->centre_y = clamp(percent(s->pad_y[s->cal_pad]), -50, 50);
        s->cal_step = 1;
        s->cal_deviation = 0;
    } else {
        st->range = clamp(s->cal_deviation, 50, 100);
        close_modal(s);
    }
}

/* --- events ---------------------------------------------------------------------------------------------------------- */

static void modal_event(mx_setup *s, mx_tui_event e) {
    switch (s->modal) {
    case MODAL_MENU: {
        const mx_tui_result r = mx_tui_list_event(&s->menu, e);
        if (r == MX_TUI_ACTIVATE) {
            const int tag = s->tag, arg = s->arg, index = (int)s->menu.selected;
            close_modal(s);
            menu_done(s, tag, arg, index);
        } else if (r == MX_TUI_CANCEL) close_modal(s);
        break;
    }
    case MODAL_DIALOG: {
        if (mx_tui_dialog_event(&s->dialog, e) == MX_TUI_ACTIVATE) {
            const int tag = s->tag, arg = s->arg, result = s->dialog.result;
            close_modal(s);
            dialog_done(s, tag, arg, result);
        }
        break;
    }
    case MODAL_INPUT: {
        const mx_tui_result r = mx_tui_input_event(&s->input, e);
        if (r == MX_TUI_ACTIVATE) {
            const int tag = s->tag, arg = s->arg;
            s->modal = MODAL_NONE;
            input_done(s, tag, arg);
        } else if (r == MX_TUI_CANCEL) close_modal(s);
        break;
    }
    case MODAL_PICKER: {
        const mx_tui_result r = mx_tui_picker_event(&s->picker, e);
        if (r == MX_TUI_ACTIVATE) {
            const int arg = s->arg, tag = s->tag;
            char path[MX_TUI_PATH];
            snprintf(path, sizeof path, "%s", s->picker.picked);
            close_modal(s);
            if (tag == TAG_PICK_ROM) picked_rom(s, (unsigned)arg, path);
            else if (tag == TAG_PICK_HARD) {
                char stored[MX_CONFIG_PATH], folder[MX_CONFIG_PATH];
                config_path(s, path, stored, sizeof stored);
                if (fs_parent(NULL, path, folder, sizeof folder)) snprintf(s->last_folder, sizeof s->last_folder, "%s", folder);
                set_hard(s, (unsigned)arg, stored);
            } else picked_image(s, (unsigned)arg, path);
        } else if (r == MX_TUI_CANCEL) close_modal(s);
        break;
    }
    case MODAL_CALIBRATE:
        if (e.key == MX_KEY_ENTER) calibrate_next(s);
        else if (e.key == MX_KEY_ESCAPE) close_modal(s);
        break;
    case MODAL_KEYS:
        if (s->keys_wait) {             /* the next host key is the answer (mx_setup_host_key); Escape of a controller or XT keyboard gives up */
            if (e.key == MX_KEY_ESCAPE) { s->keys_wait = 0; s->keys_message[0] = 0; }
            break;
        }
        switch (mx_tui_list_event(&s->keys_list, e)) {
        case MX_TUI_ACTIVATE: s->keys_wait = 1; s->keys_message[0] = 0; break;
        case MX_TUI_CANCEL: close_modal(s); break;
        default: break;
        }
        break;
    default: break;
    }
}

int mx_setup_wants_host_key(const mx_setup *s) { return s->modal == MODAL_KEYS && s->keys_wait; }

void mx_setup_host_key(mx_setup *s, const char *name, unsigned modifiers) {
    if (!mx_setup_wants_host_key(s)) return;
    if (!strcmp(name, "Escape")) { s->keys_wait = 0; s->keys_message[0] = 0; return; }
    const unsigned function = (unsigned)s->keys_list.selected;
    const char *reserved = NULL;
    unsigned other = 0;
    int problem = MX_KEY_OK;
    /* The window's keys that are a key with a modifier: Alt+Enter is full screen, Ctrl+F10 lets the captured mouse go. */
    if ((modifiers & MX_HOSTKEY_ALT) && !strcmp(name, "Return")) { problem = MX_KEY_RESERVED; reserved = "Alt+Enter"; }
    else if ((modifiers & MX_HOSTKEY_CTRL) && !strcmp(name, "F10")) { problem = MX_KEY_RESERVED; reserved = "Ctrl+F10"; }
    else problem = function < MX_CONFIG_STAND_KEYS ? mx_config_stand_key_problem(&s->edit, function, name, &reserved, &other) : MX_KEY_BAD;
    if (problem == MX_KEY_OK) {
        snprintf(s->edit.stand_keys[function], sizeof s->edit.stand_keys[function], "%s", name);
        s->keys_wait = 0;
        s->keys_message[0] = 0;
    } else if (problem == MX_KEY_RESERVED) mx_text_format(s->keys_message, sizeof s->keys_message, MX_T_D_KEY_RESERVED, reserved);
    else if (problem == MX_KEY_USED) mx_text_format(s->keys_message, sizeof s->keys_message, MX_T_D_KEY_USED, name, mx_t(KEY_FUNCTIONS[other]));
    else snprintf(s->keys_message, sizeof s->keys_message, "%s", MX_TXT(D_KEY_BAD));
}

const char *mx_setup_stand_directions(const mx_config *c, int capital, char *out, size_t capacity) {
    if (mx_config_stand_arrows(c)) return mx_t(capital ? MX_T_V_STICK_ARROWS_CAP : MX_T_V_STICK_ARROWS);
    snprintf(out, capacity, "%s %s %s %s", c->stand_keys[0], c->stand_keys[1], c->stand_keys[2], c->stand_keys[3]);
    return out;
}

void mx_setup_event(mx_setup *s, mx_tui_event e) {
    if (s->done) return;
    if (s->modal != MODAL_NONE) {
        modal_event(s, e);
        rebuild(s);
        return;
    }
    switch (e.key) {
    case MX_KEY_TAB:
    case MX_KEY_RIGHT: set_page(s, s->page + 1); return;
    case MX_KEY_BACKTAB:
    case MX_KEY_LEFT: set_page(s, s->page - 1); return;
    default: break;
    }
    const mx_tui_result r = mx_tui_list_event(&s->list, e);
    const int id = s->rows && s->list.selected < s->rows ? s->ids[s->list.selected] : 0;
    if (r == MX_TUI_CANCEL) { mx_setup_request_close(s); return; }
    if (r == MX_TUI_ACTIVATE) activate(s, id);
    else if (r == MX_TUI_PREV) change(s, id, -1);
    else if (r == MX_TUI_NEXT) change(s, id, 1);
    rebuild(s);
}

void mx_setup_xt_key(mx_setup *s, uint8_t code) {
    const int down = !(code & 0x80u);
    const uint8_t make = code & 0x7Fu;
    if (make == 0x2A || make == 0x36) { s->shift = down; return; }
    if (!down) return;
    mx_tui_event e;
    if (mx_tui_xt_event(make, s->shift, &e)) mx_setup_event(s, e);
}

void mx_setup_set_controllers(mx_setup *s, const char *const *names, unsigned count) {
    s->pad_count = count < MX_CONFIG_PADS ? count : MX_CONFIG_PADS;
    for (unsigned i = 0; i < s->pad_count; ++i) snprintf(s->pad_names[i], sizeof s->pad_names[i], "%s", names[i]);
    if (s->modal == MODAL_CALIBRATE && (unsigned)s->cal_pad >= s->pad_count) close_modal(s);
    rebuild(s);
}

void mx_setup_set_serial_ports(mx_setup *s, const char *const *names, unsigned count) {
    s->serial_given = 1;
    s->serial_count = count < MAX_SERIAL ? count : MAX_SERIAL;
    for (unsigned i = 0; i < s->serial_count; ++i) snprintf(s->serial_names[i], sizeof s->serial_names[i], "%s", names[i]);
    rebuild(s);
}

void mx_setup_pad_axes(mx_setup *s, unsigned index, int x, int y) {
    if (index >= MX_CONFIG_PADS) return;
    s->pad_x[index] = x;
    s->pad_y[index] = y;
    calibration_axes(s);
}

/* --- drawing ------------------------------------------------------------------------------------------------------------ */

/* The bar of the keys along the bottom: (key, what it does) pairs of the table's texts, ended by -1. */
static void keys_bar(mx_tui *t, const int *pairs) {
    const uint8_t bar = MX_ATTR(MX_BLACK, MX_LIGHT_GRAY), key = MX_ATTR(MX_RED, MX_LIGHT_GRAY);
    mx_tui_fill(t, mx_tui_rect_make(MX_TUI_ROWS - 1, 0, 1, MX_TUI_COLS), ' ', bar);
    int col = 1;
    for (; pairs[0] >= 0; pairs += 2) {
        col += mx_tui_put(t, MX_TUI_ROWS - 1, col, key, mx_t(pairs[0])) + 1;
        col += mx_tui_put(t, MX_TUI_ROWS - 1, col, bar, mx_t(pairs[1])) + 2;
    }
}

static void draw_menu(mx_setup *s) {
    mx_tui *t = &s->tui;
    const mx_tui_theme *th = mx_tui_popup_theme();
    int width = (int)strlen(s->menu_title) + 6;
    for (size_t i = 0; i < s->menu.count; ++i) if ((int)strlen(s->menu_labels[i]) + 6 > width) width = (int)strlen(s->menu_labels[i]) + 6;
    const mx_tui_rect r = mx_tui_centered((int)s->menu.count + 2, width, POPUP_TOP, POPUP_BOTTOM);
    mx_tui_fill(t, r, ' ', th->normal);
    mx_tui_box(t, r, MX_BOX_DOUBLE, th->frame, s->menu_title);
    mx_tui_shadow(t, r);
    mx_tui_list_draw(t, &s->menu, mx_tui_rect_make(r.row + 1, r.col + 1, (int)s->menu.count, r.cols - 2), th);
}

static void draw_input(mx_setup *s) {
    mx_tui *t = &s->tui;
    const mx_tui_theme *th = mx_tui_popup_theme();
    const mx_tui_rect r = mx_tui_centered(6, 64, POPUP_TOP, POPUP_BOTTOM);
    mx_tui_fill(t, r, ' ', th->normal);
    mx_tui_box(t, r, MX_BOX_DOUBLE, th->frame, s->input_title);
    mx_tui_shadow(t, r);
    mx_tui_put_fit(t, r.row + 2, r.col + 3, r.cols - 6, th->normal, s->input_prompt, 0);
    mx_tui_input_draw(t, &s->input, r.row + 3, r.col + 3, r.cols - 6, MX_ATTR(MX_WHITE, MX_BLACK));
}

static void draw_calibrate(mx_setup *s) {
    mx_tui *t = &s->tui;
    const mx_tui_theme *th = mx_tui_popup_theme();
    const mx_tui_rect r = mx_tui_centered(17, 50, POPUP_TOP, POPUP_BOTTOM);
    char title[64];
    mx_text_format(title, sizeof title, MX_T_CAL_TITLE, s->arg + 1);
    mx_tui_fill(t, r, ' ', th->normal);
    mx_tui_box(t, r, MX_BOX_DOUBLE, th->frame, title);
    mx_tui_shadow(t, r);
    {   /* two centred lines, broken where the text says */
        const char *instruction = mx_t(s->cal_step == 0 ? MX_T_CAL_CENTRE : MX_T_CAL_RANGE), *brk = strchr(instruction, '\n');
        char first[96], second[96];
        snprintf(first, sizeof first, "%.*s", brk ? (int)(brk - instruction) : (int)strlen(instruction), instruction);
        snprintf(second, sizeof second, "%s", brk ? brk + 1 : "");
        mx_tui_put_fit(t, r.row + 2, r.col + 3, r.cols - 6, th->normal, first, MX_FIT_CENTER);
        mx_tui_put_fit(t, r.row + 3, r.col + 3, r.cols - 6, th->normal, second, MX_FIT_CENTER);
    }
    /* the stick's place in a small frame */
    const mx_tui_rect grid = mx_tui_rect_make(r.row + 5, r.col + (r.cols - 25) / 2, 9, 25);
    mx_tui_box(t, grid, MX_BOX_SINGLE, th->frame, NULL);
    const int cx = grid.col + 1 + 11 + percent(s->pad_x[s->cal_pad]) * 11 / 100, cy = grid.row + 1 + 3 + percent(s->pad_y[s->cal_pad]) * 3 / 100;
    mx_tui_fill(t, mx_tui_rect_make(clamp(cy, grid.row + 1, grid.row + 7), clamp(cx, grid.col + 1, grid.col + 23), 1, 1), t->ascii ? 'O' : 0x04, MX_ATTR(MX_RED, MX_LIGHT_GRAY));
    char text[64];
    if (s->cal_step == 0) snprintf(text, sizeof text, "x %+d %%   y %+d %%", percent(s->pad_x[s->cal_pad]), percent(s->pad_y[s->cal_pad]));
    else mx_text_format(text, sizeof text, MX_T_CAL_REACH, s->cal_deviation);
    mx_tui_put_fit(t, r.row + 15, r.col + 3, r.cols - 6, th->normal, text, MX_FIT_CENTER);
}

/* The six functions of the keyboard joystick and the key each has; the one waiting for its key says so. */
static void draw_keys(mx_setup *s) {
    mx_tui *t = &s->tui;
    const mx_tui_theme *th = mx_tui_popup_theme();
    keys_refresh(s);
    const mx_tui_rect r = mx_tui_centered(13, 58, POPUP_TOP, POPUP_BOTTOM);
    mx_tui_fill(t, r, ' ', th->normal);
    mx_tui_box(t, r, MX_BOX_DOUBLE, th->frame, MX_TXT(D_KEYS_TITLE));
    mx_tui_shadow(t, r);
    mx_tui_list_draw(t, &s->keys_list, mx_tui_rect_make(r.row + 2, r.col + 2, MX_CONFIG_STAND_KEYS, r.cols - 4), th);
    mx_tui_put_wrapped(t, r.row + 9, r.col + 3, r.cols - 6, 2, MX_ATTR(MX_RED, MX_LIGHT_GRAY), s->keys_message);
}

static void draw(mx_setup *s) {
    mx_tui *t = &s->tui;
    const mx_tui_theme *w = mx_tui_window_theme();
    const uint8_t bar = MX_ATTR(MX_BLACK, MX_LIGHT_GRAY), active = MX_ATTR(MX_WHITE, MX_BLACK);
    mx_tui_clear(t, w->normal);
    t->clipped = 0;
    t->cursor_row = -1;
    mx_tui_fill(t, mx_tui_rect_make(0, 0, 1, MX_TUI_COLS), ' ', bar);
    char title[96];
    mx_text_format(title, sizeof title, MX_T_SETUP_TITLE, s->env.version ? s->env.version : "");
    mx_tui_put(t, 0, 1, bar, title);
    mx_tui_put(t, 0, MX_TUI_COLS - 17, bar, "FEPER JUNIOR-XT");
    mx_tui_fill(t, mx_tui_rect_make(1, 0, 1, MX_TUI_COLS), ' ', MX_ATTR(MX_BLACK, MX_CYAN));
    int col = 1;
    for (int p = 0; p < MX_SETUP_PAGES; ++p) {
        char text[24];
        snprintf(text, sizeof text, " %s ", mx_t(PAGE_TITLES[p]));
        col += mx_tui_put(t, 1, col, p == s->page ? active : MX_ATTR(MX_BLACK, MX_CYAN), text) + 1;
    }
    const mx_tui_rect window = mx_tui_rect_make(2, 0, 20, MX_TUI_COLS);
    mx_tui_box(t, window, MX_BOX_DOUBLE, w->frame, mx_t(PAGE_TITLES[s->page]));
    mx_tui_list_draw(t, &s->list, mx_tui_rect_make(LIST_ROW, 1, LIST_ROWS, MX_TUI_COLS - 2), w);
    /* the hint of the selected row, then the keys */
    const char *hint = s->rows && s->list.selected < s->rows ? s->hints[s->list.selected] : "";
    mx_tui_fill(t, mx_tui_rect_make(22, 0, 2, MX_TUI_COLS), ' ', MX_ATTR(MX_LIGHT_GRAY, MX_BLACK));
    mx_tui_put_wrapped(t, 22, 1, MX_TUI_COLS - 2, 2, MX_ATTR(MX_LIGHT_GRAY, MX_BLACK), hint);
    static const int form_keys[] = {MX_T_KEY_LEFT_RIGHT, MX_T_ACT_PAGE, MX_T_KEY_UP_DOWN, MX_T_ACT_SELECT, MX_T_KEY_PAGE_UPDOWN, MX_T_ACT_CHANGE,
                                    MX_T_KEY_ENTER, MX_T_ACT_CHOOSE, MX_T_KEY_ESC, MX_T_ACT_DONE, -1};
    static const int menu_keys[] = {MX_T_KEY_UP_DOWN, MX_T_ACT_SELECT, MX_T_KEY_ENTER, MX_T_ACT_CHOOSE, MX_T_KEY_ESC, MX_T_ACT_BACK, -1};
    static const int input_keys[] = {MX_T_KEY_ENTER, MX_T_ACT_ACCEPT, MX_T_KEY_ESC, MX_T_ACT_CANCEL, -1};
    static const int picker_keys[] = {MX_T_KEY_UP_DOWN, MX_T_ACT_SELECT, MX_T_KEY_ENTER, MX_T_ACT_OPEN, MX_T_KEY_BACKSPACE, MX_T_ACT_UP_FOLDER,
                                      MX_T_KEY_ESC, MX_T_ACT_CANCEL, -1};
    static const int dialog_keys[] = {MX_T_KEY_LEFT_RIGHT, MX_T_ACT_SELECT, MX_T_KEY_ENTER, MX_T_ACT_CHOOSE, -1};
    static const int calibrate_keys[] = {MX_T_KEY_ENTER, MX_T_ACT_NEXT, MX_T_KEY_ESC, MX_T_ACT_CANCEL, -1};
    static const int keys_keys[] = {MX_T_KEY_UP_DOWN, MX_T_ACT_SELECT, MX_T_KEY_ENTER, MX_T_ACT_ASSIGN, MX_T_KEY_ESC, MX_T_ACT_BACK, -1};
    static const int wait_keys[] = {MX_T_KEY_ANY, MX_T_ACT_ASSIGN, MX_T_KEY_ESC, MX_T_ACT_CANCEL, -1};
    switch (s->modal) {
    case MODAL_MENU: draw_menu(s); keys_bar(t, menu_keys); break;
    case MODAL_DIALOG: mx_tui_dialog_draw(t, &s->dialog, POPUP_TOP, POPUP_BOTTOM); keys_bar(t, dialog_keys); break;
    case MODAL_INPUT: draw_input(s); keys_bar(t, input_keys); break;
    case MODAL_PICKER: mx_tui_picker_draw(t, &s->picker, POPUP_TOP, POPUP_BOTTOM); keys_bar(t, picker_keys); break;
    case MODAL_CALIBRATE: draw_calibrate(s); keys_bar(t, calibrate_keys); break;
    case MODAL_KEYS: draw_keys(s); keys_bar(t, s->keys_wait ? wait_keys : keys_keys); break;
    default: keys_bar(t, form_keys); break;
    }
}

void mx_setup_frame(mx_setup *s, int cursor_on, mx_frame *frame) {
    draw(s);
    mx_tui_render(&s->tui, cursor_on);
    mx_tui_frame(&s->tui, frame);
}

/* --- life cycle ----------------------------------------------------------------------------------------------------------- */

mx_setup *mx_setup_create(const mx_config *config, const mx_setup_env *env) {
    mx_setup *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->env = *env;
    s->edit = *config;
    s->saved_fullscreen = config->fullscreen;
    s->edit.fullscreen = env->fullscreen != 0;
    s->initial = s->edit;
    s->running = *config;
    s->list.form = 1;                   /* Page Up and Page Down change the value of the selected row */
    mx_text_set_font(env->font);
    mx_text_set_language(s->edit.language);
    scan_serial(s);
    mx_tui_init(&s->tui, env->font);
    rebuild(s);
    return s;
}

void mx_setup_destroy(mx_setup *s) {
    if (!s) return;
    close_modal(s);
    free(s);
}

void mx_setup_open_page(mx_setup *s, int page) {
    if (page < 0 || page >= MX_SETUP_PAGES) return;
    set_page(s, page);
}

int mx_setup_page(const mx_setup *s) { return s->page; }
const mx_tui *mx_setup_screen(const mx_setup *s) { return &s->tui; }
unsigned mx_setup_clipped(const mx_setup *s) { return s->tui.clipped; }
int mx_setup_popup(const mx_setup *s) { return s->modal != MODAL_NONE; }
void mx_setup_list_state(const mx_setup *s, size_t *count, size_t *selected, size_t *top) {
    *count = s->list.count;
    *selected = s->list.selected;
    *top = s->list.top;
}
const mx_config *mx_setup_config(const mx_setup *s) { return &s->edit; }
int mx_setup_done(const mx_setup *s) { return s->done; }
int mx_setup_exit(const mx_setup *s) { return s->exit; }

void mx_setup_request_exit(mx_setup *s) {
    if (s->done) return;
    s->want_exit = 1;
    finish(s);                          /* the settings are saved as leaving does, with no question: nothing is rebuilt */
}
int mx_setup_reset(const mx_setup *s) { return s->reset; }

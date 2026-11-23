/* mezinxt.yaml (config.h). libyaml handles the YAML syntax; this file
 * enforces the schema: unknown keys, duplicate keys, bad values and extra
 * documents are refused with a message naming what is wrong and its line. */
#include "config.h"
#include "yamledit.h"
#include "../host/host.h"

#include <yaml.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct reader {
    yaml_document_t *doc;
    char *error;
    size_t cap;
    size_t line;               /* the line being read, for messages */
    int legacy_fast;           /* machine.fast_disk as the file has it (-1: absent): the older setting of every drive's `fast` */
    unsigned fast_given;       /* the drives (bit n: diskette drive n, bit 4 + n: fixed disk n) whose own `fast` the file has */
} reader;

static int failf(reader *r, const char *format, ...) {
    if (!r->cap) return 0;
    size_t used = 0;
    if (r->line) used = (size_t)snprintf(r->error, r->cap, "line %zu: ", r->line);
    if (used < r->cap) {
        va_list args;
        va_start(args, format);
        vsnprintf(r->error + used, r->cap - used, format, args);
        va_end(args);
    }
    return 0;
}
static int fail(reader *r, const char *message) { return failf(r, "%s", message); }

static yaml_node_t *node(reader *r, int index) {
    yaml_node_t *n = r->doc ? yaml_document_get_node(r->doc, index) : NULL;
    if (n) r->line = n->start_mark.line + 1;
    return n;
}
static const char *scalar(reader *r, yaml_node_t *n) {
    if (n) r->line = n->start_mark.line + 1;
    if (!n || n->type != YAML_SCALAR_NODE || strlen((char *)n->data.scalar.value) != n->data.scalar.length) {
        fail(r, "Expected a single value");
        return NULL;
    }
    return (const char *)n->data.scalar.value;
}
static int mapping(reader *r, yaml_node_t *n) {
    if (n) r->line = n->start_mark.line + 1;
    if (!n || n->type != YAML_MAPPING_NODE) return fail(r, "Expected a mapping");
    for (yaml_node_pair_t *p = n->data.mapping.pairs.start; p < n->data.mapping.pairs.top; ++p) {
        const char *key = scalar(r, node(r, p->key));
        if (!key) return 0;
        const size_t line = r->line;
        for (yaml_node_pair_t *q = n->data.mapping.pairs.start; q < p; ++q) {
            const char *previous = scalar(r, node(r, q->key));
            if (!previous) return 0;
            if (strcmp(key, previous) == 0) {
                r->line = line;           /* the repeated key's line, not the first one's */
                return fail(r, "Duplicate configuration key");
            }
        }
    }
    return 1;
}
#define PAIRS(v, q) for (yaml_node_pair_t *q = (v)->data.mapping.pairs.start; q < (v)->data.mapping.pairs.top; ++q)

static int boolean(reader *r, yaml_node_t *n, int *out) {
    const char *s = scalar(r, n);
    if (s && strcmp(s, "true") == 0) { *out = 1; return 1; }
    if (s && strcmp(s, "false") == 0) { *out = 0; return 1; }
    return s ? fail(r, "Expected true or false") : 0;
}

/* A path: relative ones are relative to the configuration's folder (base). */
static int path_value(reader *r, const char *s, const char *base, char *out) {
    const int absolute = s[0] == '/' || s[0] == '\\' || (s[0] && s[1] == ':');
    const int length = absolute ? snprintf(out, MX_CONFIG_PATH, "%s", s) : snprintf(out, MX_CONFIG_PATH, "%s%s", base, s);
    return length >= 0 && length < MX_CONFIG_PATH ? 1 : fail(r, "Path is too long");
}

/* --- the choice values ---------------------------------------------------------------- */

typedef struct named { const char *name; uint32_t value; } named;
static const named bioses[] = {{"3.0", MX_BIOS_3_0}, {"4.0", MX_BIOS_4_0}, {"5.0", MX_BIOS_5_0}, {"5.0-alt", MX_BIOS_5_0_ALT}, {NULL, 0}};
static const named cpus[] = {{"8086", MX_CPU_8086}, {"8088", MX_CPU_8088}, {NULL, 0}};
static const named speeds[] = {{"4.77", MX_SPEED_4_77}, {"7.16", MX_SPEED_7_16}, {"9.54", MX_SPEED_9_54},
                               {"unthrottled", MX_SPEED_UNTHROTTLED}, {NULL, 0}};
static const named displays[] = {{"none", MX_DISPLAY_NONE}, {"cga40", MX_DISPLAY_CGA40}, {"cga80", MX_DISPLAY_CGA80}, {"mda", MX_DISPLAY_MDA},
                                 {NULL, 0}};
static const named looks[] = {{"plain", MX_LOOK_PLAIN}, {"crt", MX_LOOK_CRT}, {NULL, 0}};
static const named monitors[] = {{"rgbi", MX_MONITOR_RGBI}, {"rgb", MX_MONITOR_RGB}, {"mono-green", MX_MONITOR_MONO_GREEN},
                                 {NULL, 0}};
static const named aspects[] = {{"original", MX_CONFIG_ASPECT_ORIGINAL}, {"square", MX_CONFIG_ASPECT_SQUARE}, {NULL, 0}};
static const named scales[] = {{"fit", MX_CONFIG_SCALE_FIT}, {"integer", MX_CONFIG_SCALE_INTEGER}, {NULL, 0}};
static const named filters[] = {{"nearest", MX_CONFIG_FILTER_NEAREST}, {"linear", MX_CONFIG_FILTER_LINEAR},
                                {"sharp", MX_CONFIG_FILTER_SHARP}, {NULL, 0}};
static const named pc_serials[] = {{"none", MX_PCSERIAL_NONE}, {"one", MX_PCSERIAL_ONE}, {"two", MX_PCSERIAL_TWO}, {NULL, 0}};
static const named languages[] = {{"en", MX_LANG_EN}, {"ro", MX_LANG_RO}, {NULL, 0}};
static const char *const port_keys[MX_CONFIG_PORTS] = {"sio_a", "sio_b", "lpt", "com1", "com2", "lpt_mda"};
static const char *const drive_keys[MX_CONFIG_DRIVES] = {"a", "b", "c", "d"};
static const char *const rom_keys[MX_CONFIG_ROMS] = {"rom_c8000", "rom_f0000", "rom_f8000"};
static const char *const hard_keys[MX_CONFIG_HARD] = {"hd0", "hd1"};
static const char *const stand_defaults[MX_CONFIG_STAND_KEYS] = {"Up", "Down", "Left", "Right", "Left Ctrl", "Left Shift"};
const char *mx_config_hard_key(unsigned drive) { return drive < MX_CONFIG_HARD ? hard_keys[drive] : "?"; }
int mx_config_fast_active(const mx_config *c, unsigned floppy_active, unsigned hard_active) {
    for (unsigned i = 0; i < MX_CONFIG_DRIVES; ++i) if ((floppy_active >> i & 1u) && c->drives[i].fast) return 1;
    for (unsigned i = 0; i < MX_CONFIG_HARD; ++i) if ((hard_active >> i & 1u) && c->hard[i].fast) return 1;
    return 0;
}
static const uint32_t rom_addresses[MX_CONFIG_ROMS] = {0xC8000u, 0xF0000u, 0xF8000u};

static const char *name_of(const named *names, uint32_t value) {
    for (; names->name; ++names) if (names->value == value) return names->name;
    return "?";
}
const char *mx_config_bios_name(uint32_t v) { return name_of(bioses, v); }
const char *mx_config_cpu_name(uint32_t v) { return name_of(cpus, v); }
const char *mx_config_speed_name(uint32_t v) { return name_of(speeds, v); }
const char *mx_config_display_name(uint32_t v) { return name_of(displays, v); }
const char *mx_config_look_name(uint32_t v) { return name_of(looks, v); }
const char *mx_config_monitor_name(uint32_t v) { return name_of(monitors, v); }
const char *mx_config_aspect_name(uint32_t v) { return name_of(aspects, v); }
const char *mx_config_scale_name(uint32_t v) { return name_of(scales, v); }
const char *mx_config_filter_name(uint32_t v) { return name_of(filters, v); }
const char *mx_config_pc_serial_name(uint32_t v) { return name_of(pc_serials, v); }
const char *mx_config_language_name(uint32_t v) { return name_of(languages, v); }
const char *mx_config_port_key(unsigned port) { return port < MX_CONFIG_PORTS ? port_keys[port] : "?"; }
int mx_config_port_is_parallel(unsigned port) { return port == MX_CONFIG_PORT_LPT || port == MX_CONFIG_PORT_LPT_MDA; }
static const char *const stick_keys[MX_CONFIG_STICKS] = {"joystick1", "joystick2"};
const char *mx_config_rom_key(unsigned socket) { return socket < MX_CONFIG_ROMS ? rom_keys[socket] : "?"; }
uint32_t mx_config_rom_address(unsigned socket) { return socket < MX_CONFIG_ROMS ? rom_addresses[socket] : 0; }
int mx_config_ram_valid(uint32_t kb) { return kb == 64 || kb == 128 || kb == 192 || kb == 256 || kb == 640; }
const char *mx_config_stick_device_name(const mx_stick_config *stick, char *buffer, size_t capacity) {
    if (stick->device == MX_STICK_KEYBOARD) snprintf(buffer, capacity, "keyboard");
    else if (stick->device == MX_STICK_CONTROLLER) snprintf(buffer, capacity, "controller %u", (unsigned)stick->pad);
    else snprintf(buffer, capacity, "none");
    return buffer;
}

/* One of the names; the message lists them. */
static int choice(reader *r, yaml_node_t *n, const char *key, const named *names, uint32_t *out) {
    const char *s = scalar(r, n);
    if (!s) return 0;
    for (const named *p = names; p->name; ++p) if (!strcmp(s, p->name)) { *out = p->value; return 1; }
    char list[160];
    size_t used = 0;
    for (const named *p = names; p->name && used < sizeof list; ++p)
        used += (size_t)snprintf(list + used, sizeof list - used, "%s%s", p == names ? "" : p[1].name ? ", " : " or ", p->name);
    return failf(r, "%s must be %s", key, list);
}

/* --- the keyboard stand-in's keys ------------------------------------------------------------- */

static const char *const stand_keys_names[MX_CONFIG_STAND_KEYS] = {"up", "down", "left", "right", "fire1", "fire2"};
static int (*key_known)(const char *name);

const char *mx_config_stand_key_key(unsigned function) { return function < MX_CONFIG_STAND_KEYS ? stand_keys_names[function] : "?"; }
const char *mx_config_stand_key_default(unsigned function) { return function < MX_CONFIG_STAND_KEYS ? stand_defaults[function] : ""; }
void mx_config_set_key_check(int (*known)(const char *name)) { key_known = known; }

/* Key names compare without regard to case, as the host's lookup does. */
static int same_key(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

int mx_config_stand_arrows(const mx_config *c) {
    for (unsigned i = 0; i < 4; ++i) if (!same_key(c->stand_keys[i], stand_defaults[i])) return 0;
    return 1;
}

/* A name no key has, or one the window keeps. */
static int key_name_problem(const char *name, const char **reserved) {
    static const struct { const char *name, *written; } window[] = {{"F11", "F11"}, {"F12", "F12"}, {"Pause", "Pause"}, {"PrintScreen", "Print Screen"}};
    const size_t n = strlen(name);
    if (!n || n >= MX_CONFIG_KEY_NAME) return MX_KEY_BAD;
    for (const char *p = name; *p; ++p) if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) return MX_KEY_BAD;
    if (key_known && !key_known(name)) return MX_KEY_BAD;
    for (size_t i = 0; i < sizeof window / sizeof *window; ++i)
        if (same_key(name, window[i].name)) {
            if (reserved) *reserved = window[i].written;
            return MX_KEY_RESERVED;
        }
    return MX_KEY_OK;
}

int mx_config_stand_key_problem(const mx_config *c, unsigned function, const char *name, const char **reserved, unsigned *other) {
    const int problem = key_name_problem(name, reserved);
    if (problem != MX_KEY_OK) return problem;
    for (unsigned i = 0; i < MX_CONFIG_STAND_KEYS; ++i)
        if (i != function && same_key(c->stand_keys[i], name)) {
            if (other) *other = i;
            return MX_KEY_USED;
        }
    return MX_KEY_OK;
}

uint32_t mx_config_language_for_locale(const char *locale) {
    if (!locale) return MX_LANG_EN;
    const int ro = (locale[0] | 0x20) == 'r' && (locale[1] | 0x20) == 'o';
    return ro && (!locale[2] || locale[2] == '-' || locale[2] == '_' || locale[2] == '.' || locale[2] == '@') ? MX_LANG_RO : MX_LANG_EN;
}

uint32_t mx_config_default_language(void) {
    char locale[64];
    return mx_host_user_locale(locale, sizeof locale) ? mx_config_language_for_locale(locale) : MX_LANG_EN;
}

void mx_config_defaults(mx_config *c) {
    memset(c, 0, sizeof *c);
    c->language = mx_config_default_language();
    c->bios = MX_BIOS_4_0;
    c->cpu = MX_CPU_8086;
    c->speed = MX_SPEED_4_77;
    c->ram_kb = 640;
    c->floppies = 2;
    c->display = MX_DISPLAY_CGA80;
    c->look = MX_LOOK_CRT;
    c->monitor = MX_MONITOR_MONO_GREEN;
    c->aspect = MX_CONFIG_ASPECT_ORIGINAL;
    c->scale = MX_CONFIG_SCALE_FIT;
    c->filter = MX_CONFIG_FILTER_SHARP;
    c->vsync = 1;
    c->snow = 1;
    c->sound = 1;
    c->sio_adapter = 1;
    c->volume = 100;
    c->drive_sound = 1;
    c->drive_volume = 50;
    for (unsigned i = 0; i < MX_CONFIG_DRIVES; ++i) {
        c->drives[i].tracks = 40;
        c->drives[i].fast = 1;
    }
    for (unsigned i = 0; i < MX_CONFIG_HARD; ++i) c->hard[i].fast = 1;
    for (unsigned i = 0; i < MX_CONFIG_STICKS; ++i) {
        c->sticks[i].dead_zone = 10;
        c->sticks[i].range = 100;
    }
    for (unsigned i = 0; i < MX_CONFIG_STAND_KEYS; ++i) snprintf(c->stand_keys[i], sizeof c->stand_keys[i], "%s", stand_defaults[i]);
}

/* --- the sections ------------------------------------------------------------------- */

static int integer(reader *r, yaml_node_t *n, const char *key, int lo, int hi, int *out);

static int machine_section(reader *r, yaml_node_t *v, const char *base, mx_config *c) {
    if (!mapping(r, v)) return 0;
    PAIRS(v, q) {
        const char *name = scalar(r, node(r, q->key));
        yaml_node_t *x = node(r, q->value);
        int ok;
        if (!strcmp(name, "bios")) ok = choice(r, x, "machine.bios", bioses, &c->bios);
        else if (!strcmp(name, "cpu")) ok = choice(r, x, "machine.cpu", cpus, &c->cpu);
        else if (!strcmp(name, "speed")) ok = choice(r, x, "machine.speed", speeds, &c->speed);
        else if (!strcmp(name, "display")) ok = choice(r, x, "machine.display", displays, &c->display);
        else if (!strcmp(name, "fpu")) ok = boolean(r, x, &c->fpu);
        else if (!strcmp(name, "fast_disk")) ok = boolean(r, x, &r->legacy_fast);     /* retired: now each drive's `fast` (drives_section) */
        else if (!strcmp(name, "winchester")) ok = boolean(r, x, &c->winchester);
        else if (!strcmp(name, "ram_kb")) {
            const char *s = scalar(r, x);
            char *end = NULL;
            const long kb = s ? strtol(s, &end, 10) : 0;
            ok = s != NULL;
            if (ok && *s && end && !*end && mx_config_ram_valid((uint32_t)kb)) c->ram_kb = (uint32_t)kb;
            else if (ok) ok = fail(r, "machine.ram_kb must be 64, 128, 192, 256 or 640");
        } else if (!strcmp(name, "drives")) {
            int n = 0;
            ok = integer(r, x, "machine.drives", 1, 4, &n);
            if (ok) c->floppies = (uint32_t)n;
        } else {
            unsigned socket = 0;
            while (socket < MX_CONFIG_ROMS && strcmp(name, rom_keys[socket])) ++socket;
            if (socket < MX_CONFIG_ROMS) {
                const char *s = scalar(r, x);
                ok = s != NULL;
                if (ok && *s) ok = path_value(r, s, base, c->rom[socket]);
                else if (ok) c->rom[socket][0] = 0;
            } else ok = fail(r, "Unknown machine key; expected bios, cpu, speed, ram_kb, display, fpu, winchester, drives, rom_c8000, rom_f0000 or rom_f8000");
        }
        if (!ok) return 0;
    }
    return 1;
}

static int drive_config(reader *r, yaml_node_t *v, const char *base, mx_drive_config *d, unsigned fast_bit) {
    /* "a:" with nothing after it is an empty drive. */
    if (v && v->type == YAML_SCALAR_NODE && !v->data.scalar.length) return 1;
    if (!mapping(r, v)) return 0;
    PAIRS(v, q) {
        const char *name = scalar(r, node(r, q->key));
        yaml_node_t *x = node(r, q->value);
        int ok;
        if (!strcmp(name, "image")) {
            const char *s = scalar(r, x);
            ok = s != NULL;
            if (ok && *s) ok = path_value(r, s, base, d->image);
            else if (ok) d->image[0] = 0;
        } else if (!strcmp(name, "write_protect")) ok = boolean(r, x, &d->write_protect);
        else if (!strcmp(name, "fast")) {
            ok = boolean(r, x, &d->fast);
            r->fast_given |= 1u << fast_bit;
        } else if (!strcmp(name, "tracks")) {
            const char *s = scalar(r, x);
            ok = s != NULL;
            if (ok && !strcmp(s, "40")) d->tracks = 40;
            else if (ok && !strcmp(s, "80")) d->tracks = 80;
            else if (ok) ok = fail(r, "A drive's tracks must be 40 or 80");
        } else ok = fail(r, "Unknown drive key; expected image, write_protect, tracks or fast");
        if (!ok) return 0;
    }
    return 1;
}

/* A fixed disk: its image and fast access. */
static int hard_config(reader *r, yaml_node_t *v, const char *base, mx_drive_config *d, unsigned fast_bit) {
    if (v && v->type == YAML_SCALAR_NODE && !v->data.scalar.length) return 1;
    if (!mapping(r, v)) return 0;
    PAIRS(v, q) {
        const char *name = scalar(r, node(r, q->key));
        yaml_node_t *x = node(r, q->value);
        int ok;
        if (!strcmp(name, "image")) {
            const char *s = scalar(r, x);
            ok = s != NULL;
            if (ok && *s) ok = path_value(r, s, base, d->image);
            else if (ok) d->image[0] = 0;
        } else if (!strcmp(name, "fast")) {
            ok = boolean(r, x, &d->fast);
            r->fast_given |= 1u << fast_bit;
        } else ok = fail(r, "Unknown fixed disk key; expected image or fast");
        if (!ok) return 0;
    }
    return 1;
}

static int drives_section(reader *r, yaml_node_t *v, const char *base, mx_config *c) {
    if (!mapping(r, v)) return 0;
    PAIRS(v, q) {
        const char *name = scalar(r, node(r, q->key));
        yaml_node_t *x = node(r, q->value);
        int ok = 0, found = 0;
        for (unsigned i = 0; i < MX_CONFIG_DRIVES; ++i)
            if (!strcmp(name, drive_keys[i])) { found = 1; ok = drive_config(r, x, base, &c->drives[i], i); }
        for (unsigned i = 0; i < MX_CONFIG_HARD; ++i)
            if (!strcmp(name, hard_keys[i])) { found = 1; ok = hard_config(r, x, base, &c->hard[i], MX_CONFIG_DRIVES + i); }
        if (!found) ok = fail(r, "Unknown drive; expected a, b, c, d, hd0 or hd1");
        if (!ok) return 0;
    }
    return 1;
}

static int video_section(reader *r, yaml_node_t *v, mx_config *c) {
    if (!mapping(r, v)) return 0;
    PAIRS(v, q) {
        const char *name = scalar(r, node(r, q->key));
        yaml_node_t *x = node(r, q->value);
        int ok;
        if (!strcmp(name, "look")) ok = choice(r, x, "video.look", looks, &c->look);
        else if (!strcmp(name, "monitor")) ok = choice(r, x, "video.monitor", monitors, &c->monitor);
        else if (!strcmp(name, "aspect")) ok = choice(r, x, "video.aspect", aspects, &c->aspect);
        else if (!strcmp(name, "scale")) ok = choice(r, x, "video.scale", scales, &c->scale);
        else if (!strcmp(name, "filter")) ok = choice(r, x, "video.filter", filters, &c->filter);
        else if (!strcmp(name, "fullscreen")) ok = boolean(r, x, &c->fullscreen);
        else if (!strcmp(name, "vsync")) ok = boolean(r, x, &c->vsync);
        else if (!strcmp(name, "snow")) ok = boolean(r, x, &c->snow);
        else if (!strcmp(name, "drive_lights")) ok = boolean(r, x, &c->drive_lights);
        else ok = fail(r, "Unknown video key; expected look, monitor, aspect, scale, filter, fullscreen, vsync, snow or drive_lights");
        if (!ok) return 0;
    }
    return 1;
}

static int sound_section(reader *r, yaml_node_t *v, mx_config *c) {
    if (!mapping(r, v)) return 0;
    PAIRS(v, q) {
        const char *name = scalar(r, node(r, q->key));
        yaml_node_t *x = node(r, q->value);
        int ok;
        if (!strcmp(name, "enabled")) ok = boolean(r, x, &c->sound);
        else if (!strcmp(name, "volume")) ok = integer(r, x, "sound.volume", 0, 100, &c->volume);
        else if (!strcmp(name, "drives")) ok = boolean(r, x, &c->drive_sound);
        else if (!strcmp(name, "drives_volume")) ok = integer(r, x, "sound.drives_volume", 0, 100, &c->drive_volume);
        else ok = fail(r, "Unknown sound key; expected enabled, volume, drives or drives_volume");
        if (!ok) return 0;
    }
    return 1;
}

static int ui_section(reader *r, yaml_node_t *v, mx_config *c) {
    if (!mapping(r, v)) return 0;
    PAIRS(v, q) {
        const char *name = scalar(r, node(r, q->key));
        yaml_node_t *x = node(r, q->value);
        int ok;
        if (!strcmp(name, "language")) ok = choice(r, x, "ui.language", languages, &c->language);
        else ok = fail(r, "Unknown ui key; expected language");
        if (!ok) return 0;
    }
    return 1;
}

/* A TCP target: HOST:PORT (a listener: PORT or HOST:PORT), the port 1 to 65535. */
int mx_config_port_problem(uint32_t type, const char *target) {
    if (type == MX_END_TCP || type == MX_END_TCP_LISTEN) {
        const char *colon = strrchr(target, ':');
        const char *digits = colon ? colon + 1 : target;
        char *end = NULL;
        const long port = *digits ? strtol(digits, &end, 10) : 0;
        if (type == MX_END_TCP && (!colon || colon == target)) return MX_PORT_NEEDS_HOST_PORT;
        if (!*digits || *end || port < 1 || port > 65535) return MX_PORT_BAD_TCP_PORT;
        if (colon && (colon == target || memchr(target, ':', (size_t)(colon - target)))) return type == MX_END_TCP ? MX_PORT_NEEDS_HOST_PORT : MX_PORT_LISTEN_FORM;
    } else if (!*target) return MX_PORT_EMPTY;
    return MX_PORT_OK;
}

int mx_config_port_check(uint32_t type, const char *target, char *error, size_t capacity) {
    static const char *const messages[] = {"", "A TCP connection needs HOST:PORT", "The TCP port must be a number from 1 to 65535",
                                           "A TCP server takes PORT or HOST:PORT", "The port's device, pipe or file is empty"};
    const int problem = mx_config_port_problem(type, target);
    if (problem && capacity) snprintf(error, capacity, "%s", messages[problem]);
    return problem == MX_PORT_OK;
}

/* "COM3", "com12" or a device path: a host serial port without its prefix. */
static int bare_serial(const char *s) {
    if (!strncmp(s, "/dev/", 5)) return 1;
    if ((s[0] | 0x20) == 'c' && (s[1] | 0x20) == 'o' && (s[2] | 0x20) == 'm' && s[3]) {
        for (const char *d = s + 3; *d; ++d) if (*d < '0' || *d > '9') return 0;
        return 1;
    }
    return 0;
}

/* A port from its kind (serial, pipe, file or output, loopback, tcp, tcp-listen)
 * and its value; the file's two spellings of a target end here. */
static int port_set(reader *r, unsigned index, const char *kind, const char *value, const char *base, mx_port_config *p) {
    const int lpt = mx_config_port_is_parallel(index), mouse_port = index == MX_CONFIG_PORT_COM1 || index == MX_CONFIG_PORT_COM2;
    const char *expected = index == MX_CONFIG_PORT_LPT_MDA ? "ports.lpt_mda must be none or file:FILE"
                         : lpt ? "ports.lpt must be none or file:FILE"
                         : mouse_port ? "A PC serial port must be none, loopback, mouse, COMn, pipe:NAME, tcp:HOST:PORT, tcp-listen:PORT or file:FILE"
                                      : "A serial port must be none, loopback, COMn, pipe:NAME, tcp:HOST:PORT, tcp-listen:PORT or file:FILE";
    char problem[96];
    uint32_t type = MX_END_NONE;
    if (!strcmp(kind, "none")) { p->type = MX_END_NONE; p->path[0] = 0; return 1; }
    if (!strcmp(kind, "file") || !strcmp(kind, "output")) type = MX_END_OUTPUT;
    else if (!strcmp(kind, "loopback") && !*value) type = MX_END_LOOPBACK;
    else if (!strcmp(kind, "mouse") && !*value && mouse_port) type = MX_END_MOUSE;
    else if (!strcmp(kind, "serial")) type = MX_END_SERIAL;
    else if (!strcmp(kind, "pipe")) type = MX_END_PIPE;
    else if (!strcmp(kind, "tcp")) type = MX_END_TCP;
    else if (!strcmp(kind, "tcp-listen")) type = MX_END_TCP_LISTEN;
    else return fail(r, expected);
    if (lpt && type != MX_END_OUTPUT) return fail(r, expected);
    if (type == MX_END_LOOPBACK || type == MX_END_MOUSE) { p->type = type; p->path[0] = 0; return 1; }
    if (!mx_config_port_check(type, value, problem, sizeof problem)) return fail(r, problem);
    if (strlen(value) >= sizeof p->path) return fail(r, "The port's name is too long");
    p->type = type;
    if (type == MX_END_OUTPUT) return path_value(r, value, base, p->path);
    strcpy(p->path, value);
    return 1;
}

/* A port: "none", "loopback", "COM3", or "KIND:VALUE" (pipe, file, tcp, tcp-listen,
 * serial); the older mapping {serial: DEVICE}, {pipe: NAME}, {output: FILE} still reads. */
static int port_config(reader *r, yaml_node_t *v, const char *base, unsigned index, mx_port_config *p) {
    const int mouse_port = index == MX_CONFIG_PORT_COM1 || index == MX_CONFIG_PORT_COM2;
    const char *expected = index == MX_CONFIG_PORT_LPT_MDA ? "ports.lpt_mda must be none or file:FILE"
                         : mx_config_port_is_parallel(index) ? "ports.lpt must be none or file:FILE"
                         : mouse_port ? "A PC serial port must be none, loopback, mouse, COMn, pipe:NAME, tcp:HOST:PORT, tcp-listen:PORT or file:FILE"
                                      : "A serial port must be none, loopback, COMn, pipe:NAME, tcp:HOST:PORT, tcp-listen:PORT or file:FILE";
    if (v && v->type == YAML_SCALAR_NODE) {
        const char *s = scalar(r, v);
        if (!s) return 0;
        if (!*s) return port_set(r, index, "none", "", base, p);
        if (!strcmp(s, "none") || !strcmp(s, "loopback") || !strcmp(s, "mouse")) return port_set(r, index, s, "", base, p);
        if (bare_serial(s)) return port_set(r, index, "serial", s, base, p);
        const char *colon = strchr(s, ':');
        if (!colon) return fail(r, expected);
        char kind[16];
        if ((size_t)(colon - s) >= sizeof kind) return fail(r, expected);
        memcpy(kind, s, (size_t)(colon - s));
        kind[colon - s] = 0;
        return port_set(r, index, kind, colon + 1, base, p);
    }
    if (!mapping(r, v)) return 0;
    if (v->data.mapping.pairs.top - v->data.mapping.pairs.start != 1) return fail(r, expected);
    yaml_node_pair_t *q = v->data.mapping.pairs.start;
    const char *name = scalar(r, node(r, q->key));
    const char *value = scalar(r, node(r, q->value));
    if (!name || !value) return 0;
    if (!*value) return fail(r, "The port's device, pipe or file is empty");
    return port_set(r, index, name, value, base, p);
}

static int ports_section(reader *r, yaml_node_t *v, const char *base, mx_config *c) {
    if (!mapping(r, v)) return 0;
    PAIRS(v, q) {
        const char *name = scalar(r, node(r, q->key));
        yaml_node_t *x = node(r, q->value);
        int ok = 0, found = 0;
        if (!strcmp(name, "adapter")) { found = 1; ok = boolean(r, x, &c->sio_adapter); }
        else if (!strcmp(name, "pc_serial")) { found = 1; ok = choice(r, x, "ports.pc_serial", pc_serials, &c->pc_serial); }
        for (unsigned i = 0; i < MX_CONFIG_PORTS; ++i)
            if (!strcmp(name, port_keys[i])) { found = 1; ok = port_config(r, x, base, i, &c->ports[i]); }
        if (!found) ok = fail(r, "Unknown port; expected adapter, pc_serial, sio_a, sio_b, lpt, com1, com2 or lpt_mda");
        if (!ok) return 0;
    }
    return 1;
}

/* A whole number between lo and hi. */
static int integer(reader *r, yaml_node_t *n, const char *key, int lo, int hi, int *out) {
    const char *s = scalar(r, n);
    if (!s) return 0;
    char *end = NULL;
    const long v = strtol(s, &end, 10);
    if (!*s || !end || *end || v < lo || v > hi) return failf(r, "%s must be a whole number from %d to %d", key, lo, hi);
    *out = (int)v;
    return 1;
}

/* none, keyboard, or controller N. */
static int stick_device(reader *r, yaml_node_t *n, const char *key, mx_stick_config *st) {
    const char *s = scalar(r, n);
    if (!s) return 0;
    unsigned pad = 0;
    char tail = 0;
    if (!strcmp(s, "none")) { st->device = MX_STICK_NONE; st->pad = 0; return 1; }
    if (!strcmp(s, "keyboard")) { st->device = MX_STICK_KEYBOARD; st->pad = 0; return 1; }
    if (sscanf(s, "controller %u%c", &pad, &tail) == 1 && pad >= 1 && pad <= MX_CONFIG_PADS) {
        st->device = MX_STICK_CONTROLLER;
        st->pad = pad;
        return 1;
    }
    return failf(r, "%s must be none, keyboard or controller 1 to %d", key, MX_CONFIG_PADS);
}

static int stick_config(reader *r, yaml_node_t *v, unsigned index, mx_stick_config *st) {
    if (v && v->type == YAML_SCALAR_NODE && !v->data.scalar.length) return 1;
    if (!mapping(r, v)) return 0;
    PAIRS(v, q) {
        const char *name = scalar(r, node(r, q->key));
        yaml_node_t *x = node(r, q->value);
        char key[64];
        snprintf(key, sizeof key, "controllers.%s.%s", stick_keys[index], name);
        int ok;
        if (!strcmp(name, "device")) ok = stick_device(r, x, key, st);
        else if (!strcmp(name, "dead_zone")) ok = integer(r, x, key, 0, 50, &st->dead_zone);
        else if (!strcmp(name, "range")) ok = integer(r, x, key, 50, 100, &st->range);
        else if (!strcmp(name, "centre_x")) ok = integer(r, x, key, -50, 50, &st->centre_x);
        else if (!strcmp(name, "centre_y")) ok = integer(r, x, key, -50, 50, &st->centre_y);
        else ok = fail(r, "Unknown joystick key; expected device, dead_zone, range, centre_x or centre_y");
        if (!ok) return 0;
    }
    return 1;
}

/* controllers.keys: the name of a host key for each function of the stand-in. */
static int keys_config(reader *r, yaml_node_t *v, mx_config *c) {
    if (v && v->type == YAML_SCALAR_NODE && !v->data.scalar.length) return 1;
    if (!mapping(r, v)) return 0;
    PAIRS(v, q) {
        const char *name = scalar(r, node(r, q->key));
        yaml_node_t *x = node(r, q->value);
        unsigned i = 0;
        while (i < MX_CONFIG_STAND_KEYS && strcmp(name, stand_keys_names[i])) ++i;
        if (i == MX_CONFIG_STAND_KEYS) return fail(r, "Unknown key function; expected up, down, left, right, fire1 or fire2");
        const char *value = scalar(r, x), *reserved = NULL;
        if (!value) return 0;
        const int problem = key_name_problem(value, &reserved);
        if (problem == MX_KEY_BAD) return failf(r, "controllers.keys.%s must be the name of a host key, as --hostkey takes it (Up, Left Ctrl, A, Space)", name);
        if (problem == MX_KEY_RESERVED) return failf(r, "controllers.keys.%s: %s belongs to the window; choose another key", name, reserved);
        snprintf(c->stand_keys[i], sizeof c->stand_keys[i], "%s", value);
    }
    return 1;
}

static int controllers_section(reader *r, yaml_node_t *v, mx_config *c) {
    if (!mapping(r, v)) return 0;
    PAIRS(v, q) {
        const char *name = scalar(r, node(r, q->key));
        yaml_node_t *x = node(r, q->value);
        int ok = 0, found = 0;
        if (!strcmp(name, "adapter")) { found = 1; ok = boolean(r, x, &c->game_adapter); }
        else if (!strcmp(name, "light_pen")) { found = 1; ok = boolean(r, x, &c->light_pen); }
        else if (!strcmp(name, "keys")) { found = 1; ok = keys_config(r, x, c); }
        for (unsigned i = 0; i < MX_CONFIG_STICKS; ++i)
            if (!strcmp(name, stick_keys[i])) { found = 1; ok = stick_config(r, x, i, &c->sticks[i]); }
        if (!found) ok = fail(r, "Unknown controller; expected adapter, light_pen, keys, joystick1 or joystick2");
        if (!ok) return 0;
    }
    return 1;
}

/* The folder part of a path, with its separator ("" for none). */
static void folder_of(const char *path, char *out) {
    snprintf(out, MX_CONFIG_PATH, "%s", path);
    char *slash = strrchr(out, '/'), *backslash = strrchr(out, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
    if (slash) slash[1] = 0; else out[0] = 0;
}

/* Two functions of the keyboard stand-in with the same key, as a message; NULL when each has a key of its own. */
static const char *stand_key_clash(const mx_config *c, char *buffer, size_t capacity) {
    for (unsigned i = 0; i < MX_CONFIG_STAND_KEYS; ++i)
        for (unsigned j = i + 1; j < MX_CONFIG_STAND_KEYS; ++j)
            if (same_key(c->stand_keys[i], c->stand_keys[j])) {
                snprintf(buffer, capacity, "controllers.keys.%s and controllers.keys.%s are both %s: each needs a key of its own",
                         stand_keys_names[i], stand_keys_names[j], c->stand_keys[i]);
                return buffer;
            }
    return NULL;
}

int mx_config_check(const mx_config *c, char *error, size_t capacity) {
    const char *msg = NULL;
    char keys_msg[160];
    if (c->sio_adapter && c->pc_serial != MX_PCSERIAL_NONE)
        msg = "ports.pc_serial: COM1 of the 8250 adapter and the SIO adapter both drive IRQ 4; set ports.adapter to false to fit the 8250 adapter";
    else if (c->ports[MX_CONFIG_PORT_COM1].type == MX_END_MOUSE && c->ports[MX_CONFIG_PORT_COM2].type == MX_END_MOUSE)
        msg = "ports.com1 and ports.com2: the host has one mouse, so only one port can be mouse";
    else if (c->winchester && c->rom[0][0])
        msg = "machine.rom_c8000: the Winchester coupler's ROM is at C8000h; put the image in rom_f0000 or rom_f8000, or set machine.winchester to false";
    else if (c->hard[1].image[0] && !c->hard[0].image[0])
        msg = "drives.hd1: the second fixed disk needs a first one (drives.hd0)";
    else msg = stand_key_clash(c, keys_msg, sizeof keys_msg);
    if (msg && capacity) snprintf(error, capacity, "%s", msg);
    return msg == NULL;
}

int mx_config_mouse_attached(const mx_config *c) {
    return (c->pc_serial >= MX_PCSERIAL_ONE && c->ports[MX_CONFIG_PORT_COM1].type == MX_END_MOUSE) ||
           (c->pc_serial >= MX_PCSERIAL_TWO && c->ports[MX_CONFIG_PORT_COM2].type == MX_END_MOUSE);
}

int mx_config_parse(const char *text, size_t length, const char *source, mx_config *out, char *error, size_t capacity) {
    yaml_parser_t parser;
    yaml_document_t doc;
    reader r;
    memset(&r, 0, sizeof r);
    r.error = error;
    r.cap = capacity;
    r.legacy_fast = -1;
    if (capacity) error[0] = 0;
    if (strlen(source) >= MX_CONFIG_PATH) return fail(&r, "Configuration path is too long");
    if (!yaml_parser_initialize(&parser)) return fail(&r, "Cannot allocate the YAML parser");
    yaml_parser_set_input_string(&parser, (const unsigned char *)text, length);
    if (!yaml_parser_load(&parser, &doc)) {
        if (capacity) snprintf(error, capacity, "line %zu: %s", parser.problem_mark.line + 1, parser.problem ? parser.problem : "YAML syntax error");
        yaml_parser_delete(&parser);
        return 0;
    }
    r.doc = &doc;
    char base[MX_CONFIG_PATH];
    folder_of(source, base);
    mx_config c;
    mx_config_defaults(&c);
    snprintf(c.source, sizeof c.source, "%s", source);
    yaml_node_t *root = yaml_document_get_root_node(&doc);
    int ok = 1;
    /* An empty file is all defaults. */
    if (root && !(root->type == YAML_SCALAR_NODE && !root->data.scalar.length)) {
        ok = mapping(&r, root);
        if (ok) PAIRS(root, p) {
            const char *key = scalar(&r, node(&r, p->key));
            yaml_node_t *v = node(&r, p->value);
            if (!key) { ok = 0; break; }
            if (!strcmp(key, "machine")) ok = machine_section(&r, v, base, &c);
            else if (!strcmp(key, "drives")) ok = drives_section(&r, v, base, &c);
            else if (!strcmp(key, "video")) ok = video_section(&r, v, &c);
            else if (!strcmp(key, "sound")) ok = sound_section(&r, v, &c);
            else if (!strcmp(key, "ports")) ok = ports_section(&r, v, base, &c);
            else if (!strcmp(key, "controllers")) ok = controllers_section(&r, v, &c);
            else if (!strcmp(key, "ui")) ok = ui_section(&r, v, &c);
            else ok = fail(&r, "Unknown section; expected machine, drives, video, sound, ports, controllers or ui");
            if (!ok) break;
        }
    }
    yaml_document_delete(&doc);
    /* machine.fast_disk, retired, is every drive's `fast` unless the file gives the drive its own. */
    if (ok && r.legacy_fast >= 0) {
        for (unsigned i = 0; i < MX_CONFIG_DRIVES; ++i) if (!(r.fast_given >> i & 1u)) c.drives[i].fast = r.legacy_fast;
        for (unsigned i = 0; i < MX_CONFIG_HARD; ++i) if (!(r.fast_given >> (MX_CONFIG_DRIVES + i) & 1u)) c.hard[i].fast = r.legacy_fast;
    }
    if (ok) {
        r.doc = NULL;
        r.line = 0;
        if (!yaml_parser_load(&parser, &doc)) ok = fail(&r, "Invalid trailing YAML document");
        else {
            if (yaml_document_get_root_node(&doc)) ok = fail(&r, "Only one YAML document is allowed");
            yaml_document_delete(&doc);
        }
    }
    yaml_parser_delete(&parser);
    if (ok) {
        r.line = 0;
        if (capacity) error[0] = 0;
        ok = mx_config_check(&c, error, capacity);
    }
    if (ok) *out = c;
    return ok;
}

int mx_config_load(const char *path, mx_config *out, char *error, size_t capacity) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (capacity) snprintf(error, capacity, "Cannot open the configuration");
        return 0;
    }
    char *text = NULL;
    long size = -1;
    if (!fseek(f, 0, SEEK_END)) size = ftell(f);
    int ok = size >= 0 && size < 1024 * 1024 && !fseek(f, 0, SEEK_SET) && (text = malloc((size_t)size + 1)) != NULL &&
             fread(text, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok) {
        free(text);
        if (capacity) snprintf(error, capacity, "Cannot read the configuration");
        return 0;
    }
    text[size] = 0;
    ok = mx_config_parse(text, (size_t)size, path, out, error, capacity);
    free(text);
    return ok;
}

/* --- writing back ----------------------------------------------------------------- */

/* A key set to a scalar unless it already holds it (its quoting then
 * stays as the user wrote it). mode 1: a path (single quotes keep its
 * backslashes); mode 2: written as it is (true, false, numbers). */
static int set_scalar(struct mx_ydoc *doc, const char *const *path, size_t depth, const char *value, int mode) {
    char now[MX_CONFIG_PATH * 2 + 8], quoted[MX_CONFIG_PATH * 2 + 8];
    if (mx_ydoc_get(doc, path, depth, now, sizeof now)) {
        if (!strcmp(now, value)) return 1;
    } else if (!*value && mx_ydoc_has(doc, path, depth)) return 1;     /* "key:" with nothing: empty already */
    if (!*value) return mx_ydoc_set(doc, path, depth, "\"\"");
    if (mode == 2) return mx_ydoc_set(doc, path, depth, value);
    return mx_ydoc_set(doc, path, depth, mode == 1 ? mx_ydoc_quote_path(value, quoted, sizeof quoted) : mx_ydoc_quote(value, quoted, sizeof quoted));
}
static int set(struct mx_ydoc *doc, const char *section, const char *key, const char *value) {
    const char *path[2] = {section, key};
    return set_scalar(doc, path, 2, value, 0);
}
static int set_bool(struct mx_ydoc *doc, const char *section, const char *key, int value) {
    const char *path[2] = {section, key};
    return set_scalar(doc, path, 2, value ? "true" : "false", 2);
}
/* A path as the file holds it: relative to the configuration's folder when
 * it lies inside it. */
static const char *stored_path(const mx_config *c, const char *path) {
    char base[MX_CONFIG_PATH];
    folder_of(c->source, base);
    const size_t n = strlen(base);
    return n && !strncmp(path, base, n) ? path + n : path;
}

const char *mx_config_port_text(const mx_config *c, const mx_port_config *p, char *buffer, size_t capacity) {
    switch (p->type) {
    case MX_END_LOOPBACK: snprintf(buffer, capacity, "loopback"); break;
    case MX_END_MOUSE: snprintf(buffer, capacity, "mouse"); break;
    case MX_END_SERIAL: snprintf(buffer, capacity, bare_serial(p->path) ? "%s" : "serial:%s", p->path); break;
    case MX_END_PIPE: snprintf(buffer, capacity, "pipe:%s", p->path); break;
    case MX_END_OUTPUT: snprintf(buffer, capacity, "file:%s", stored_path(c, p->path)); break;
    case MX_END_TCP: snprintf(buffer, capacity, "tcp:%s", p->path); break;
    case MX_END_TCP_LISTEN: snprintf(buffer, capacity, "tcp-listen:%s", p->path); break;
    default: snprintf(buffer, capacity, "none"); break;
    }
    return buffer;
}

/* A setting that is written only when it is not its default or the file names it already: a file
 * of an older template is not filled with keys it never had. */
static int set_unless_default(struct mx_ydoc *doc, const char *const *path, size_t depth, const char *value, int mode, int is_default) {
    if (is_default && !mx_ydoc_has(doc, path, depth)) return 1;
    return set_scalar(doc, path, depth, value, mode);
}

int mx_config_store(const mx_config *c, struct mx_ydoc *doc) {
    char count[16];
    const char *ram[2] = {"machine", "ram_kb"};
    snprintf(count, sizeof count, "%u", (unsigned)c->ram_kb);
    int ok = set(doc, "machine", "bios", mx_config_bios_name(c->bios)) && set(doc, "machine", "cpu", mx_config_cpu_name(c->cpu)) &&
             set(doc, "machine", "speed", mx_config_speed_name(c->speed)) && set_scalar(doc, ram, 2, count, 2) &&
             set(doc, "machine", "display", mx_config_display_name(c->display)) && set_bool(doc, "machine", "fpu", c->fpu) &&
             mx_ydoc_remove(doc, (const char *const[2]){"machine", "fast_disk"}, 2);      /* retired: each drive has its own `fast` */
    if (ok) {
        const char *winchester[2] = {"machine", "winchester"};
        ok = set_unless_default(doc, winchester, 2, c->winchester ? "true" : "false", 2, !c->winchester);
    }
    if (ok) {
        const char *floppies[2] = {"machine", "drives"};
        snprintf(count, sizeof count, "%u", (unsigned)c->floppies);
        ok = set_unless_default(doc, floppies, 2, count, 2, c->floppies == 2);
    }
    for (unsigned i = 0; ok && i < MX_CONFIG_ROMS; ++i) {
        const char *path[2] = {"machine", rom_keys[i]};
        ok = set_unless_default(doc, path, 2, c->rom[i][0] ? stored_path(c, c->rom[i]) : "", 1, !c->rom[i][0]);
    }
    for (unsigned i = 0; ok && i < MX_CONFIG_DRIVES; ++i) {
        const char *image[3] = {"drives", drive_keys[i], "image"}, *protect[3] = {"drives", drive_keys[i], "write_protect"},
                   *tracks[3] = {"drives", drive_keys[i], "tracks"}, *fast[3] = {"drives", drive_keys[i], "fast"};
        ok = set_unless_default(doc, image, 3, c->drives[i].image[0] ? stored_path(c, c->drives[i].image) : "", 1, !c->drives[i].image[0]) &&
             set_unless_default(doc, protect, 3, c->drives[i].write_protect ? "true" : "false", 2, !c->drives[i].write_protect) &&
             set_unless_default(doc, tracks, 3, c->drives[i].tracks == 80 ? "80" : "40", 2, c->drives[i].tracks != 80) &&
             set_unless_default(doc, fast, 3, c->drives[i].fast ? "true" : "false", 2, c->drives[i].fast);
    }
    for (unsigned i = 0; ok && i < MX_CONFIG_HARD; ++i) {
        const char *image[3] = {"drives", hard_keys[i], "image"}, *fast[3] = {"drives", hard_keys[i], "fast"};
        ok = set_unless_default(doc, image, 3, c->hard[i].image[0] ? stored_path(c, c->hard[i].image) : "", 1, !c->hard[i].image[0]) &&
             set_unless_default(doc, fast, 3, c->hard[i].fast ? "true" : "false", 2, c->hard[i].fast);
    }
    ok = ok && set(doc, "video", "look", mx_config_look_name(c->look)) && set(doc, "video", "monitor", mx_config_monitor_name(c->monitor)) &&
         set(doc, "video", "aspect", mx_config_aspect_name(c->aspect)) && set(doc, "video", "scale", mx_config_scale_name(c->scale)) &&
         set(doc, "video", "filter", mx_config_filter_name(c->filter)) && set_bool(doc, "video", "fullscreen", c->fullscreen) &&
         set_bool(doc, "video", "vsync", c->vsync) && set_bool(doc, "sound", "enabled", c->sound);
    if (ok) {
        const char *snow[2] = {"video", "snow"}, *lights[2] = {"video", "drive_lights"};
        ok = set_unless_default(doc, snow, 2, c->snow ? "true" : "false", 2, c->snow) &&
             set_unless_default(doc, lights, 2, c->drive_lights ? "true" : "false", 2, !c->drive_lights);
    }
    ok = ok && set_bool(doc, "ports", "adapter", c->sio_adapter);
    if (ok) {
        const char *pc[2] = {"ports", "pc_serial"};
        ok = set_unless_default(doc, pc, 2, mx_config_pc_serial_name(c->pc_serial), 0, c->pc_serial == MX_PCSERIAL_NONE);
    }
    if (ok) {
        const char *volume[2] = {"sound", "volume"};
        char number[16];
        snprintf(number, sizeof number, "%d", c->volume);
        ok = set_scalar(doc, volume, 2, number, 2);
    }
    if (ok) {
        const char *drives[2] = {"sound", "drives"}, *drives_volume[2] = {"sound", "drives_volume"};
        char number[16];
        snprintf(number, sizeof number, "%d", c->drive_volume);
        ok = set_unless_default(doc, drives, 2, c->drive_sound ? "true" : "false", 2, c->drive_sound) &&
             set_unless_default(doc, drives_volume, 2, number, 2, c->drive_volume == 50);
    }
    for (unsigned i = 0; ok && i < MX_CONFIG_PORTS; ++i) {
        const char *path[2] = {"ports", port_keys[i]};
        char text[MX_CONFIG_PATH * 2];
        mx_config_port_text(c, &c->ports[i], text, sizeof text);
        /* "tcp:127.0.0.1:2323" and "file:out.txt" read well unquoted; anything odd is quoted. */
        int plain = isalnum((unsigned char)text[0]) || text[0] == '/';
        for (const char *t = text; *t && plain; ++t) plain = isalnum((unsigned char)*t) || strchr("._:/-", *t) != NULL;
        /* The PC serial ports are written when they are set or the file has them already. */
        if (i >= MX_CONFIG_PORT_COM1) ok = set_unless_default(doc, path, 2, text, plain ? 2 : 1, c->ports[i].type == MX_END_NONE);
        else ok = set_scalar(doc, path, 2, text, plain ? 2 : 1);
    }
    if (ok) {
        const char *adapter[2] = {"controllers", "adapter"}, *pen[2] = {"controllers", "light_pen"};
        ok = set_unless_default(doc, adapter, 2, c->game_adapter ? "true" : "false", 2, !c->game_adapter) &&
             set_unless_default(doc, pen, 2, c->light_pen ? "true" : "false", 2, !c->light_pen);
    }
    for (unsigned i = 0; ok && i < MX_CONFIG_STAND_KEYS; ++i) {
        const char *path[3] = {"controllers", "keys", stand_keys_names[i]};
        ok = set_unless_default(doc, path, 3, c->stand_keys[i], 0, same_key(c->stand_keys[i], stand_defaults[i]));
    }
    for (unsigned i = 0; ok && i < MX_CONFIG_STICKS; ++i) {
        const mx_stick_config *st = &c->sticks[i];
        const char *device[3] = {"controllers", stick_keys[i], "device"}, *dead[3] = {"controllers", stick_keys[i], "dead_zone"},
                   *range[3] = {"controllers", stick_keys[i], "range"}, *cx[3] = {"controllers", stick_keys[i], "centre_x"},
                   *cy[3] = {"controllers", stick_keys[i], "centre_y"};
        char number[16], name[32];
        ok = set_scalar(doc, device, 3, mx_config_stick_device_name(st, name, sizeof name), 0);
        snprintf(number, sizeof number, "%d", st->dead_zone);
        ok = ok && set_scalar(doc, dead, 3, number, 2);
        snprintf(number, sizeof number, "%d", st->range);
        ok = ok && set_scalar(doc, range, 3, number, 2);
        snprintf(number, sizeof number, "%d", st->centre_x);
        ok = ok && set_scalar(doc, cx, 3, number, 2);
        snprintf(number, sizeof number, "%d", st->centre_y);
        ok = ok && set_scalar(doc, cy, 3, number, 2);
    }
    return ok && set(doc, "ui", "language", mx_config_language_name(c->language));
}

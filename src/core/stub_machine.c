/* The stub machine (machine.h, machine_impl.h): stands in for the Junior
 * until the board exists. It runs no guest code: its time advances exactly
 * as asked, and it shows an 80x25 text test screen drawn with the Junior's
 * own 8x8 character ROM into a 640x200 frame (shown 4:3), as the CGA's
 * 80-column text mode lays it out: the 256 glyphs, the 16 colours, the
 * version and which ROMs were found. Typed keys are echoed on its bottom
 * lines and each press beeps, so the shell, the configuration, the
 * presenter and the sound path are checked end to end. Without the
 * character ROM it uses MezinXT's own fallback font (font8x8.h) and says
 * what is missing. Its texts are the string table's (text.h) in the
 * language in use, composed again when that changes.
 *
 * MezinXT's own code; no hardware is
 * described here. */
#include "machine_impl.h"
#include "font8x8.h"
#include "text.h"
#include "mx_version.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLS 80
#define ROWS 25
#define WIDTH (COLS * 8)
#define HEIGHT (ROWS * 8)
/* The CGA's frame: 912 dots by 262 lines of the crystal (59.92 Hz). */
#define FRAME_PERIOD (912u * 262u)
/* Sound: 48 kHz, derived exactly from guest time: 48000 / (157.5/11 MHz)
 * = 176 / 52500 samples per crystal period. */
#define AUDIO_RATE 48000u
#define AUDIO_NUM 176u
#define AUDIO_DEN 52500u
#define TONE_FRAMES (AUDIO_RATE / 10u)    /* 100 ms per key press */
#define TONE_HALF_PERIOD 27u              /* about 889 Hz */
#define ECHO_MAX 64

typedef struct stub {
    char description[160], error[128];
    mx_config config;
    uint8_t font[2048];
    int chargen;                          /* the font is the Junior's character ROM */
    char rom_name[5][48], rom_file[5][40];/* the ROMs the screen lists, and whether each was found */
    int rom_found[5];
    int missing;                          /* a ROM is not in roms/ */
    uint32_t composed_language;           /* the language the fixed text was composed in */
    int echo_col;                         /* where the typed keys start on their line */
    unsigned unknown;                     /* unrecognised files in roms/ */
    /* time */
    uint64_t time;
    /* the screen: characters and attributes, then the frame */
    uint8_t text[ROWS][COLS][2];
    uint8_t pixels[WIDTH * HEIGHT];
    uint64_t serial;
    uint64_t drawn_phase, drawn_input, input_serial;
    /* the echo */
    int shift, caps;
    char echo[ECHO_MAX + 1];
    int last_code, last_down;
    /* sound */
    uint64_t audio_done, tone_until;
    struct mx_disk *drive[2];
} stub;

/* --- the text screen ---------------------------------------------------------------- */

static void put(stub *s, int row, int col, uint8_t attr, const char *text) {
    for (; *text && col < COLS; ++text, ++col) {
        s->text[row][col][0] = (uint8_t)*text;
        s->text[row][col][1] = attr;
    }
}
static void putf(stub *s, int row, int col, uint8_t attr, const char *format, ...) {
    char line[COLS + 1];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    put(s, row, col, attr, line);
}
static void fill(stub *s, int row, int col, int count, uint8_t ch, uint8_t attr) {
    for (int i = 0; i < count && col + i < COLS; ++i) {
        s->text[row][col + i][0] = ch;
        s->text[row][col + i][1] = attr;
    }
}

/* The parts that never change, in the language in use. */
static void compose(stub *s) {
    s->composed_language = mx_text_language();
    memset(s->text, 0, sizeof s->text);
    for (int r = 0; r < ROWS; ++r) fill(s, r, 0, COLS, ' ', 0x07);
    fill(s, 0, 0, COLS, ' ', 0x1F);
    char line[COLS + 1];
    mx_text_format(line, sizeof line, MX_T_STUB_TITLE, MX_VERSION);
    put(s, 0, 1, 0x1F, line);
    const char *name = MX_TXT(STUB_NAME);
    put(s, 0, COLS - (int)strlen(name), 0x1E, name);
    if (s->chargen) put(s, 1, 1, 0x07, MX_TXT(STUB_GLYPHS_ROM));
    else put(s, 1, 1, 0x0E, MX_TXT(STUB_GLYPHS_FALLBACK));
    if (s->missing) put(s, 2, 1, 0x0E, MX_TXT(STUB_MISSING));
    else if (s->config.speed == MX_SPEED_UNTHROTTLED)
        putf(s, 2, 1, 0x07, mx_t(MX_T_STUB_CONFIGURED_FREE), mx_config_bios_name(s->config.bios), mx_config_cpu_name(s->config.cpu), (unsigned)s->config.ram_kb);
    else {
        char speed[16];                                     /* 4.77, or 4,77 where the language has the decimal comma */
        snprintf(speed, sizeof speed, "%s", mx_config_speed_name(s->config.speed));
        if (mx_text_language() == MX_LANG_RO)
            for (char *c = speed; *c; ++c) if (*c == '.') *c = ',';
        putf(s, 2, 1, 0x07, mx_t(MX_T_STUB_CONFIGURED), mx_config_bios_name(s->config.bios), mx_config_cpu_name(s->config.cpu), speed,
             (unsigned)s->config.ram_kb);
    }
    /* The 256 glyphs, 32 to a row. */
    for (int r = 0; r < 8; ++r) {
        putf(s, 4 + r, 1, 0x03, "%02X", r * 32);
        for (int i = 0; i < 32; ++i) {
            s->text[4 + r][5 + 2 * i][0] = (uint8_t)(r * 32 + i);
            s->text[4 + r][5 + 2 * i][1] = 0x0F;
        }
    }
    /* The 16 colours: as backgrounds with their numbers, then as foregrounds. */
    put(s, 13, 1, 0x07, MX_TXT(STUB_PAPER));
    put(s, 14, 1, 0x07, MX_TXT(STUB_INK));
    for (int c = 0; c < 16; ++c) {
        const uint8_t ink = c == 0 || c == 1 || c == 4 || c == 5 || c == 8 ? 0x0F : 0x00;
        fill(s, 13, 8 + 4 * c, 4, ' ', (uint8_t)(c << 4 | ink));
        putf(s, 13, 9 + 4 * c, (uint8_t)(c << 4 | ink), "%X", c);
        fill(s, 14, 8 + 4 * c, 3, 0xDB, (uint8_t)c);
    }
    /* The ROMs found. */
    if (s->unknown) putf(s, 16, 1, 0x07, mx_t(s->unknown == 1 ? MX_T_STUB_ROMS_ONE_OTHER : MX_T_STUB_ROMS_OTHERS), s->unknown);
    else put(s, 16, 1, 0x07, MX_TXT(STUB_ROMS));
    for (int i = 0; i < 5; ++i) {
        char rom[COLS + 1];
        snprintf(rom, sizeof rom, "%-30s %-18s %s", s->rom_name[i], s->rom_file[i], s->rom_found[i] ? MX_TXT(STUB_FOUND) : MX_TXT(STUB_MISSING_ROM));
        put(s, 17 + i, 3, s->rom_found[i] ? 0x0A : 0x0C, rom);
    }
    fill(s, 24, 0, COLS, ' ', 0x70);
    put(s, 24, 1, 0x70, MX_TXT(STUB_HELP));
}

/* The lines that follow input. */
static void echo_line(stub *s) {
    fill(s, 23, 0, COLS, ' ', 0x07);
    put(s, 23, 1, 0x07, MX_TXT(STUB_KEYS));
    s->echo_col = 2 + (int)strlen(MX_TXT(STUB_KEYS));
    put(s, 23, s->echo_col, 0x0F, s->echo);
    if (s->last_code >= 0) putf(s, 23, COLS - 14, 0x03, "[%02Xh %s]", s->last_code, s->last_down ? MX_TXT(STUB_MAKE) : MX_TXT(STUB_BREAK));
}

/* The frame: each cell's glyph rows in its colours (attribute bit 7 is a
 * bright background: no blink), the cursor an underline on the cell's last
 * two rows after the echo, blinking every 8 frames as the 6845's does. */
static void render(stub *s, int cursor_on) {
    for (int r = 0; r < ROWS; ++r)
        for (int c = 0; c < COLS; ++c) {
            const uint8_t ch = s->text[r][c][0], attr = s->text[r][c][1];
            const uint8_t ink = attr & 15u, paper = attr >> 4;
            for (int y = 0; y < 8; ++y) {
                const uint8_t bits = s->font[ch * 8 + y];
                uint8_t *p = s->pixels + (size_t)(r * 8 + y) * WIDTH + (size_t)c * 8;
                for (int x = 0; x < 8; ++x) p[x] = bits & (0x80u >> x) ? ink : paper;
            }
        }
    if (cursor_on) {
        const int col = s->echo_col + (int)strlen(s->echo);
        if (col < COLS)
            for (int y = 6; y < 8; ++y) memset(s->pixels + (size_t)(23 * 8 + y) * WIDTH + (size_t)col * 8, 0x0F, 8);
    }
    ++s->serial;
}

static int stub_frame(void *self, mx_frame *f) {
    stub *s = self;
    const uint64_t phase = s->time / FRAME_PERIOD / 8u;
    if (s->composed_language != mx_text_language()) {
        compose(s);
        s->drawn_phase = UINT64_MAX;
    }
    if (phase != s->drawn_phase || s->input_serial != s->drawn_input) {
        s->drawn_phase = phase;
        s->drawn_input = s->input_serial;
        echo_line(s);
        render(s, !(phase & 1u));
    }
    f->width = WIDTH;
    f->height = HEIGHT;
    f->pitch = WIDTH;
    f->pixels = s->pixels;
    f->aspect_w = 4;
    f->aspect_h = 3;
    f->serial = s->serial;
    f->format = MX_FRAME_RGBI;
    return 1;
}

/* --- life cycle ------------------------------------------------------------------ */

void *mx_stub_create(const mx_config *config, const mx_rom_index *roms, char *error, size_t capacity) {
    stub *s = calloc(1, sizeof *s);
    if (!s) {
        if (capacity) snprintf(error, capacity, "Out of memory");
        return NULL;
    }
    s->config = *config;
    snprintf(s->description, sizeof s->description, "stub machine: test screen (BIOS %s configured; no CPU yet)",
             mx_config_bios_name(config->bios));
    const mx_rom_file *chargen = mx_rom_index_find(roms, mx_romdb_find("chargen"));
    s->chargen = chargen != NULL;
    if (chargen) memcpy(s->font, chargen->data, sizeof s->font);
    else mx_font8x8_build(s->font);
    for (size_t i = 0; i < mx_romdb_count() && i < 5; ++i) {
        const mx_known_rom *k = mx_romdb_entry(i);
        const mx_rom_file *f = mx_rom_index_find(roms, k);
        if (!f) s->missing = 1;
        s->rom_found[i] = f != NULL;
        snprintf(s->rom_name[i], sizeof s->rom_name[i], "%s", k->name);
        snprintf(s->rom_file[i], sizeof s->rom_file[i], "%s", f ? f->name : k->file);
    }
    for (size_t i = 0; roms && i < roms->count; ++i) s->unknown += roms->files[i].rom == NULL;
    s->last_code = -1;
    s->drawn_phase = UINT64_MAX;
    compose(s);
    return s;
}

static void stub_destroy(void *self) { free(self); }
static const char *stub_description(const void *self) { return ((const stub *)self)->description; }
static const char *stub_error(const void *self) { return ((const stub *)self)->error; }

static void stub_reset(void *self, int power_on) {
    stub *s = self;
    (void)power_on;
    s->echo[0] = 0;
    s->last_code = -1;
    s->shift = s->caps = 0;
    ++s->input_serial;
}

static mx_run_status stub_run(void *self, uint64_t ticks) {
    stub *s = self;
    s->time += ticks;
    return MX_RUN_OK;
}
static uint64_t stub_time(const void *self) { return ((const stub *)self)->time; }
static uint64_t stub_frame_period(const void *self) { (void)self; return FRAME_PERIOD; }

/* --- sound ------------------------------------------------------------------------ */

/* The speaker is always there: muting and the volume are the shell's. */
static uint32_t stub_audio_rate(const void *self) { (void)self; return AUDIO_RATE; }

static size_t stub_audio_pull(void *self, int16_t *out, size_t frames) {
    stub *s = self;
    const uint64_t due = s->time * AUDIO_NUM / AUDIO_DEN;
    /* Frames the shell never took are dropped beyond one second. */
    if (due - s->audio_done > AUDIO_RATE) s->audio_done = due - AUDIO_RATE;
    size_t n = 0;
    for (; n < frames && s->audio_done < due; ++n, ++s->audio_done) {
        const int16_t v = s->audio_done < s->tone_until ? (s->audio_done / TONE_HALF_PERIOD & 1u ? 6000 : -6000) : 0;
        out[n * 2] = out[n * 2 + 1] = v;
    }
    return n;
}

/* The stub has no drives to hear: silence at the pace the caller asks for (its beep is dropped, as at full speed). */
static void stub_set_drive_sound(void *self, int enabled, unsigned volume) { (void)self; (void)enabled; (void)volume; }
static void stub_set_snow(void *self, int on) { (void)self; (void)on; }
static size_t stub_audio_free(void *self, int16_t *out, size_t frames) {
    int16_t spare[512 * 2];
    while (stub_audio_pull(self, spare, 512) == 512) {}
    memset(out, 0, frames * 4u);
    return frames;
}

/* --- input -------------------------------------------------------------------------- */

/* Set 1 make codes 02h-39h as the US layout prints them (IBM PC/XT
 * Technical Reference, keyboard figure), unshifted and shifted; 0 for keys
 * that type nothing. */
static const char PLAIN[0x3A] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '};
static const char SHIFTED[0x3A] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '};
/* The keypad, 47h-53h, as digits. */
static const char KEYPAD[13] = {'7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.'};

static int stub_key(void *self, uint8_t code, int down) {
    stub *s = self;
    if (code == 0 || code > 0x7F) return 1;
    s->last_code = code;
    s->last_down = down != 0;
    ++s->input_serial;
    if (code == 0x2A || code == 0x36) { s->shift = down ? s->shift | (code == 0x2A ? 1 : 2) : s->shift & ~(code == 0x2A ? 1 : 2); return 1; }
    if (!down) return 1;
    s->tone_until = s->time * AUDIO_NUM / AUDIO_DEN + TONE_FRAMES;
    if (code == 0x3A) { s->caps = !s->caps; return 1; }
    size_t n = strlen(s->echo);
    if (code == 0x0E) { if (n) s->echo[n - 1] = 0; return 1; }          /* backspace */
    if (code == 0x1C) { s->echo[0] = 0; return 1; }                     /* Enter starts a new line */
    char ch = 0;
    if (code < 0x3A) {
        ch = s->shift ? SHIFTED[code] : PLAIN[code];
        if (s->caps && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        else if (s->caps && ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    } else if (code >= 0x47 && code <= 0x53) ch = KEYPAD[code - 0x47];
    if (!ch) return 1;
    if (n == ECHO_MAX) { memmove(s->echo, s->echo + 1, ECHO_MAX - 1); n--; }
    s->echo[n] = ch;
    s->echo[n + 1] = 0;
    return 1;
}

static void stub_joystick(void *self, unsigned stick, int x, int y, unsigned buttons) { (void)self; (void)stick; (void)x; (void)y; (void)buttons; }
static void stub_joystick_plug(void *self, unsigned stick, int plugged) { (void)self; (void)stick; (void)plugged; }
static void stub_pointer(void *self, int x, int y, int inside, unsigned buttons) { (void)self; (void)x; (void)y; (void)inside; (void)buttons; }
static void stub_mouse(void *self, int dx, int dy, unsigned buttons) { (void)self; (void)dx; (void)dy; (void)buttons; }

/* --- media: two floppy drives that only hold their images ----------------------------- */

static unsigned stub_drives(const void *self, mx_media_kind kind) { (void)self; return kind == MX_MEDIA_FLOPPY ? 2u : 0u; }
static int stub_insert(void *self, mx_media_kind kind, unsigned drive, struct mx_disk *disk, char *error, size_t capacity) {
    stub *s = self;
    if (kind != MX_MEDIA_FLOPPY || drive > 1) {
        if (capacity) snprintf(error, capacity, "There is no such drive");
        return 0;
    }
    if (s->drive[drive]) {
        if (capacity) snprintf(error, capacity, "The drive is not empty");
        return 0;
    }
    s->drive[drive] = disk;
    return 1;
}
static struct mx_disk *stub_media(const void *self, mx_media_kind kind, unsigned drive) {
    const stub *s = self;
    return kind == MX_MEDIA_FLOPPY && drive < 2 ? s->drive[drive] : NULL;
}
static int stub_eject(void *self, mx_media_kind kind, unsigned drive) {
    stub *s = self;
    if (kind != MX_MEDIA_FLOPPY || drive > 1 || !s->drive[drive]) return 0;
    s->drive[drive] = NULL;
    return 1;
}
static unsigned stub_drive_activity(const void *self, mx_media_kind kind) { (void)self; (void)kind; return 0; }

/* --- ports and checks: nothing there -------------------------------------------------- */

static void stub_set_serial(void *self, const mx_serial_host *host) { (void)self; (void)host; }
static size_t stub_serial_receive(void *self, unsigned port, const uint8_t *data, size_t count) {
    (void)self; (void)port; (void)data;
    return count;                         /* taken and dropped */
}
static void stub_serial_modem(void *self, unsigned port, int dcd, int cts, int dsr, int ri) {
    (void)self; (void)port; (void)dcd; (void)cts; (void)dsr; (void)ri;
}
static void stub_set_trace(void *self, mx_trace_fn fn, void *ctx) { (void)self; (void)fn; (void)ctx; }
static int stub_peek(void *self, uint32_t address, uint8_t *value) { (void)self; (void)address; *value = 0xFF; return 0; }

const mx_machine_impl mx_stub_impl = {
    stub_destroy, stub_description, stub_error, stub_reset, stub_run, stub_time, stub_frame_period, stub_frame,
    stub_audio_rate, stub_audio_pull, stub_key, stub_joystick, stub_joystick_plug, stub_pointer, stub_mouse,
    stub_drives, stub_insert, stub_media, stub_eject, stub_drive_activity,
    stub_set_serial, stub_serial_receive, stub_serial_modem, stub_set_trace, stub_peek,
    stub_set_drive_sound, stub_audio_free, stub_set_snow,
};

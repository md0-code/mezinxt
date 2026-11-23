/* mezinxt: the command line, the configuration, the ROMs, then the window
 * (desktop.c). */
#include "../core/config.h"
#include "../core/disk.h"
#include "../core/machine.h"
#include "../core/romdb.h"
#include "../host/host.h"
#include "desktop.h"
#include "setup.h"
#include "mezinxt_default_yaml.h"
#include "mx_version.h"

#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_NAME "mezinxt.yaml"

/* The last message of a failure, also shown in a message box when the
 * program was started without a console. */
static char last_error[2048];
static void complain(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(last_error, sizeof last_error, format, args);
    va_end(args);
    fprintf(stderr, "%s\n", last_error);
}

/* Without --config, mezinxt.yaml in the working directory; a missing one is
 * written from the template so every setting can be edited. */
static const char *default_configuration(char *error, size_t capacity) {
    FILE *f = fopen(CONFIG_NAME, "rb");
    if (f) { fclose(f); return CONFIG_NAME; }
    /* The template, with the language of the host's user locale (the template says en). */
    unsigned char text[sizeof mx_default_yaml];
    memcpy(text, mx_default_yaml, sizeof text);
    static const char KEY[] = "language: en";
    if (mx_config_default_language() == MX_LANG_RO)
        for (size_t i = 0; i + sizeof KEY - 1 <= sizeof text; ++i)
            if (!memcmp(text + i, KEY, sizeof KEY - 1)) { text[i + sizeof KEY - 3] = 'r'; text[i + sizeof KEY - 2] = 'o'; break; }
    /* The template puts the manual disk in drive A, as a release's run folder has it; without the file there the drive is empty. */
    static const char DISK[] = "image: disks/manual.img";
    f = fopen("disks/manual.img", "rb");
    if (f) fclose(f);
    else
        for (size_t i = 0; i + sizeof DISK - 1 <= sizeof text; ++i)
            if (!memcmp(text + i, DISK, sizeof DISK - 1)) { memset(text + i + 6, ' ', sizeof DISK - 7); break; }
    f = fopen(CONFIG_NAME, "wb");
    int ok = f && fwrite(text, 1, sizeof text, f) == sizeof text;
    if (f && fclose(f)) ok = 0;
    if (!ok) { snprintf(error, capacity, "Cannot create %s", CONFIG_NAME); return NULL; }
    printf("Created %s with the default settings\n", CONFIG_NAME);
    return CONFIG_NAME;
}

/* The roms folders: in the working directory, beside the configuration,
 * beside the program (the same folder is read once). */
static int scan_roms(mx_rom_index *roms, const char *yaml) {
    char beside[MX_CONFIG_PATH], program[MX_CONFIG_PATH] = "", folder[MX_CONFIG_PATH];
    snprintf(folder, sizeof folder, "%s", yaml ? yaml : "");
    char *slash = strrchr(folder, '/'), *backslash = strrchr(folder, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
    if (slash) slash[1] = 0; else folder[0] = 0;
    snprintf(beside, sizeof beside, "%sroms", folder);
    if (mx_host_program_folder(folder, sizeof folder)) snprintf(program, sizeof program, "%sroms", folder);
    const char *folders[3] = {"roms", beside, program[0] ? program : NULL};
    mx_rom_index_init(roms);
    return mx_rom_index_scan(roms, folders, 3);
}

/* The known ROMs and where each was found, then every file of the roms
 * folders and what it was recognised as. */
static void list_roms(const mx_rom_index *ix) {
    char what[256];
    printf("Known ROMs (recognised by the SHA-256 of their content, whatever the file name):\n");
    printf("  %-14s %-28s %6s  %-18s %s\n", "key", "name", "bytes", "standard name", "found");
    for (size_t i = 0; i < mx_romdb_count(); ++i) {
        const mx_known_rom *k = mx_romdb_entry(i);
        const mx_rom_file *f = mx_rom_index_find(ix, k);
        printf("  %-14s %-28s %6u  %-18s %s\n", k->key, k->name, (unsigned)k->size, k->file, f ? f->path : "missing");
    }
    for (size_t k = 0; k < ix->folder_count; ++k) {
        const size_t length = strlen(ix->folders[k]);
        size_t n = 0;
        for (size_t i = 0; i < ix->count; ++i) n += !strncmp(ix->files[i].path, ix->folders[k], length);
        printf("\nROM folder %s: %zu file%s\n", ix->folders[k], n, n == 1 ? "" : "s");
        for (size_t i = 0; i < ix->count; ++i)
            if (!strncmp(ix->files[i].path, ix->folders[k], length))
                printf("  %s: %s\n", ix->files[i].name, mx_rom_file_describe(ix, &ix->files[i], what, sizeof what));
    }
    if (!mx_rom_index_find(ix, mx_romdb_bios(MX_BIOS_4_0)) || !mx_rom_index_find(ix, mx_romdb_find("chargen")))
        printf("\npython tools/fetch_roms.py fetches the ROMs into roms/\n");
}

/* controllers.keys names host keys: the configuration reader (src/core, with no SDL) asks whether SDL has one of that name. */
static int host_key_known(const char *name) { return SDL_GetScancodeFromName(name) != SDL_SCANCODE_UNKNOWN; }

static int usage(void) {
    printf("MezinXT %s, an emulator of the FEPER JUNIOR-XT\n\n"
           "mezinxt [--config FILE.yaml] [--hidden [--realtime]] [--frames N] [--screenshot FILE.png|FILE.bmp] [--wav FILE.wav]\n"
           "        [--key CODE@N]... [--type TEXT@N]... [--hostkey KEY@N]... [--drop FILE@N]... [--drop-b FILE@N]...\n"
           "        [--joy N:X,Y,BUTTONS@N]... [--pointer X,Y,BUTTON@N]... [--mouse DX,DY,BUTTONS@N]...\n"
           "        [--trace FILE.txt] [--test-screen] [--setup-page NAME]\n"
           "mezinxt --list-roms [--config FILE.yaml] | --version | --help\n\n"
           "Without --config, mezinxt.yaml in the working directory is used, written with the defaults when missing.\n"
           "ROMs are read from roms/ in the working directory, beside the configuration and beside the program.\n"
           "--hidden runs without a visible window or sound device, the same on any host; --frames N stops after N\n"
           "frames; --realtime paces a hidden window to the clock as a visible one is; --screenshot saves the last\n"
           "frame (a PNG when the name ends in .png); --wav records the sound to a file instead of playing it;\n"
           "--key CODE@N presses XT key CODE (hex make code, + 80 for a release) after N frames, up to 1024 of\n"
           "them (they drive the setup screen while it is open); --type TEXT@N types TEXT from frame N on, two\n"
           "frames a key (\\n Enter, \\e Esc, \\t Tab, \\b Backspace, \\\\ a backslash, ^Z Ctrl+Z, ^^ a caret); keys and\n"
           "typing are given in the order of their frames; --hostkey KEY@N presses a host key after N frames as the\n"
           "keyboard would (SDL's key names, with ctrl+, shift+ or alt+ before: ctrl+f12@5; + or - in front of the\n"
           "name for only the press or the release), --hostevent focus-lost@N takes the keyboard from the window,\n"
           "--drop FILE@N drops a file on the window (--drop-b: with Shift held); --joy N:X,Y,BUTTONS@F sets joystick\n"
           "N (1 or 2) of the game control adapter to X and Y (-32768 to 32767) and buttons 0 to 3 (bit 0 and 1) at\n"
           "frame F and plugs it in (--joy N:off@F unplugs it); --pointer X,Y,BUTTON@F puts the pen on pixel X,Y of the\n"
           "640x200 frame with its switch 0 or 1 (--pointer off@F takes it off the picture); they act when the adapter\n"
           "(controllers.adapter) is fitted and the light pen (controllers.light_pen) enabled; --mouse click@F clicks\n"
           "the picture to capture the host mouse and --mouse DX,DY,BUTTONS@F moves it by DX,DY counts with the left (1)\n"
           "and right (2) buttons held while it is captured, for a serial mouse (ports.com1 or com2: mouse);\n"
           "--setup-page NAME opens the setup screen at the start on a page (drives, machine, display, sound,\n"
           "ports or controllers); the machine stays paused while it is open;\n"
           "--trace writes the guest's I/O cycles and interrupts to a file (\"IN port value\", \"OUT port value\",\n"
           "\"INT vector\"); --test-screen runs the test screen instead of the Junior.\n"
           "In the window: Alt+Enter toggles full screen, F12 opens and closes the setup screen (a game controller's\n"
           "Start or Guide button does too), Shift+F12 exits MezinXT at once (the setup screen's Exit item does too: the\n"
           "disk images are written back and what was changed in the setup screen is saved), Ctrl+F12 switches the\n"
           "machine off and on, Alt+F12 resets it, Pause stops and starts it,\n"
           "Print Screen saves mezinxt-NNNN.png, Ctrl+Pause sends Ctrl-Break and Ctrl+Alt+End Ctrl-Alt-Del; other\n"
           "keys are the XT keys in the same place. A disk image dropped on the window goes into drive A (drive B\n"
           "with Shift held). A joystick fed by the keyboard stand-in (controllers.joystick1 or 2: keyboard, with the\n"
           "game control adapter fitted) takes the arrow keys and Left Ctrl and Left Shift (buttons 1 and 2) from the\n"
           "guest, or the keys controllers.keys names (the setup screen's Controllers page, Keyboard keys); F11 gives\n"
           "them to the guest and back, with a message. The mouse over the picture is the light pen, the left button\n"
           "its switch. A serial mouse is set on the setup screen's Controllers page (Mouse: COM1 or COM2, which fits\n"
           "the PC serial adapter and takes out the Junior's own serial and printer adapter; ports.pc_serial and\n"
           "ports.com1 or com2: mouse in the file): a click on the picture captures the host mouse (the pointer is\n"
           "hidden, the click itself only captures) and Ctrl+F10 lets it go; the setup screen, Pause and losing the\n"
           "keyboard release it too. video.drive_lights shows a light for each drive at work.\n"
           "In the setup screen Left and Right turn the pages, Up and Down select an option, Page Up and Page Down\n"
           "change its value, Enter opens or flips it and Esc leaves (a game controller: the d-pad moves, left and\n"
           "right turn the pages, the shoulder buttons change the value, A is Enter, B is Esc, Start leaves).\n"
           "ui.language in mezinxt.yaml (or the setup screen's Display page) sets the language of the setup screen and\n"
           "of the messages in the window: en or ro (Romanian); the default is the host's language.\n", MX_VERSION);
    return 0;
}

/* --trace: one line per I/O cycle and interrupt acknowledge. */
static void trace_line(void *ctx, int kind, uint32_t address, uint32_t value) {
    FILE *f = ctx;
    if (kind == MX_TRACE_IO_READ) fprintf(f, "IN %04X %02X\n", (unsigned)address, (unsigned)value);
    else if (kind == MX_TRACE_IO_WRITE) fprintf(f, "OUT %04X %02X\n", (unsigned)address, (unsigned)value);
    else if (kind == MX_TRACE_INTERRUPT) fprintf(f, "INT %02X\n", (unsigned)address);
}

/* --type TEXT@N: the text as key transitions from frame N on, two frames a
 * key: a Shift or Ctrl make and the key's make on one frame, the key's break
 * and the release on the next. \n \e \t \b are Enter, Esc, Tab and Backspace,
 * \\ a backslash, ^Z Ctrl+Z (any letter), ^^ a caret. */
static int add_key(mx_desktop_options *o, uint64_t frame, unsigned code) {
    if (o->key_count == MX_DESKTOP_KEYS) return 0;
    o->key_at[o->key_count] = frame;
    o->key_code[o->key_count++] = (uint8_t)code;
    return 1;
}

static int add_typing(mx_desktop_options *o, const char *spec) {
    const char *at = strrchr(spec, '@');
    unsigned long long frame = 0;
    char tail = 0;
    if (!at || sscanf(at + 1, "%llu%c", &frame, &tail) != 1 || !frame) return 0;
    for (const char *p = spec; p < at;) {
        int shift = 0, code = -1;
        unsigned mod = 0;
        const int ch = (unsigned char)*p++;
        if (ch == '\\' && p < at) {
            const int e = (unsigned char)*p++;
            code = e == 'n' || e == 'r' ? 0x1C : e == 'e' ? 0x01 : e == 't' ? 0x0F : e == 'b' ? 0x0E
                 : e == '\\' ? mx_desktop_char_key('\\', &shift) : -1;
        } else if (ch == '^' && p < at && *p != '^') {
            const int letter = (unsigned char)*p++ | 0x20;
            if (letter >= 'a' && letter <= 'z') code = mx_desktop_char_key(letter, &shift);
            mod = 0x1D;
        } else {
            if (ch == '^' && p < at) p++;                       /* ^^ */
            code = mx_desktop_char_key(ch, &shift);
        }
        if (code < 0) return 0;
        if (shift && !mod) mod = 0x2A;
        if ((mod && !add_key(o, frame, mod)) || !add_key(o, frame, (unsigned)code) || !add_key(o, frame + 1, (unsigned)code | 0x80u) ||
            (mod && !add_key(o, frame + 1, mod | 0x80u)))
            return 0;
        frame += 2;
    }
    return 1;
}

/* --joy N:X,Y,BUTTONS and --pointer X,Y,BUTTON (or ":off" / "off"): the step's values, 0 when they are not valid. */
static int joy_values(mx_desktop_script *step, const char *text) {
    char *end = NULL;
    const unsigned long stick = strtoul(text, &end, 10);
    if (end == text || *end != ':' || stick < 1 || stick > 2) return 0;
    step->stick = (unsigned)stick - 1u;
    text = end + 1;
    if (!strcmp(text, "off")) { step->off = 1; return 1; }
    const long x = strtol(text, &end, 10);
    if (end == text || *end != ',') return 0;
    text = end + 1;
    const long y = strtol(text, &end, 10);
    if (end == text || *end != ',') return 0;
    text = end + 1;
    const unsigned long buttons = strtoul(text, &end, 10);
    if (end == text || *end || buttons > 3 || x < -32768 || x > 32767 || y < -32768 || y > 32767) return 0;
    step->x = (int)x;
    step->y = (int)y;
    step->buttons = (unsigned)buttons;
    return 1;
}

/* --mouse DX,DY,BUTTONS or click. */
static int mouse_values(mx_desktop_script *step, const char *text) {
    char *end = NULL;
    if (!strcmp(text, "click")) { step->click = 1; return 1; }
    const long x = strtol(text, &end, 10);
    if (end == text || *end != ',') return 0;
    text = end + 1;
    const long y = strtol(text, &end, 10);
    if (end == text || *end != ',') return 0;
    text = end + 1;
    const unsigned long buttons = strtoul(text, &end, 10);
    if (end == text || *end || buttons > 3 || x < -100000 || x > 100000 || y < -100000 || y > 100000) return 0;
    step->x = (int)x;
    step->y = (int)y;
    step->buttons = (unsigned)buttons;
    return 1;
}

static int pointer_values(mx_desktop_script *step, const char *text) {
    char *end = NULL;
    if (!strcmp(text, "off")) { step->off = 1; return 1; }
    const long x = strtol(text, &end, 10);
    if (end == text || *end != ',') return 0;
    text = end + 1;
    const long y = strtol(text, &end, 10);
    if (end == text || *end != ',') return 0;
    text = end + 1;
    const unsigned long button = strtoul(text, &end, 10);
    if (end == text || *end || button > 1 || x < 0 || x > 639 || y < 0 || y > 199) return 0;
    step->x = (int)x;
    step->y = (int)y;
    step->buttons = (unsigned)button;
    return 1;
}

/* --hostkey KEY@N, --drop FILE@N, --joy and --pointer: a step of the checks script (desktop.h). */
static int add_script(mx_desktop_options *o, const char *spec, int kind, unsigned mods) {
    const char *at = strrchr(spec, '@');
    unsigned long long frame = 0;
    char tail = 0, name[256];
    if (o->script_count == MX_DESKTOP_SCRIPT || !at || sscanf(at + 1, "%llu%c", &frame, &tail) != 1 || !frame) return 0;
    if (o->script_count && o->script[o->script_count - 1].at > frame) return 0;
    mx_desktop_script *step = &o->script[o->script_count];
    memset(step, 0, sizeof *step);
    step->at = frame;
    step->kind = kind;
    step->mods = mods;
    if (kind == MX_SCRIPT_DROP) {
        static char paths[MX_DESKTOP_SCRIPT][MX_CONFIG_PATH];
        const size_t n = (size_t)(at - spec);
        if (!n || n >= MX_CONFIG_PATH) return 0;
        memcpy(paths[o->script_count], spec, n);
        paths[o->script_count][n] = 0;
        step->path = paths[o->script_count];
    } else if (kind == MX_SCRIPT_FOCUS_LOST) {
        if (strncmp(spec, "focus-lost", (size_t)(at - spec)) || (size_t)(at - spec) != 10) return 0;
    } else if (kind == MX_SCRIPT_JOY || kind == MX_SCRIPT_POINTER || kind == MX_SCRIPT_MOUSE) {
        char values[64];
        const size_t n = (size_t)(at - spec);
        if (!n || n >= sizeof values) return 0;
        memcpy(values, spec, n);
        values[n] = 0;
        if (!(kind == MX_SCRIPT_JOY ? joy_values(step, values) : kind == MX_SCRIPT_MOUSE ? mouse_values(step, values) : pointer_values(step, values))) return 0;
    } else {
        const char *key = spec;
        size_t n = (size_t)(at - spec);
        if (n > 1 && (*key == '+' || *key == '-')) {         /* + a press only, - a release only */
            step->mods |= *key == '+' ? MX_SCRIPT_PRESS : MX_SCRIPT_RELEASE;
            ++key;
            --n;
        }
        for (;;) {                                          /* ctrl+, shift+ and alt+ in front */
            unsigned bit = 0;
            size_t len = 0;
            if (n > 5 && !SDL_strncasecmp(key, "ctrl+", 5)) { bit = MX_SCRIPT_CTRL; len = 5; }
            else if (n > 6 && !SDL_strncasecmp(key, "shift+", 6)) { bit = MX_SCRIPT_SHIFT; len = 6; }
            else if (n > 4 && !SDL_strncasecmp(key, "alt+", 4)) { bit = MX_SCRIPT_ALT; len = 4; }
            if (!bit) break;
            step->mods |= bit;
            key += len;
            n -= len;
        }
        if (!n || n >= sizeof name) return 0;
        memcpy(name, key, n);
        name[n] = 0;
        step->scancode = (int)SDL_GetScancodeFromName(name);
        if (step->scancode == SDL_SCANCODE_UNKNOWN) return 0;
    }
    ++o->script_count;
    return 1;
}

static int run(int argc, char **argv) {
    const char *yaml = NULL, *trace_path = NULL;
    int roms_list = 0, test_screen = 0;
    mx_desktop_options options;
    memset(&options, 0, sizeof options);
    options.setup_page = -1;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--version")) { printf("MezinXT %s\n", MX_VERSION); return 0; }
        else if (!strcmp(argv[i], "--help")) return usage();
        else if (!strcmp(argv[i], "--list-roms")) roms_list = 1;
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) yaml = argv[++i];
        else if (!strcmp(argv[i], "--hidden")) options.hidden = 1;
        else if (!strcmp(argv[i], "--realtime")) options.realtime = 1;
        else if (!strcmp(argv[i], "--hostkey") && i + 1 < argc) {
            if (!add_script(&options, argv[++i], MX_SCRIPT_HOSTKEY, 0)) {
                complain("--hostkey needs KEY@N: an SDL key name (F12, PrintScreen, Pause, A), optionally with ctrl+, shift+ or alt+, and a frame "
                         "number; at most %d, in the order of their frames", MX_DESKTOP_SCRIPT);
                return 2;
            }
        } else if (!strcmp(argv[i], "--hostevent") && i + 1 < argc) {
            if (!add_script(&options, argv[++i], MX_SCRIPT_FOCUS_LOST, 0)) {
                complain("--hostevent needs focus-lost@N: the event and a frame number, in the order of the frames of the other steps");
                return 2;
            }
        } else if (!strcmp(argv[i], "--joy") && i + 1 < argc) {
            if (!add_script(&options, argv[++i], MX_SCRIPT_JOY, 0)) {
                complain("--joy needs N:X,Y,BUTTONS@F (joystick 1 or 2, X and Y from -32768 to 32767, buttons 0 to 3) or N:off@F, F a frame "
                         "number; at most %d steps, in the order of their frames", MX_DESKTOP_SCRIPT);
                return 2;
            }
        } else if (!strcmp(argv[i], "--pointer") && i + 1 < argc) {
            if (!add_script(&options, argv[++i], MX_SCRIPT_POINTER, 0)) {
                complain("--pointer needs X,Y,BUTTON@F (a pixel of the 640x200 frame, the pen's switch 0 or 1) or off@F, F a frame "
                         "number; at most %d steps, in the order of their frames", MX_DESKTOP_SCRIPT);
                return 2;
            }
        } else if (!strcmp(argv[i], "--mouse") && i + 1 < argc) {
            if (!add_script(&options, argv[++i], MX_SCRIPT_MOUSE, 0)) {
                complain("--mouse needs click@F (click the picture, capturing the host mouse) or DX,DY,BUTTONS@F (move it by DX,DY counts "
                         "with the buttons 0 to 3 held), F a frame number; at most %d steps, in the order of their frames", MX_DESKTOP_SCRIPT);
                return 2;
            }
        } else if ((!strcmp(argv[i], "--drop") || !strcmp(argv[i], "--drop-b")) && i + 1 < argc) {
            const unsigned shift = argv[i][6] == '-' ? MX_SCRIPT_SHIFT : 0;
            if (!add_script(&options, argv[++i], MX_SCRIPT_DROP, shift)) {
                complain("--drop needs FILE@N: a file and a frame number; at most %d, in the order of their frames", MX_DESKTOP_SCRIPT);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) options.screenshot = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) options.wav = argv[++i];
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_path = argv[++i];
        else if (!strcmp(argv[i], "--test-screen")) test_screen = 1;
        else if (!strcmp(argv[i], "--setup-page") && i + 1 < argc) {
            options.setup_page = mx_setup_page_by_name(argv[++i]);
            if (options.setup_page < 0) { complain("--setup-page needs drives, machine, display, sound, ports or controllers"); return 2; }
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            char *end = NULL;
            options.frames = strtoull(argv[++i], &end, 10);
            if (!options.frames || !end || *end) { complain("--frames needs a positive whole number"); return 2; }
        } else if (!strcmp(argv[i], "--type") && i + 1 < argc) {
            if (!add_typing(&options, argv[++i])) {
                complain("--type needs TEXT@N: the text to type and the frame it starts at (at most %d key transitions)", MX_DESKTOP_KEYS);
                return 2;
            }
        } else if (!strcmp(argv[i], "--key") && i + 1 < argc) {
            unsigned code = 0;
            unsigned long long at = 0;
            char tail = 0;
            if (options.key_count == MX_DESKTOP_KEYS || sscanf(argv[++i], "%x@%llu%c", &code, &at, &tail) != 2 || !(code & 0x7F) ||
                code > 0xFF || !at) {
                complain("--key needs CODE@N: an XT make code in hex (+ 80 for a release) and a frame number (at most %d)", MX_DESKTOP_KEYS);
                return 2;
            }
            options.key_at[options.key_count] = at;
            options.key_code[options.key_count++] = (uint8_t)code;
        } else { complain("Unknown or incomplete argument: %s (mezinxt --help)", argv[i]); return 2; }
    }
    if (!mx_machine_host_ok()) { complain("MezinXT needs a little-endian host"); return 1; }
    char error[1024] = "";
    mx_config config;
    static mx_rom_index roms;
    if (roms_list) {
        if (!scan_roms(&roms, yaml)) { complain("Out of memory"); return 1; }
        list_roms(&roms);
        mx_rom_index_free(&roms);
        return 0;
    }
    mx_config_set_key_check(host_key_known);
    if (!yaml && !(yaml = default_configuration(error, sizeof error))) { complain("%s", error); return 1; }
    if (!mx_config_load(yaml, &config, error, sizeof error)) { complain("%s: %s", yaml, error); return 2; }
    if (!scan_roms(&roms, yaml)) { complain("Out of memory"); return 1; }
    mx_machine *machine = test_screen ? mx_machine_create_stub(&config, &roms, error, sizeof error)
                                      : mx_machine_create(&config, &roms, error, sizeof error);
    if (!machine) { complain("%s", error); mx_rom_index_free(&roms); return 1; }
    /* The disk images the configuration names, in their drives (core/disk.h); the shell keeps them in step. */
    mx_disk *disks[MX_DISK_SLOTS];
    if (!mx_disk_insert_configured(machine, &config, disks, error, sizeof error)) {
        complain("%s", error);
        mx_machine_destroy(machine);
        mx_rom_index_free(&roms);
        return 2;
    }
    if (error[0]) fprintf(stderr, "Warning: %s\n", error);
    options.disks = disks;
    FILE *trace = NULL;
    if (trace_path) {
        if (!(trace = fopen(trace_path, "w"))) { complain("Cannot write %s", trace_path); mx_machine_destroy(machine); mx_disk_close_all(disks); mx_rom_index_free(&roms); return 1; }
        mx_machine_set_trace(machine, trace_line, trace);
        options.trace = trace_line;
        options.trace_ctx = trace;
    }
    options.roms = &roms;
    options.stub = test_screen;
    printf("MezinXT %s: %s\n", MX_VERSION, mx_machine_description(machine));
    fflush(stdout);
    int status = mx_desktop_run(&machine, &config, &options, error, sizeof error);
    if (status) complain("%s", error);
    mx_machine_set_trace(machine, NULL, NULL);
    mx_machine_destroy(machine);
    /* The machine has written the images back; one it could not is reported. */
    for (unsigned i = 0; i < MX_DISK_SLOTS; ++i)
        if (disks[i] && mx_disk_dirty(disks[i])) { complain("Cannot save %s: %s", mx_disk_path(disks[i]), mx_disk_error(disks[i])); status = 1; }
    mx_disk_close_all(disks);
    mx_rom_index_free(&roms);
    if (trace && fclose(trace) && !status) { complain("Cannot write %s", trace_path); status = 1; }
    return status ? 3 : 0;
}

/* A program without a console window of its own: messages go to the
 * console it was started from or to the pipes a script gave it; started
 * from the desktop, a failure is shown in a message box. */
int main(int argc, char **argv) {
    const int unseen = mx_host_console_init();
    const int status = run(argc, argv);
    fflush(stdout);
    if (unseen && status && last_error[0]) mx_host_alert("MezinXT", last_error);
    return status;
}

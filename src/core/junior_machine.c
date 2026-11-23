/* The Junior as a machine (machine.h, machine_impl.h): the board (hw/junior.h)
 * built from the configuration and the ROM index, behind the machine
 * interface, with its CGA (cga_card.h) when machine.display names one, the
 * diskette adapter (machine.drives drives of 40 or 80 tracks), the serial and
 * parallel adapter (ports_card.h) when ports.adapter says it is fitted, the
 * PC/XT asynchronous adapter's 8250 ports (pcserial_card.h) when ports.pc_serial
 * says so, the game control adapter (hw/gameport.h) when controllers.adapter does, the
 * Winchester coupler (hw/hdc.h) with its option ROM at C8000h when machine.winchester does, and the
 * 384 KiB module's EPROM images (machine.rom_*) when the memory is 640 KiB. machine.display
 * names the one display coupler: the CGA, or the monochrome coupler (mda_card.h) with its
 * printer port; the two are not fitted together (the shell has one screen).
 * Without a display adapter on the board the frame is black.
 * MezinXT's own code. */
#include "cga_card.h"
#include "disk.h"
#include "font8x8.h"
#include "machine_impl.h"
#include "mda_card.h"
#include "pcserial_card.h"
#include "ports_card.h"
#include "stick.h"
#include "../host/host.h"
#include "../hw/drivesnd.h"
#include "../hw/fdc_card.h"
#include "../hw/gameport.h"
#include "../hw/hdc.h"
#include "../hw/junior.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The frame without a display adapter: the CGA's 912 dots by 262 lines of
 * the crystal (59.92 Hz). */
#define FRAME_PERIOD (912u * 262u)

typedef struct board {
    mx_junior *j;
    mx_cga_card *cga;           /* NULL: no CGA */
    mx_mda_card *mda;           /* NULL: no monochrome coupler (cga and mda are never both set) */
    mx_config config;
    char description[160], error[128];
    mx_trace_fn trace;
    void *trace_ctx;
    mx_fdc_card fdc;            /* the diskette adapter: I/O 3F0h-3F7h, IRQ 6, DMA channel 2 */
    mx_ports_card *ports;       /* the serial and parallel adapter; NULL: not fitted */
    mx_pcserial_card *pcserial; /* the 8250 pair (COM1, COM2); NULL: not fitted */
    struct mx_disk *drive[4];   /* the images in the drives (borrowed) */
    mx_gameport game;           /* the game control adapter, when fitted (have_game) */
    int have_game;
    uint8_t plugged[2];         /* the joysticks in the adapter's connectors */
    int stick_x[2], stick_y[2];
    unsigned stick_buttons[2];
    int pen_x, pen_y, pen_inside;       /* the light pen as the host last said, to put back after a power cycle */
    unsigned pen_buttons;
    uint8_t *option_rom[MX_CONFIG_ROMS];    /* the EPROM pairs of the 384 KiB module, padded to their size */
    mx_hdc hdc;                 /* the Winchester coupler: I/O 320h-323h, IRQ 5, DMA channel 3 (have_hdc) */
    int have_hdc;
    uint8_t *hdc_rom;           /* its option ROM at C8000h */
    struct mx_disk *hard[MX_HDC_DRIVES];    /* the fixed disks' images (borrowed) */
} board;

/* The fixed disk's light (and the shell's fast-disk mode) stays on this long after a command: a fifth of a second. */
#define HARD_ACTIVITY_HOLD (MX_OSC_HZ / 5u)

/* The module's EPROM sockets: a pair of 2764 (16 KiB) or 27128 (32 KiB), selected by straps; the room an
 * image needs. The pair at F8000h cannot be a 27128: that would cover the BIOS at FE000h-FFFFFh. */
#define ROM_SMALL 0x4000u
#define ROM_LARGE 0x8000u

static void release(board *b) {
    if (b->j) mx_junior_destroy(b->j);
    mx_cga_card_free(b->cga);
    mx_mda_card_free(b->mda);
    mx_ports_card_free(b->ports);
    mx_pcserial_card_free(b->pcserial);
    for (unsigned i = 0; i < MX_CONFIG_ROMS; ++i) free(b->option_rom[i]);
    free(b->hdc_rom);
    free(b);
}

/* The image of socket `i` read and mapped read-only, 0 with the reason. */
static int map_option_rom(board *b, unsigned i, const char *path, char *error, size_t capacity) {
    uint8_t *file = NULL;
    size_t size = 0;
    int writable = 0;
    char why[256] = "";
    const uint32_t base = mx_config_rom_address(i);
    if (!mx_host_read_file(path, ROM_LARGE, &file, &size, &writable, why, sizeof why)) {
        if (capacity) snprintf(error, capacity, "machine.%s: %s", mx_config_rom_key(i), why[0] ? why : "cannot read the image");
        return 0;
    }
    const size_t room = size <= ROM_SMALL ? ROM_SMALL : ROM_LARGE;
    if (!size || (base == 0xF8000u && room == ROM_LARGE)) {
        if (capacity) {
            snprintf(error, capacity, !size ? "machine.%s: the image is empty" : "machine.%s: a 32 KiB ROM at F8000h would cover the BIOS (at most 16 KiB there)",
                     mx_config_rom_key(i));
        }
        free(file);
        return 0;
    }
    uint8_t *rom = malloc(room);
    if (!rom) { free(file); if (capacity) snprintf(error, capacity, "Out of memory"); return 0; }
    memset(rom, 0xFF, room);                    /* an EPROM's erased cells */
    memcpy(rom, file, size);
    free(file);
    b->option_rom[i] = rom;
    mx_bus_map_direct(mx_junior_bus(b->j), base, (uint32_t)room, rom, 0);
    return 1;
}

/* The drive sound (hw/drivesnd.h) on or off, at `volume` percent. On, the diskette drives and the Winchester coupler
 * report their mechanical events to it, and it is told what is running already (a motor, a spindle); off, they have no
 * observer at all. */
static void drive_sound_apply(board *b, int enabled, unsigned volume) {
    mx_drivesnd *ds = mx_junior_drivesnd(b->j);
    const int was = mx_drivesnd_enabled(ds);
    mx_junior_set_drive_sound(b->j, enabled, volume);
    const int on = mx_drivesnd_enabled(ds);
    if (on == was) return;
    const mx_drive_hook hook = on ? mx_drivesnd_event : NULL;
    mx_fdc_card_set_drive_hook(&b->fdc, hook, ds);
    if (b->have_hdc) mx_hdc_set_drive_hook(&b->hdc, hook, ds);
    const uint64_t now = mx_junior_time(b->j);
    for (unsigned n = 0; on && n < b->fdc.drives; ++n)
        if (b->fdc.drive[n].motor) mx_drivesnd_event(ds, MX_DRIVE_FLOPPY0 + n, MX_DRIVE_MOTOR, now, 1, 0);
}

void *mx_junior_machine_create(const mx_config *config, const mx_rom_index *roms, char *error, size_t capacity) {
    if (!mx_config_check(config, error, capacity)) return NULL;
    const mx_rom_file *bios = mx_rom_index_find(roms, mx_romdb_bios(config->bios));
    if (!bios) {
        if (capacity) snprintf(error, capacity, "BIOS %s is not in roms/ (python tools/fetch_roms.py)", mx_config_bios_name(config->bios));
        return NULL;
    }
    board *b = calloc(1, sizeof *b);
    if (!b) { if (capacity) snprintf(error, capacity, "Out of memory"); return NULL; }
    b->config = *config;
    mx_junior_options o;
    memset(&o, 0, sizeof o);
    o.cpu = config->cpu == MX_CPU_8088 ? MX_JUNIOR_8088 : MX_JUNIOR_8086;
    /* OSC periods per CPU clock: 4.77 MHz (OSC/3), 7.16 (OSC/2), 9.54
     * (OSC/1.5); unthrottled runs at 4.77 MHz guest time, only faster. */
    o.clock_num = config->speed == MX_SPEED_7_16 ? 2u : 3u;
    o.clock_den = config->speed == MX_SPEED_9_54 ? 2u : 1u;
    o.ram_kb = config->ram_kb;
    o.drives = config->floppies;           /* machine.drives: the DIP switches SW7-8 say so */
    /* The switches say what is fitted: the BIOS looks for a CGA only when
     * they announce one (and waits on its status register if none answers). */
    o.display = config->display == MX_DISPLAY_CGA40 ? MX_JUNIOR_DISPLAY_CGA40
              : config->display == MX_DISPLAY_CGA80 ? MX_JUNIOR_DISPLAY_CGA80
              : config->display == MX_DISPLAY_MDA ? MX_JUNIOR_DISPLAY_MDA : MX_JUNIOR_DISPLAY_NONE;
    o.fpu = config->fpu != 0;            /* the 8087 and its switch, SW2 */
    b->j = mx_junior_create(&o, bios->data, (size_t)bios->size, error, capacity);
    if (!b->j) { free(b); return NULL; }
    /* The 384 KiB module carries the EPROM sockets: only a board of 640 KiB has them. */
    for (unsigned i = 0; config->ram_kb == 640 && i < MX_CONFIG_ROMS; ++i)
        if (config->rom[i][0] && !map_option_rom(b, i, config->rom[i], error, capacity)) { release(b); return NULL; }
    if (o.display != MX_JUNIOR_DISPLAY_NONE) {
        /* The glyphs are the Junior's character ROM; without it MezinXT's
         * own font keeps the screen legible. */
        uint8_t font[MX_CGA_GLYPH_BYTES];
        const mx_rom_file *chargen = mx_rom_index_find(roms, mx_romdb_find("chargen"));
        if (chargen && chargen->size >= sizeof font) memcpy(font, chargen->data, sizeof font);
        else mx_font8x8_build(font);
        if (o.display == MX_JUNIOR_DISPLAY_MDA) b->mda = mx_mda_card_attach(b->j, font);
        else b->cga = mx_cga_card_attach(b->j, font);
        if (b->cga) mx_cga_card_set_snow(b->cga, config->snow);
        if (!b->cga && !b->mda) {
            if (capacity) snprintf(error, capacity, "Out of memory");
            release(b);
            return NULL;
        }
    }
    /* The diskette adapter (hw/fdc_card.h): the BIOS reaches it through the
     * 765 and the 8237 only. The drives follow the DIP switches (SW7-8). */
    mx_bus *bus = mx_junior_bus(b->j);
    mx_fdc_card_init(&b->fdc, mx_junior_host(b->j), "fdc", o.drives);
    for (unsigned i = 0; i < o.drives; ++i) mx_fdc_card_set_tracks(&b->fdc, i, config->drives[i].tracks);
    b->fdc.dev.ops->reset(&b->fdc.dev);
    if (!mx_bus_map_io(bus, &b->fdc.dev, MX_FDC_IO_BASE, MX_FDC_IO_BASE + MX_FDC_IO_SIZE - 1u, 0) ||
        !mx_bus_route(bus, &b->fdc.dev, MX_FDC_CARD_IRQ, MX_ROUTE_IRQ, 6) ||
        !mx_bus_route(bus, &b->fdc.dev, MX_FDC_CARD_DRQ, MX_ROUTE_DRQ, 2)) {
        if (capacity) snprintf(error, capacity, "The board has no room for the diskette adapter");
        release(b);
        return NULL;
    }
    /* The Winchester coupler (manual p. 20: "Cuplor de disc fix (Winchester)" at 320h-32Fh on IRQ 5), as the IBM
     * Fixed Disk Adapter on DMA channel 3, with MezinXT's own BIOS for it at C8000h (hw/hdc.h). */
    if (config->winchester) {
        mx_hdc_init(&b->hdc, mx_junior_host(b->j), "hdc");
        b->hdc_rom = malloc(MX_HDC_ROM_SIZE);
        if (!b->hdc_rom) { if (capacity) snprintf(error, capacity, "Out of memory"); release(b); return NULL; }
        memcpy(b->hdc_rom, mx_hdc_rom, MX_HDC_ROM_SIZE);
        b->hdc.dev.ops->reset(&b->hdc.dev);
        if (!mx_bus_map_io(bus, &b->hdc.dev, MX_HDC_IO_BASE, MX_HDC_IO_BASE + MX_HDC_IO_SIZE - 1u, 0) ||
            !mx_bus_route(bus, &b->hdc.dev, MX_HDC_IRQ, MX_ROUTE_IRQ, MX_HDC_IRQ_LINE) ||
            !mx_bus_route(bus, &b->hdc.dev, MX_HDC_DRQ, MX_ROUTE_DRQ, MX_HDC_DMA_CHANNEL) ||
            !mx_bus_map_direct(bus, MX_HDC_ROM_BASE, MX_HDC_ROM_SIZE, b->hdc_rom, 0)) {
            if (capacity) snprintf(error, capacity, "The board has no room for the Winchester coupler");
            release(b);
            return NULL;
        }
        b->have_hdc = 1;
    }
    /* The game control adapter (joystick coupler) at 200h-207h; the joysticks are plugged in by the shell. */
    if (config->game_adapter) {
        mx_gameport_init(&b->game, mx_junior_host(b->j), "gameport");
        if (!mx_bus_map_io(bus, &b->game.dev, MX_GAMEPORT_IO_BASE, MX_GAMEPORT_IO_BASE + MX_GAMEPORT_IO_SIZE - 1u, 0)) {
            if (capacity) snprintf(error, capacity, "The board has no room for the game control adapter");
            release(b);
            return NULL;
        }
        b->have_game = 1;
    }
    /* The serial and parallel adapter: the SIO at 250h and the printer at 378h, the BIOS finds them by probing. */
    if (config->sio_adapter) {
        b->ports = mx_ports_card_attach(b->j);
        if (!b->ports) {
            if (capacity) snprintf(error, capacity, "The board has no room for the serial and parallel adapter");
            release(b);
            return NULL;
        }
    }
    /* The PC/XT asynchronous adapter: COM1 at 3F8h (IRQ 4) and COM2 at 2F8h (IRQ 3), which BIOS 3.0 and 5.0 find by their IIR. */
    if (config->pc_serial != MX_PCSERIAL_NONE) {
        b->pcserial = mx_pcserial_card_attach(b->j, config->pc_serial == MX_PCSERIAL_TWO ? 2u : 1u);
        if (!b->pcserial) {
            if (capacity) snprintf(error, capacity, "The board has no room for the 8250 adapter");
            release(b);
            return NULL;
        }
    }
    drive_sound_apply(b, config->drive_sound, (unsigned)config->drive_volume);
    snprintf(b->description, sizeof b->description, "FEPER JUNIOR-XT, BIOS %s, %s%s, %u KiB", mx_config_bios_name(config->bios),
             mx_config_cpu_name(config->cpu), config->fpu ? " and 8087" : "", (unsigned)config->ram_kb);
    return b;
}

static void b_destroy(void *self) {
    board *b = self;
    /* The images are the caller's: written back and let go. */
    for (unsigned n = 0; n < 4; ++n)
        if (b->drive[n]) mx_disk_release(b->drive[n]);
    for (unsigned n = 0; n < MX_HDC_DRIVES; ++n)
        if (b->hard[n]) mx_disk_release(b->hard[n]);
    release(b);
}
static const char *b_description(const void *self) { return ((const board *)self)->description; }
static const char *b_error(const void *self) { return ((const board *)self)->error; }
static void b_reset(void *self, int power_on) {
    board *b = self;
    for (unsigned n = 0; n < 4; ++n)
        if (b->drive[n]) mx_disk_flush(b->drive[n]);
    for (unsigned n = 0; n < MX_HDC_DRIVES; ++n)
        if (b->hard[n]) mx_disk_flush(b->hard[n]);
    if (power_on && b->cga) {
        mx_cga_card_power_on(b->cga);
        mx_cga_card_pointer(b->cga, b->pen_x, b->pen_y, b->pen_inside, b->pen_buttons);     /* the pen stays where the host has it */
    }
    if (power_on && b->mda) mx_mda_card_power_on(b->mda);
    mx_junior_reset(b->j, power_on);
}

static mx_run_status b_run(void *self, uint64_t ticks) {
    board *b = self;
    const int halted = mx_junior_run(b->j, ticks);
    /* A disk the guest changed is written back once it has stood changed a
     * second (guest time, so the same run writes the same files). */
    const uint64_t now = mx_junior_time(b->j);
    for (unsigned n = 0; n < 4; ++n)
        if (b->drive[n]) mx_disk_tick(b->drive[n], now);
    for (unsigned n = 0; n < MX_HDC_DRIVES; ++n)
        if (b->hard[n]) mx_disk_tick(b->hard[n], now);
    return halted ? MX_RUN_HALTED : MX_RUN_OK;
}
static uint64_t b_time(const void *self) { return mx_junior_time(((const board *)self)->j); }
static uint64_t b_frame_period(const void *self) {
    const board *b = self;
    return b->cga ? mx_cga_card_frame_period(b->cga) : b->mda ? mx_mda_card_frame_period(b->mda) : FRAME_PERIOD;
}
static int b_frame(void *self, mx_frame *f) { return mx_junior_frame(((board *)self)->j, f); }

/* The speaker is always there: muting and the volume are the shell's. */
static uint32_t b_audio_rate(const void *self) { (void)self; return 48000u; }
static size_t b_audio_pull(void *self, int16_t *stereo, size_t frames) { return mx_junior_audio(((board *)self)->j, stereo, frames); }
static size_t b_audio_free(void *self, int16_t *stereo, size_t frames) { return mx_junior_audio_free(((board *)self)->j, stereo, frames); }
static void b_set_drive_sound(void *self, int enabled, unsigned volume) { drive_sound_apply(self, enabled, volume); }
static void b_set_snow(void *self, int on) {
    board *b = self;
    if (b->cga) mx_cga_card_set_snow(b->cga, on);
}

static int b_key(void *self, uint8_t make_code, int down) { return mx_junior_key(((board *)self)->j, make_code, down); }
/* A joystick's axes and buttons into the game port: the pot's resistance follows the position; a stick
 * that is not plugged in has open axes and no buttons. */
static void game_update(board *b, unsigned stick) {
    if (!b->have_game) return;
    const unsigned first = stick * 2u;
    if (b->plugged[stick]) {
        mx_gameport_set_axis(&b->game, first, mx_stick_ohms(b->stick_x[stick]));
        mx_gameport_set_axis(&b->game, first + 1u, mx_stick_ohms(b->stick_y[stick]));
    } else {
        mx_gameport_set_axis(&b->game, first, MX_GAMEPORT_OPEN);
        mx_gameport_set_axis(&b->game, first + 1u, MX_GAMEPORT_OPEN);
    }
    unsigned pressed = 0;
    for (unsigned s = 0; s < 2; ++s) if (b->plugged[s]) pressed |= (b->stick_buttons[s] & 3u) << (s * 2u);
    mx_gameport_set_buttons(&b->game, pressed);
}
static void b_joystick(void *self, unsigned stick, int x, int y, unsigned buttons) {
    board *b = self;
    if (stick > 1) return;
    b->stick_x[stick] = x;
    b->stick_y[stick] = y;
    b->stick_buttons[stick] = buttons;
    game_update(b, stick);
}
static void b_joystick_plug(void *self, unsigned stick, int plugged) {
    board *b = self;
    if (stick > 1) return;
    b->plugged[stick] = plugged != 0;
    game_update(b, stick);
}
static void b_pointer(void *self, int x, int y, int inside, unsigned buttons) {
    board *b = self;
    b->pen_x = x;
    b->pen_y = y;
    b->pen_inside = inside != 0;
    b->pen_buttons = buttons;
    if (b->cga) mx_cga_card_pointer(b->cga, x, y, inside, buttons);
}
static void b_mouse(void *self, int dx, int dy, unsigned buttons) {
    board *b = self;
    if (b->pcserial) mx_pcserial_card_mouse(b->pcserial, dx, dy, buttons);
}

/* Media: the images in the diskette adapter's drives and on the Winchester coupler (disk.h). */
static unsigned b_drives(const void *self, mx_media_kind kind) {
    const board *b = self;
    if (kind == MX_MEDIA_FLOPPY) return b->fdc.drives;
    return kind == MX_MEDIA_HARD_DISK && b->have_hdc ? MX_HDC_DRIVES : 0u;
}
static int b_in_use(const board *b, const struct mx_disk *disk, char *error, size_t capacity) {
    for (unsigned n = 0; n < b->fdc.drives; ++n)
        if (b->drive[n] == disk) { if (capacity) snprintf(error, capacity, "The disk is already in drive %c", (char)('A' + n)); return 1; }
    for (unsigned n = 0; n < MX_HDC_DRIVES; ++n)
        if (b->hard[n] == disk) { if (capacity) snprintf(error, capacity, "The disk is already fixed disk %u", n); return 1; }
    return 0;
}
static int b_insert(void *self, mx_media_kind kind, unsigned drive, struct mx_disk *disk, char *error, size_t capacity) {
    board *b = self;
    if (drive >= b_drives(b, kind)) { if (capacity) snprintf(error, capacity, "There is no such drive"); return 0; }
    if (!disk) { if (capacity) snprintf(error, capacity, "There is no disk to insert"); return 0; }
    struct mx_disk **slot = kind == MX_MEDIA_FLOPPY ? &b->drive[drive] : &b->hard[drive];
    if (*slot) { if (capacity) snprintf(error, capacity, "The drive is not empty"); return 0; }
    if (b_in_use(b, disk, error, capacity)) return 0;
    if (kind == MX_MEDIA_FLOPPY ? !mx_disk_insert_floppy(disk, &b->fdc, drive, error, capacity)
                                : !mx_disk_insert_hard(disk, &b->hdc, drive, error, capacity))
        return 0;
    *slot = disk;
    return 1;
}
static struct mx_disk *b_media(const void *self, mx_media_kind kind, unsigned drive) {
    const board *b = self;
    if (drive >= b_drives(b, kind)) return NULL;
    return kind == MX_MEDIA_FLOPPY ? b->drive[drive] : b->hard[drive];
}
static int b_eject(void *self, mx_media_kind kind, unsigned drive) {
    board *b = self;
    if (drive >= b_drives(b, kind)) return 0;
    if (kind == MX_MEDIA_FLOPPY) {
        if (!b->drive[drive]) return 0;
        mx_disk_eject_floppy(b->drive[drive], &b->fdc, drive);
        b->drive[drive] = NULL;
    } else {
        if (!b->hard[drive]) return 0;
        mx_disk_eject_hard(b->hard[drive], &b->hdc, drive);
        b->hard[drive] = NULL;
    }
    return 1;
}
static unsigned b_drive_activity(const void *self, mx_media_kind kind) {
    const board *b = self;
    if (kind == MX_MEDIA_FLOPPY) return mx_fdc_card_activity(&b->fdc);
    return kind == MX_MEDIA_HARD_DISK && b->have_hdc ? mx_hdc_activity(&b->hdc, mx_junior_time(b->j), HARD_ACTIVITY_HOLD) : 0u;
}

/* The byte ports: the serial and parallel adapter's, when it is fitted (ports_card.h). */
static void b_set_serial(void *self, const mx_serial_host *host) {
    board *b = self;
    if (b->ports) mx_ports_card_set_host(b->ports, host);
    if (b->pcserial) mx_pcserial_card_set_host(b->pcserial, host);
    if (b->mda) mx_mda_card_set_host(b->mda, host);
}
static int is_pc_port(unsigned port) { return port == MX_PORT_COM1 || port == MX_PORT_COM2; }
static size_t b_serial_receive(void *self, unsigned port, const uint8_t *data, size_t count) {
    board *b = self;
    if (is_pc_port(port)) return b->pcserial ? mx_pcserial_card_receive(b->pcserial, port, data, count) : count;
    return b->ports ? mx_ports_card_receive(b->ports, port, data, count) : count;
}
static void b_serial_modem(void *self, unsigned port, int dcd, int cts, int dsr, int ri) {
    board *b = self;
    if (is_pc_port(port)) {
        if (b->pcserial) mx_pcserial_card_modem(b->pcserial, port, dcd, cts, dsr, ri);
    } else if (port == MX_PORT_LPT_MDA) {
        if (b->mda) mx_mda_card_modem(b->mda, dsr);
    } else if (b->ports) mx_ports_card_modem(b->ports, port, dcd, cts, dsr, ri);
}

/* The board's trace kinds are machine.h's. */
_Static_assert(MX_JUNIOR_TRACE_IO_READ == MX_TRACE_IO_READ && MX_JUNIOR_TRACE_IO_WRITE == MX_TRACE_IO_WRITE &&
               MX_JUNIOR_TRACE_FETCH == MX_TRACE_FETCH && MX_JUNIOR_TRACE_INTERRUPT == MX_TRACE_INTERRUPT, "trace kinds");
static void b_set_trace(void *self, mx_trace_fn fn, void *ctx) { mx_junior_set_trace(((board *)self)->j, fn, ctx); }
static int b_peek(void *self, uint32_t address, uint8_t *value) {
    *value = mx_junior_peek(((board *)self)->j, address);
    return 1;
}

const mx_machine_impl mx_junior_impl = {
    b_destroy, b_description, b_error, b_reset, b_run, b_time, b_frame_period, b_frame,
    b_audio_rate, b_audio_pull, b_key, b_joystick, b_joystick_plug, b_pointer, b_mouse,
    b_drives, b_insert, b_media, b_eject, b_drive_activity,
    b_set_serial, b_serial_receive, b_serial_modem, b_set_trace, b_peek,
    b_set_drive_sound, b_audio_free, b_set_snow,
};

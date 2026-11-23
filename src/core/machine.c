/* The machine interface (machine.h): the public calls dispatched to the
 * machine that runs, and the monitors' colours (the CGA card's output levels are video_out.c's). MezinXT's own code. */
#include "machine_impl.h"
#include "video_out.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fixed-width types and 8-bit bytes. */
_Static_assert(CHAR_BIT == 8, "MezinXT needs 8-bit bytes");
_Static_assert(sizeof(uint16_t) == 2 && sizeof(uint32_t) == 4 && sizeof(uint64_t) == 8, "fixed-width types");

struct mx_machine {
    const mx_machine_impl *impl;
    void *self;
};

int mx_machine_host_ok(void) {
    const uint16_t one = 1;
    uint8_t first;
    memcpy(&first, &one, 1);
    return first == 1;
}

static mx_machine *wrap(const mx_machine_impl *impl, void *self, char *error, size_t capacity) {
    if (!self) return NULL;
    mx_machine *m = malloc(sizeof *m);
    if (!m) {
        impl->destroy(self);
        if (capacity) snprintf(error, capacity, "Out of memory");
        return NULL;
    }
    m->impl = impl;
    m->self = self;
    return m;
}

mx_machine *mx_machine_create_stub(const mx_config *config, const mx_rom_index *roms, char *error, size_t capacity) {
    if (!mx_machine_host_ok()) {
        if (capacity) snprintf(error, capacity, "This host is not little-endian: MezinXT cannot run on it");
        return NULL;
    }
    return wrap(&mx_stub_impl, mx_stub_create(config, roms, error, capacity), error, capacity);
}

/* The Junior's board; without its BIOS in the ROM index, the stub's test
 * screen, which names the missing ROMs. */
mx_machine *mx_machine_create(const mx_config *config, const mx_rom_index *roms, char *error, size_t capacity) {
    if (!mx_machine_host_ok()) {
        if (capacity) snprintf(error, capacity, "This host is not little-endian: MezinXT cannot run on it");
        return NULL;
    }
    if (!mx_rom_index_find(roms, mx_romdb_bios(config->bios))) return mx_machine_create_stub(config, roms, error, capacity);
    return wrap(&mx_junior_impl, mx_junior_machine_create(config, roms, error, capacity), error, capacity);
}

void mx_machine_destroy(mx_machine *m) {
    if (!m) return;
    m->impl->destroy(m->self);
    free(m);
}

const char *mx_machine_description(const mx_machine *m) { return m->impl->description(m->self); }
const char *mx_machine_error(const mx_machine *m) { return m->impl->error(m->self); }
void mx_machine_reset(mx_machine *m, int power_on) { m->impl->reset(m->self, power_on); }
mx_run_status mx_machine_run(mx_machine *m, uint64_t ticks) { return m->impl->run(m->self, ticks); }
uint64_t mx_machine_time(const mx_machine *m) { return m->impl->time(m->self); }
uint64_t mx_machine_frame_period(const mx_machine *m) { return m->impl->frame_period(m->self); }
int mx_machine_frame(mx_machine *m, mx_frame *frame) { return m->impl->frame(m->self, frame); }
uint32_t mx_machine_audio_rate(const mx_machine *m) { return m->impl->audio_rate(m->self); }
size_t mx_machine_audio_pull(mx_machine *m, int16_t *stereo, size_t frames) { return m->impl->audio_pull(m->self, stereo, frames); }
void mx_machine_set_drive_sound(mx_machine *m, int enabled, unsigned volume) { m->impl->set_drive_sound(m->self, enabled, volume); }
size_t mx_machine_audio_free(mx_machine *m, int16_t *stereo, size_t frames) { return m->impl->audio_free(m->self, stereo, frames); }
void mx_machine_set_snow(mx_machine *m, int on) { m->impl->set_snow(m->self, on); }
int mx_machine_key(mx_machine *m, uint8_t make_code, int down) { return m->impl->key(m->self, make_code, down); }
void mx_machine_joystick(mx_machine *m, unsigned stick, int x, int y, unsigned buttons) { m->impl->joystick(m->self, stick, x, y, buttons); }
void mx_machine_joystick_plug(mx_machine *m, unsigned stick, int plugged) { m->impl->joystick_plug(m->self, stick, plugged); }
void mx_machine_pointer(mx_machine *m, int x, int y, int inside, unsigned buttons) { m->impl->pointer(m->self, x, y, inside, buttons); }
void mx_machine_mouse(mx_machine *m, int dx, int dy, unsigned buttons) { m->impl->mouse(m->self, dx, dy, buttons); }
unsigned mx_machine_drives(const mx_machine *m, mx_media_kind kind) { return m->impl->drives(m->self, kind); }
int mx_machine_insert(mx_machine *m, mx_media_kind kind, unsigned drive, struct mx_disk *disk, char *error, size_t capacity) {
    return m->impl->insert(m->self, kind, drive, disk, error, capacity);
}
struct mx_disk *mx_machine_media(const mx_machine *m, mx_media_kind kind, unsigned drive) { return m->impl->media(m->self, kind, drive); }
int mx_machine_eject(mx_machine *m, mx_media_kind kind, unsigned drive) { return m->impl->eject(m->self, kind, drive); }
unsigned mx_machine_drive_activity(const mx_machine *m, mx_media_kind kind) { return m->impl->drive_activity(m->self, kind); }
void mx_machine_set_serial(mx_machine *m, const mx_serial_host *host) { m->impl->set_serial(m->self, host); }
size_t mx_machine_serial_receive(mx_machine *m, unsigned port, const uint8_t *data, size_t count) {
    return m->impl->serial_receive(m->self, port, data, count);
}
void mx_machine_serial_modem(mx_machine *m, unsigned port, int dcd, int cts, int dsr, int ri) {
    m->impl->serial_modem(m->self, port, dcd, cts, dsr, ri);
}
void mx_machine_set_trace(mx_machine *m, mx_trace_fn fn, void *ctx) { m->impl->set_trace(m->self, fn, ctx); }
int mx_machine_peek(mx_machine *m, uint32_t address, uint8_t *value) { return m->impl->peek(m->self, address, value); }

/* --- the monitors ------------------------------------------------------------------------ */

/* The IBM 5153 Color Display's 16 colours: two levels per gun, a third of
 * full for intensity, and colour 6 brown (its green halved) - the RGBI
 * monitor the Junior's CGA coupler drives (manual 3.9). */
uint32_t mx_rgbi_color(unsigned index) {
    const unsigned i = index & 15u, hi = i & 8u ? 0x55u : 0u;
    unsigned r = (i & 4u ? 0xAAu : 0u) + hi, g = (i & 2u ? 0xAAu : 0u) + hi, b = (i & 1u ? 0xAAu : 0u) + hi;
    if (i == 6) g = 0x55u;
    return r << 16 | g << 8 | b;
}

void mx_frame_argb(const mx_frame *f, uint32_t monitor, uint32_t *out) {
    uint32_t colors[16];
    if (f->format == MX_FRAME_MONO) {
        /* The monochrome coupler's monitor: off, normal and intense (a CGA's light gray and white). */
        static const uint32_t levels[3] = {0x00u, 0xAAu, 0xFFu};
        for (unsigned i = 0; i < 16; ++i) {
            const uint32_t y = levels[i < 3 ? i : 2];
            colors[i] = 0xFF000000u | y << 16 | y << 8 | y;
        }
        for (uint32_t y = 0; y < f->height; ++y) {
            const uint8_t *row = f->pixels + (size_t)y * f->pitch;
            uint32_t *o = out + (size_t)y * f->width;
            for (uint32_t x = 0; x < f->width; ++x) o[x] = colors[row[x] & 15u];
        }
        return;
    }
    if (monitor != MX_MONITOR_RGBI && monitor != MX_MONITOR_RGB) {
        /* The composite jack J2: the resistor network's brightness for the colour, through the monitor's limited video
         * bandwidth (video_out.h), a scanline at a time: the frame is what the monitor shows. */
        const mx_composite_filter *filter = monitor == MX_MONITOR_MDA ? mx_composite_filter_none() : mx_composite_filter_default();
        for (uint32_t y = 0; y < f->height; ++y) mx_cga_composite_row(filter, f->pixels + (size_t)y * f->pitch, f->width, out + (size_t)y * f->width);
        return;
    }
    for (unsigned i = 0; i < 16; ++i) {
        /* The RGBI monitor's colours, or the RCA jacks J3-J5's: intensity is mixed into every channel (three levels each). */
        colors[i] = 0xFF000000u | (monitor == MX_MONITOR_RGB ? mx_cga_rgb_color(i) : mx_rgbi_color(i));
    }
    for (uint32_t y = 0; y < f->height; ++y) {
        const uint8_t *row = f->pixels + (size_t)y * f->pitch;
        uint32_t *o = out + (size_t)y * f->width;
        for (uint32_t x = 0; x < f->width; ++x) o[x] = colors[row[x] & 15u];
    }
}

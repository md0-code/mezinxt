/* Inside the machine interface (machine.h): each kind of machine (the stub
 * now, the board from package P2c) supplies this table, and machine.c
 * dispatches the public calls to it. Not for the shell. */
#ifndef MX_MACHINE_IMPL_H
#define MX_MACHINE_IMPL_H

#include "machine.h"

typedef struct mx_machine_impl {
    void (*destroy)(void *self);
    const char *(*description)(const void *self);
    const char *(*error)(const void *self);
    void (*reset)(void *self, int power_on);
    mx_run_status (*run)(void *self, uint64_t ticks);
    uint64_t (*time)(const void *self);
    uint64_t (*frame_period)(const void *self);
    int (*frame)(void *self, mx_frame *frame);
    uint32_t (*audio_rate)(const void *self);
    size_t (*audio_pull)(void *self, int16_t *stereo, size_t frames);
    int (*key)(void *self, uint8_t make_code, int down);
    void (*joystick)(void *self, unsigned stick, int x, int y, unsigned buttons);
    void (*joystick_plug)(void *self, unsigned stick, int plugged);
    void (*pointer)(void *self, int x, int y, int inside, unsigned buttons);
    void (*mouse)(void *self, int dx, int dy, unsigned buttons);
    unsigned (*drives)(const void *self, mx_media_kind kind);
    int (*insert)(void *self, mx_media_kind kind, unsigned drive, struct mx_disk *disk, char *error, size_t capacity);
    struct mx_disk *(*media)(const void *self, mx_media_kind kind, unsigned drive);
    int (*eject)(void *self, mx_media_kind kind, unsigned drive);
    unsigned (*drive_activity)(const void *self, mx_media_kind kind);
    void (*set_serial)(void *self, const mx_serial_host *host);
    size_t (*serial_receive)(void *self, unsigned port, const uint8_t *data, size_t count);
    void (*serial_modem)(void *self, unsigned port, int dcd, int cts, int dsr, int ri);
    void (*set_trace)(void *self, mx_trace_fn fn, void *ctx);
    int (*peek)(void *self, uint32_t address, uint8_t *value);
    void (*set_drive_sound)(void *self, int enabled, unsigned volume);
    size_t (*audio_free)(void *self, int16_t *stereo, size_t frames);
    void (*set_snow)(void *self, int on);
} mx_machine_impl;

/* The stub machine (stub_machine.c). */
extern const mx_machine_impl mx_stub_impl;
void *mx_stub_create(const mx_config *config, const mx_rom_index *roms, char *error, size_t capacity);

/* The Junior's board (junior_machine.c); NULL when the configured BIOS is
 * not in the index. */
extern const mx_machine_impl mx_junior_impl;
void *mx_junior_machine_create(const mx_config *config, const mx_rom_index *roms, char *error, size_t capacity);

#endif /* MX_MACHINE_IMPL_H */

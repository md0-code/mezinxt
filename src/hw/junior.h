/* MezinXT - the FEPER JUNIOR-XT's main board.
 *
 * The 8086 (or 8088) and the board's chips wired as the Junior's manual
 * (chapter 1, I/O map and 8255 bit tables) and the IBM PC/XT system board it
 * repeats connect them:
 *
 *  - 8253: clock OSC/12; OUT0 -> IR0; OUT1 clocks the refresh request
 *    flip-flop (DREQ0), which DACK0 clears; GATE0 and GATE1 high; GATE2 is
 *    port B bit 0; OUT2 and port B bit 1 drive the speaker (their AND is
 *    port C bit 4), OUT2 is port C bit 5.
 *  - 8255 (control word 99h): port A the keyboard's shift register while
 *    port B bit 7 is low (cleared, 00h, while it is high); port B bit 3
 *    selects the DIP switch nibble on port C bits 0-3; bit 4 low enables
 *    the RAM parity check, bit 5 low the I/O channel check (their latches
 *    are port C bits 7 and 6; a high bit clears its latch); bit 6 low holds
 *    the keyboard clock low; bit 7 high clears the keyboard interface.
 *  - Keyboard interface -> IR1. 8259 INT -> the CPU's INTR.
 *  - NMI = the mask register's bit 7 (port A0h, cleared at reset) AND (a
 *    RAM parity error OR an I/O channel check OR the 8087's INT).
 *  - The 8087 (options.fpu) on the CPU's ESC hook, its BUSY on TEST.
 *  - 8237: channel 0 refresh, cycles of 5 CPU clocks (4 for refresh), the
 *    page registers at 80h-83h, the CPU held for each DMA cycle.
 *  - Memory: RAM from 0 (64 to 256, or 640 KiB), open bus up to F0000h, the BIOS
 *    ROM (64 KiB, the EPROM pair interleaved) from F0000h, write-protected.
 *  - Wait states: one on every I/O cycle (5-clock cycles), none on memory;
 *    a card's I/O CH RDY through the wait flip-flop and the 8284.
 *  - DIP switches from the options: SW1 off (normal), SW2 the 8087, SW3-4
 *    off (the board's memory: all four banks; fewer banks for 64 to 192 KiB), SW5-6 the display coupler,
 *    SW7-8 the floppy drives less one (an "off" switch reads 1).
 *
 * Expansion cards join through mx_junior_bus (bus.h) with chips set up on
 * mx_junior_host; a display adapter gives the picture through
 * mx_junior_set_display.
 *
 * Portable C11. MezinXT's own code. */
#ifndef MX_JUNIOR_H
#define MX_JUNIOR_H

#include "bus.h"
#include "hw.h"
#include "sched.h"

#include <stddef.h>

typedef struct mx_junior mx_junior;

/* What the DIP switches say of the display (SW5-6). */
enum { MX_JUNIOR_DISPLAY_NONE, MX_JUNIOR_DISPLAY_CGA40, MX_JUNIOR_DISPLAY_CGA80, MX_JUNIOR_DISPLAY_MDA };
/* The processor. */
enum { MX_JUNIOR_8086, MX_JUNIOR_8088 };

typedef struct mx_junior_options {
    int cpu;                    /* MX_JUNIOR_8086 or MX_JUNIOR_8088 */
    uint32_t clock_num, clock_den; /* OSC periods per CPU clock: 3/1 (4.77 MHz), 2/1, 3/2 */
    uint32_t ram_kb;            /* 64, 128, 192, 256 (the board's banks) or 640 (with the 384 KiB module) */
    unsigned drives;            /* floppy drives, 1-4 */
    int display;                /* MX_JUNIOR_DISPLAY_* */
    int fpu;                    /* an 8087 is fitted */
} mx_junior_options;

/* A board running `bios` (64 KiB at F0000h, copied), powered on. NULL with
 * the reason. */
mx_junior *mx_junior_create(const mx_junior_options *options, const uint8_t *bios, size_t bios_size, char *error, size_t capacity);
void mx_junior_destroy(mx_junior *j);
/* Power on (memory cleared) or the RESET line (memory kept). */
void mx_junior_reset(mx_junior *j, int power_on);

/* Runs until guest time has advanced by `ticks` OSC periods (the last
 * instruction may end past it). Returns 1 when the CPU is halted with
 * interrupts disabled at the end. */
int mx_junior_run(mx_junior *j, uint64_t ticks);
uint64_t mx_junior_time(const mx_junior *j);
/* The wait states of a cycle on a window marked MX_BUS_IOCHRDY whose chip's
 * release can be sampled `release` OSC periods after the start of T2 (the
 * board's READY logic; junior.c). */
unsigned mx_junior_ready_waits(const mx_junior *j, uint32_t release);

/* Expansion: the bus to map windows and routes on, the host to set chips
 * up on, the display adapter's picture. */
mx_bus *mx_junior_bus(mx_junior *j);
const mx_hw_host *mx_junior_host(mx_junior *j);
struct mx_frame;
typedef struct mx_junior_display {
    void *ctx;
    int (*frame)(void *ctx, struct mx_frame *frame);    /* core/machine.h's frame; 0 when blank */
} mx_junior_display;
void mx_junior_set_display(mx_junior *j, const mx_junior_display *display);
int mx_junior_frame(mx_junior *j, struct mx_frame *frame);

/* The keyboard (core/machine.h's mx_machine_key) and the speaker's sound
 * (mx_speaker_render up to the board's time), with the drives' sound (drivesnd.h) mixed in when it is on. */
int mx_junior_key(mx_junior *j, uint8_t make_code, int down);
size_t mx_junior_audio(mx_junior *j, int16_t *stereo, size_t frames);
/* Free running (the machine runs faster than the clock): the speaker's frames up to the board's time are dropped and
 * `frames` frames of the drive sound's summary written (drivesnd.h), at the pace the caller pulls. */
size_t mx_junior_audio_free(mx_junior *j, int16_t *stereo, size_t frames);
/* The drive sound source the cards' drives report to (mx_drivesnd_event as their hook), and its switch: on at
 * `volume` percent (0 to 100), or off. */
struct mx_drivesnd;
struct mx_drivesnd *mx_junior_drivesnd(mx_junior *j);
void mx_junior_set_drive_sound(mx_junior *j, int enabled, unsigned volume);

/* Checks: a trace of I/O cycles, code fetches and interrupt acknowledges
 * (kinds as core/machine.h's MX_TRACE_*), a side-effect-free look at
 * memory, the DIP switches, a RAM parity error as the parity checker would
 * report it. */
typedef void (*mx_junior_trace_fn)(void *ctx, int kind, uint32_t address, uint32_t value);
#define MX_JUNIOR_TRACE_IO_READ   0
#define MX_JUNIOR_TRACE_IO_WRITE  1
#define MX_JUNIOR_TRACE_FETCH     2
#define MX_JUNIOR_TRACE_INTERRUPT 3
void mx_junior_set_trace(mx_junior *j, mx_junior_trace_fn fn, void *ctx);
uint8_t mx_junior_peek(mx_junior *j, uint32_t address);
uint8_t mx_junior_switches(const mx_junior *j);
void mx_junior_parity_error(mx_junior *j);

#endif /* MX_JUNIOR_H */

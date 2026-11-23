/* MezinXT - the game control adapter (the joystick
 * coupler; a reconstruction of the IBM Game Control Adapter, which the
 * catalogue's joystick coupler is not documented beyond).
 *
 * Board interface (hw.h): one I/O window, 200h-207h (MX_GAMEPORT_IO_*); every
 * address of it answers alike, as on the IBM card.
 *   write  any value: fires the four one-shots (the timer chip's trigger).
 *          Each axis output rises and stays high for 24.2 us + 0.011 us per
 *          ohm of the joystick's potentiometer, R from 0 to 100 kilohms
 *          (IBM Technical Reference: R_ext x 0.011 us + 24.2 us). A write
 *          while a pulse runs starts all four again.
 *   read   bit 0 joystick A X axis, bit 1 A Y, bit 2 joystick B X, bit 3 B Y:
 *          1 while that axis' one-shot is high (0 before the first write);
 *          bit 4 A button 1, bit 5 A button 2, bit 6 B button 1, bit 7 B
 *          button 2: 0 while pressed, 1 released or with nothing plugged in.
 *
 * The one-shots have no state of their own: the bit is a function of the
 * time of the last write and of `now`, so the chip schedules nothing. An
 * axis with nothing plugged in (open circuit) never times out: its bit
 * stays high until the next write, as a game port without a joystick reads.
 *
 * The host sets each axis' resistance and the buttons from the joystick it
 * has (core/stick.h gives the resistance of a stick position).
 *
 * Portable C11. MezinXT's own code. */
#ifndef MX_GAMEPORT_H
#define MX_GAMEPORT_H

#include "hw.h"

#define MX_GAMEPORT_IO_BASE 0x200u
#define MX_GAMEPORT_IO_SIZE 8u
#define MX_GAMEPORT_AXES 4
#define MX_GAMEPORT_OPEN 0xFFFFFFFFu            /* a resistance: nothing plugged in */
#define MX_GAMEPORT_MAX_OHMS 100000u

/* Axis numbers: the bit of the port they read on. */
enum { MX_GAMEPORT_AX, MX_GAMEPORT_AY, MX_GAMEPORT_BX, MX_GAMEPORT_BY };

typedef struct mx_gameport {
    mx_dev dev;
    uint64_t trigger;                   /* OSC time of the last write */
    uint8_t fired;                      /* a write has happened since power on */
    uint8_t buttons;                    /* bit n: button n pressed (A1, A2, B1, B2) */
    uint32_t ohms[MX_GAMEPORT_AXES];    /* the pots' resistances, or MX_GAMEPORT_OPEN */
    uint64_t width[MX_GAMEPORT_AXES];   /* the pulses' lengths in OSC periods; MX_NEVER: open */
} mx_gameport;

/* Nothing plugged in (every axis open, no button pressed). */
void mx_gameport_init(mx_gameport *g, const mx_hw_host *host, const char *name);
/* The length of the pulse of a pot of `ohms`, in OSC periods (rounded to the
 * nearest); MX_NEVER for MX_GAMEPORT_OPEN. */
uint64_t mx_gameport_pulse(uint32_t ohms);
/* A pot's resistance, or MX_GAMEPORT_OPEN. Resistances above the pot's 100 kilohms are cut to it. */
void mx_gameport_set_axis(mx_gameport *g, unsigned axis, uint32_t ohms);
/* Buttons pressed, bit n of `mask` for button n (A1, A2, B1, B2). */
void mx_gameport_set_buttons(mx_gameport *g, unsigned mask);

#endif /* MX_GAMEPORT_H */

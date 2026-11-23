/* MezinXT - the PC/XT keyboard and its interface: the IBM PC/XT board's circuit, which the FEPER Junior repeats.
 *
 * The keyboard sends each scan code of set 1 as a serial frame; the board
 * shifts it into a 74LS322 shift register that the CPU reads through the
 * 8255's port A (while port B bit 7 is low; high, the register is held
 * clear and reads 00h: the Junior reads its DIP switches through port C),
 * and a flip-flop
 * raises IRQ1 when a frame is complete. Writing port B bit 7 high clears
 * the shift register and the IRQ flip-flop (the BIOS pulses it after every
 * read); port B bit 6 low holds the keyboard clock low, which stops the
 * keyboard, and a clock held low for 20 ms or more and then released
 * resets the keyboard, which answers AAh (basic assurance test passed).
 * The keyboard does not send while the interface still holds an unread
 * byte or while port B bit 7 is high: it waits in its own buffer.
 *
 * The keyboard model: a 16-code buffer (a 17th code is replaced by the
 * overrun code FFh and later ones are lost), make codes on key down and
 * make + 80h on key up, typematic repeat of the last key pressed (500 ms
 * delay, 10.9 characters per second; a release of that key ends it, an
 * earlier key still held does not resume), a frame of 9 bit times at 10 kHz.
 * The keyboard powers up with the machine: it answers AAh a little after
 * reset as well. All timings are public fields in OSC periods (MX_XTKBD_US)
 * that the board may change after mx_xtkbd_init.
 *
 * Pins, as the board connects them to the 8255's port B (ops->input) and
 * its port A and the 8259 (host->line); state is the pin's level, 1 high:
 *  - MX_XTKBD_IN_CLOCK  port B bit 6: 0 holds the keyboard clock low.
 *  - MX_XTKBD_IN_CLEAR  port B bit 7: 1 clears the shift register and IRQ1
 *    (and the interface stays cleared while it is high).
 *  - MX_XTKBD_OUT_IRQ   IRQ1: 1 from the end of a frame until the clear.
 *  - MX_XTKBD_OUT_DATA(bit)  the shift register's bit (bit 0-7), reported
 *    through line when it changes; the board loads port A's input pins from
 *    it (or reads it with mx_xtkbd_data) while port B bit 7 is low.
 * After reset the interface sees both inputs low (the 8255 drives no pin,
 * and reports its undriven pins low), so the board forwards changes only.
 * The scheduler calls ops->event at the chip's deadline.
 *
 * Portable C11. MezinXT's own code. */
#ifndef MX_XTKBD_H
#define MX_XTKBD_H

#include "hw.h"

#define MX_XTKBD_IN_CLOCK    0
#define MX_XTKBD_IN_CLEAR    1
#define MX_XTKBD_OUT_IRQ     0
#define MX_XTKBD_OUT_DATA(b) (8 + (b))

/* OSC periods in n microseconds (the crystal is 157.5/11 MHz). */
#define MX_XTKBD_US(n) ((uint64_t)(n) * 315u / 22u)

typedef struct mx_xtkbd {
    mx_dev dev;
    /* timing in OSC periods, defaults set by init */
    uint64_t frame_time;        /* one frame on the wire: 900 us */
    uint64_t reset_hold;        /* clock low this long resets the keyboard: 20 ms */
    uint64_t bat_delay;         /* reset (or power on) to the AAh code: 10 ms */
    uint64_t repeat_delay;      /* key down to the first repeat: 500 ms */
    uint64_t repeat_period;     /* between repeats: 1 / 10.9 s */
    /* the interface */
    uint8_t sr;                 /* the shift register */
    uint8_t irq;                /* the IRQ1 flip-flop */
    uint8_t clock_high, clear;  /* the port B bit 6 and bit 7 levels */
    uint8_t reported_sr, reported_irq;
    uint64_t clock_low_since;
    /* the keyboard */
    uint8_t fifo[17];           /* fifo[0] is the code being sent while sending */
    uint8_t count;
    uint8_t sending;
    uint8_t down[16];           /* keys held, one bit per make code */
    uint8_t repeat_key;         /* make code repeating, 0 none */
    uint64_t t_frame, t_repeat, t_bat;    /* deadlines, MX_NEVER when unused */
} mx_xtkbd;

/* Fills in the chip in its power-on state; no host call is made. The board
 * then calls dev.ops->reset to start the keyboard's power-on sequence. */
void mx_xtkbd_init(mx_xtkbd *k, const mx_hw_host *host, const char *name);
/* A key of the 83-key keyboard pressed (down 1) or released: its set 1 make
 * code (01h-53h), as core/machine.h's mx_machine_key. 0 when the keyboard's
 * 16-code buffer is full (offer it again later); a key pressed while the
 * keyboard is resetting is lost (returns 1); a repeat of a key already down
 * is ignored (the keyboard's own typematic does the repeating). */
int mx_xtkbd_key(mx_xtkbd *k, uint8_t make_code, int down);
/* The shift register and the IRQ1 flip-flop. */
uint8_t mx_xtkbd_data(const mx_xtkbd *k);
int mx_xtkbd_irq(const mx_xtkbd *k);

#endif /* MX_XTKBD_H */

/* MezinXT - the Microsoft serial mouse (catalogue item 703.812, a
 * reconstruction: the manual names the mouse only), on an 8250 port.
 *
 * The protocol as Microsoft documented it for its serial mouse. The line is
 * 1200 baud, 7 data bits, no parity, one stop bit; the mouse takes its power
 * from the host's DTR and RTS lines and does nothing while both are low.
 * When it powers up, or RTS rises while it is powered, it resets and sends
 * one byte, 'M' (4Dh), so that a driver can tell a mouse is there. Then it
 * sends a packet of three bytes whenever the position or a button changed:
 *
 *   byte 1   1 L R Y7 Y6 X7 X6     bit 6 is the sync bit; L and R are the left
 *                                  and right buttons (1 pressed)
 *   byte 2   0 X5 X4 X3 X2 X1 X0   the low six bits of the horizontal delta
 *   byte 3   0 Y5 Y4 Y3 Y2 Y1 Y0   the low six bits of the vertical delta
 *
 * The deltas are 8-bit two's-complement counts since the last packet (X
 * positive to the right, Y positive down). The mouse accumulates the motion
 * it has not reported: a packet carries at most -128..127 of it and the rest
 * waits for the next packet, so no motion is lost. How fast packets follow
 * each other is the line's doing: the 8250 collects the next byte only when
 * its receiver is idle, so at 1200 baud and nine bits a character a packet
 * takes 22.5 ms (44 a second).
 *
 * mx_sermouse is the protocol alone (bytes in, bytes out). mx_sermouse_attach
 * puts it behind an 8250's connector as its byte end: bytes go to the UART
 * only while the guest has the receiver at 1200 baud, 7 data bits and no
 * parity (any other setting would garble the mouse's bytes; it sends none),
 * and the UART's RTS and DTR pins are the mouse's power.
 *
 * Portable C11. MezinXT's own code, from the Microsoft serial mouse protocol
 * as publicly documented. */
#ifndef MX_SERMOUSE_H
#define MX_SERMOUSE_H

#include "ns8250.h"

#define MX_SERMOUSE_LEFT  1u
#define MX_SERMOUSE_RIGHT 2u

typedef struct mx_sermouse {
    mx_ns8250 *uart;                    /* the port it is plugged into (NULL: none) */
    uint8_t dtr, rts, powered;
    int acc_x, acc_y;                   /* motion not reported yet */
    unsigned buttons, reported;         /* the host's buttons, and the last packet's */
    uint8_t queue[4];
    unsigned head, count;
} mx_sermouse;

/* An unpowered mouse. */
void mx_sermouse_init(mx_sermouse *m);
/* The host's motion since the last call (relative counts) and its buttons
 * (bit 0 left, bit 1 right). */
void mx_sermouse_motion(mx_sermouse *m, int dx, int dy, unsigned buttons);
/* The DTR and RTS lines (1 asserted): power, and reset on a power-up or an
 * RTS rising edge. */
void mx_sermouse_modem(mx_sermouse *m, int dtr, int rts);
/* 1 when a byte is waiting to be sent. */
int mx_sermouse_pending(const mx_sermouse *m);
/* The next byte on the line: 1 and *byte, or 0 for none. */
int mx_sermouse_next(mx_sermouse *m, uint8_t *byte);
/* Plugs the mouse into an 8250: its byte end, as described above. The port's
 * flow control should stay on. */
void mx_sermouse_attach(mx_sermouse *m, mx_ns8250 *uart);

#endif /* MX_SERMOUSE_H */

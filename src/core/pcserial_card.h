/* The PC/XT asynchronous communications adapter as cards of the Junior's
 * board (a reconstruction: the catalogue has no such board,
 * the BIOS 3.0 and 5.0 probe for one): one or two INS8250s (hw/ns8250.h) on
 * the board's host and bus, their connectors handed to the machine
 * interface's byte ports (machine.h: MX_PORT_COM1, MX_PORT_COM2).
 *
 *     COM1   I/O 3F8h-3FFh, IRQ 4
 *     COM2   I/O 2F8h-2FFh, IRQ 3
 *
 * The BIOS finds a port by the interrupt identification register (3FAh,
 * 2FAh) and lists it in the equipment word and the BIOS data area. COM1 and
 * the SIO adapter (core/ports_card.h) both drive IRQ 4, which a second card
 * cannot share, so the board takes either: the machine
 * refuses the pair (mx_config_check).
 *
 * The host side is mx_serial_host as for the SIO adapter: `transmit` for each
 * byte a port sends, `outputs` for RTS and DTR; bytes come in through
 * mx_pcserial_card_receive (a small queue per port, the UART takes them as its
 * receiver has room), the host's modem lines through mx_pcserial_card_modem.
 * A port whose end is a loopback plug has its TxD wired to its RxD; one whose
 * end is the mouse (mx_serial_host.mouse) has the Microsoft serial mouse
 * (hw/sermouse.h) behind it, fed by mx_pcserial_card_mouse.
 *
 * MezinXT's own code. */
#ifndef MX_PCSERIAL_CARD_H
#define MX_PCSERIAL_CARD_H

#include "machine.h"
#include "../hw/junior.h"
#include "../hw/ns8250.h"
#include "../hw/sermouse.h"

/* Bytes from the host a port keeps waiting for its receiver. */
#define MX_PCSERIAL_RX 256u

typedef struct mx_pcserial_card {
    unsigned count;                     /* 1 or 2 ports */
    mx_ns8250 uart[2];
    mx_sermouse mouse;
    int mouse_unit;                     /* the port the mouse is plugged into, -1 none */
    mx_serial_host host;
    int have_host;
    uint8_t rx[2][MX_PCSERIAL_RX];
    unsigned rx_head[2], rx_count[2];
} mx_pcserial_card;

/* Puts `count` (1 or 2) ports on the board (windows and routes). NULL when out of memory or the bus is full. */
mx_pcserial_card *mx_pcserial_card_attach(mx_junior *j, unsigned count);
/* Frees the card: after the board (mx_junior_destroy), whose bus points into it. */
void mx_pcserial_card_free(mx_pcserial_card *card);
/* The host's ends (copied; NULL: nothing connected). The modem inputs fall back to deasserted and bytes waiting
 * are dropped. */
void mx_pcserial_card_set_host(mx_pcserial_card *card, const mx_serial_host *host);
/* Bytes from the host for a port; returns how many were taken (the rest is offered again later). A port that is
 * not here, or whose end is the mouse, takes all and drops it. */
size_t mx_pcserial_card_receive(mx_pcserial_card *card, unsigned port, const uint8_t *data, size_t count);
/* The host's modem lines for a port. */
void mx_pcserial_card_modem(mx_pcserial_card *card, unsigned port, int dcd, int cts, int dsr, int ri);
/* The host mouse's motion and buttons, for the port whose end is the mouse. */
void mx_pcserial_card_mouse(mx_pcserial_card *card, int dx, int dy, unsigned buttons);

#endif /* MX_PCSERIAL_CARD_H */

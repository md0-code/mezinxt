/* The serial and parallel adapter as a card of the Junior's board: the SIO adapter (hw/sio_card.h) and the printer port
 * (hw/lpt.h) on the board's host and bus, their connectors handed to the
 * machine interface's byte ports (machine.h: MX_PORT_SIO_A, MX_PORT_SIO_B,
 * MX_PORT_LPT).
 *
 *     SIO adapter   I/O 250h-25Fh, IRQ 4 (the strap's default), one I/O wait
 *                   state of its own on top of the board's
 *     printer port  I/O 378h-37Bh (the strap's other choice is 278h), IRQ 7
 *
 * BIOS 4.0 probes the SIO at 253h and the printer at 3BCh, 378h and 278h (a
 * byte written to the data latch must read back), so the printer is its
 * printer 1.
 *
 * The host side is mx_serial_host: the card calls `transmit` for each byte a
 * channel sends or the printer prints and `outputs` for RTS and DTR; bytes
 * come in through mx_ports_card_receive (held in a small queue per channel,
 * the SIO takes them as its receiver has room, so the host's own buffer is
 * the flow control), the host's modem lines through mx_ports_card_modem. A
 * channel whose end is a loopback plug (the `loopback` callback) has its TxD
 * wired to its RxD inside the SIO. A printer is connected and on line when
 * the host says so (a modem call with DSR 1 on MX_PORT_LPT); until then the
 * port reads off line, as with nothing on the connector.
 *
 * MezinXT's own code. */
#ifndef MX_PORTS_CARD_H
#define MX_PORTS_CARD_H

#include "machine.h"
#include "../hw/junior.h"
#include "../hw/lpt.h"
#include "../hw/sio_card.h"

/* Bytes from the host a channel keeps waiting for its receiver. */
#define MX_PORTS_RX 256u

typedef struct mx_ports_card {
    mx_junior *board;
    mx_sio_card sio;
    mx_lpt lpt;
    mx_serial_host host;
    int have_host;
    uint8_t rx[2][MX_PORTS_RX];
    unsigned rx_head[2], rx_count[2];
} mx_ports_card;

/* Puts the adapter on the board (windows and routes). NULL when out of
 * memory or the bus is full. */
mx_ports_card *mx_ports_card_attach(mx_junior *j);
/* Frees the card: after the board (mx_junior_destroy), whose bus points into it. */
void mx_ports_card_free(mx_ports_card *card);
/* The host's ends (copied; NULL: nothing connected). The channels' modem
 * inputs fall back to deasserted and the printer to off line until the host
 * reports its lines, and bytes waiting are dropped. */
void mx_ports_card_set_host(mx_ports_card *card, const mx_serial_host *host);
/* Bytes from the host for a port; returns how many were taken (the rest is
 * offered again later). Everything but an SIO channel takes all and drops it. */
size_t mx_ports_card_receive(mx_ports_card *card, unsigned port, const uint8_t *data, size_t count);
/* The host's modem lines for a port. */
void mx_ports_card_modem(mx_ports_card *card, unsigned port, int dcd, int cts, int dsr, int ri);

#endif /* MX_PORTS_CARD_H */

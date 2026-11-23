/* MezinXT - the Junior's bus: memory map, I/O decode and the expansion
 * slots' registration.
 *
 * Memory: the 1 MiB space in 2 KiB pages. A page is direct (RAM, or ROM
 * that ignores writes), a chip's window, or open bus (reads 0xFF). The CPU's
 * hot path reads and writes direct pages through mx_bus_mem_read and friends
 * without a call.
 *
 * I/O: the PC/XT decodes ten address bits (A0-A9), so port n answers at n +
 * 400h, n + 800h... as well; the 1024 ports map to chip windows or open bus.
 *
 * A window hands its chip the cycle's offset from the window's first
 * address (hw.h). Chips are 8-bit unless their window has MX_BUS_WORD: an
 * 8086 word cycle on an 8-bit chip becomes two byte cycles, low byte first.
 *
 * The slots: a card is any chip (hw.h) set up on the board's host
 * (mx_junior_host). It joins the bus by mapping its windows and routing its
 * output pins:
 *
 *     mx_bus_map_io(bus, &card.dev, 0x3D0, 0x3DF, 0);
 *     mx_bus_map_mem(bus, &card.dev, 0xB8000, 0xBFFFF, MX_BUS_WORD);
 *     mx_bus_route(bus, &card.dev, CARD_IRQ_PIN, MX_ROUTE_IRQ, 5);
 *     mx_bus_route(bus, &card.dev, CARD_DRQ_PIN, MX_ROUTE_DRQ, 2);
 *
 * An IRQ route drives the 8259's IR input; a DRQ route the 8237's DREQ and
 * makes the card the device on that channel's DACK (its ops->dack moves the
 * bytes); an IOCHK route the I/O channel check (an NMI when enabled). Every
 * chip with a window or a route is reset with the board. The chip's ops->wait
 * holds its cycles (READY) beyond the board's own wait states, or, with
 * MX_BUS_IOCHRDY, through I/O CH RDY.
 *
 * Portable C11. MezinXT's own code. */
#ifndef MX_BUS_H
#define MX_BUS_H

#include "hw.h"

#define MX_BUS_PAGE_BITS 11u
#define MX_BUS_PAGE_SIZE (1u << MX_BUS_PAGE_BITS)
#define MX_BUS_PAGES     (0x100000u >> MX_BUS_PAGE_BITS)
#define MX_BUS_PORTS     1024u
#define MX_BUS_WINDOWS   64
#define MX_BUS_ROUTES    32
#define MX_BUS_CHIPS     32

/* Window flags. MX_BUS_WORD: the chip takes 8086 word cycles whole.
 * MX_BUS_IOCHRDY: the chip holds its cycles by pulling I/O CH RDY low from
 * the command's leading edge; its ops->wait gives the OSC periods from then
 * until the release can be sampled by the board (its propagation and the
 * READY synchroniser's setup included), and the board turns that into whole
 * wait states as its READY logic does (mx_junior_ready_waits). */
enum { MX_BUS_WORD = 1, MX_BUS_IOCHRDY = 2 };
/* Route kinds. */
enum { MX_ROUTE_IRQ, MX_ROUTE_DRQ, MX_ROUTE_IOCHK };

typedef struct mx_bus_page {
    uint8_t *data;              /* direct memory (NULL: a window or open bus) */
    uint8_t writable;           /* direct: RAM (1) or ROM (0) */
    uint8_t window;             /* 0: open bus; else 1 + the window's index */
} mx_bus_page;

typedef struct mx_bus_window {
    mx_dev *dev;
    uint32_t first, last;
    unsigned flags;
} mx_bus_window;

typedef struct mx_bus_route_entry {
    mx_dev *dev;
    int pin, kind, number;
} mx_bus_route_entry;

typedef struct mx_bus {
    mx_bus_page page[MX_BUS_PAGES];
    uint8_t port[MX_BUS_PORTS];             /* 0: open bus; else 1 + window index */
    mx_bus_window window[MX_BUS_WINDOWS];
    unsigned windows;
    mx_bus_route_entry route[MX_BUS_ROUTES];
    unsigned routes;
    mx_dev *chip[MX_BUS_CHIPS];             /* every chip with a window or a route, once */
    unsigned chips;
} mx_bus;

void mx_bus_init(mx_bus *b);
/* Direct memory: `size` bytes of `data` from `first` (both page-aligned);
 * ROM ignores writes. The data stays the caller's. 0 when misaligned. */
int mx_bus_map_direct(mx_bus *b, uint32_t first, uint32_t size, uint8_t *data, int writable);
/* A chip's windows (inclusive bounds; memory windows page-aligned, a whole
 * number of pages). A later window replaces what was there. 0 when the
 * table is full or the bounds are wrong. */
int mx_bus_map_mem(mx_bus *b, mx_dev *dev, uint32_t first, uint32_t last, unsigned flags);
int mx_bus_map_io(mx_bus *b, mx_dev *dev, uint32_t first, uint32_t last, unsigned flags);
/* A chip's output pin to a line of the bus (MX_ROUTE_*: IRQ 0-7, DRQ 0-3,
 * IOCHK). The board applies the DMA side (mx_junior: through its host). */
int mx_bus_route(mx_bus *b, mx_dev *dev, int pin, int kind, int number);
/* The route of a chip's pin, or NULL. */
const mx_bus_route_entry *mx_bus_route_of(const mx_bus *b, const mx_dev *dev, int pin);

/* Cycles. Memory addresses are 20 bits, ports 16 (decoded on 10). `now` is
 * for a window's wait query only. */
uint8_t mx_bus_mem_read_slow(mx_bus *b, uint32_t address);
void mx_bus_mem_write_slow(mx_bus *b, uint32_t address, uint8_t value);
uint16_t mx_bus_mem_read16(mx_bus *b, uint32_t address);
void mx_bus_mem_write16(mx_bus *b, uint32_t address, uint16_t value);
uint8_t mx_bus_io_read(mx_bus *b, uint16_t port);
void mx_bus_io_write(mx_bus *b, uint16_t port, uint8_t value);
uint16_t mx_bus_io_read16(mx_bus *b, uint16_t port);
void mx_bus_io_write16(mx_bus *b, uint16_t port, uint16_t value);
/* Extra OSC periods a window's chip holds a cycle whose command goes active
 * at `now` (ops->wait); 0 for direct memory, open bus and chips without wait
 * states. */
uint32_t mx_bus_wait(mx_bus *b, unsigned space, uint32_t address, int write, uint64_t now);
/* The window that answers an address (NULL: direct memory or open bus). */
const mx_bus_window *mx_bus_window_at(const mx_bus *b, unsigned space, uint32_t address);

static inline uint8_t mx_bus_mem_read(mx_bus *b, uint32_t address) {
    const mx_bus_page *p = &b->page[(address & 0xFFFFFu) >> MX_BUS_PAGE_BITS];
    return p->data ? p->data[address & (MX_BUS_PAGE_SIZE - 1u)] : mx_bus_mem_read_slow(b, address);
}
static inline void mx_bus_mem_write(mx_bus *b, uint32_t address, uint8_t value) {
    const mx_bus_page *p = &b->page[(address & 0xFFFFFu) >> MX_BUS_PAGE_BITS];
    if (p->data) { if (p->writable) p->data[address & (MX_BUS_PAGE_SIZE - 1u)] = value; }
    else mx_bus_mem_write_slow(b, address, value);
}

#endif /* MX_BUS_H */

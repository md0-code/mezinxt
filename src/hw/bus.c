/* MezinXT - the Junior's bus (bus.h). The ten-bit I/O decode and the
 * splitting of word cycles for 8-bit chips follow the IBM PC/XT Technical
 * Reference's description of the I/O channel, which the Junior's manual
 * (chapter 1) says its board repeats. MezinXT's own code. */
#include "bus.h"

#include <string.h>

void mx_bus_init(mx_bus *b) { memset(b, 0, sizeof *b); }

static void add_chip(mx_bus *b, mx_dev *dev) {
    for (unsigned i = 0; i < b->chips; ++i)
        if (b->chip[i] == dev) return;
    if (b->chips < MX_BUS_CHIPS) b->chip[b->chips++] = dev;
}

int mx_bus_map_direct(mx_bus *b, uint32_t first, uint32_t size, uint8_t *data, int writable) {
    if ((first | size) & (MX_BUS_PAGE_SIZE - 1u) || first + size > 0x100000u) return 0;
    for (uint32_t a = 0; a < size; a += MX_BUS_PAGE_SIZE) {
        mx_bus_page *p = &b->page[(first + a) >> MX_BUS_PAGE_BITS];
        p->data = data + a;
        p->writable = (uint8_t)(writable != 0);
        p->window = 0;
    }
    return 1;
}

static int add_window(mx_bus *b, mx_dev *dev, uint32_t first, uint32_t last, unsigned flags) {
    if (b->windows == MX_BUS_WINDOWS || last < first) return 0;
    mx_bus_window *w = &b->window[b->windows++];
    w->dev = dev;
    w->first = first;
    w->last = last;
    w->flags = flags;
    add_chip(b, dev);
    return (int)b->windows;     /* 1 + index */
}

int mx_bus_map_mem(mx_bus *b, mx_dev *dev, uint32_t first, uint32_t last, unsigned flags) {
    if (first & (MX_BUS_PAGE_SIZE - 1u) || (last + 1u) & (MX_BUS_PAGE_SIZE - 1u) || last > 0xFFFFFu) return 0;
    const int w = add_window(b, dev, first, last, flags);
    if (!w) return 0;
    for (uint32_t a = first; a <= last; a += MX_BUS_PAGE_SIZE) {
        mx_bus_page *p = &b->page[a >> MX_BUS_PAGE_BITS];
        p->data = NULL;
        p->writable = 0;
        p->window = (uint8_t)w;
    }
    return 1;
}

int mx_bus_map_io(mx_bus *b, mx_dev *dev, uint32_t first, uint32_t last, unsigned flags) {
    if (last >= MX_BUS_PORTS) return 0;
    const int w = add_window(b, dev, first, last, flags);
    if (!w) return 0;
    for (uint32_t p = first; p <= last; ++p) b->port[p] = (uint8_t)w;
    return 1;
}

int mx_bus_route(mx_bus *b, mx_dev *dev, int pin, int kind, int number) {
    for (unsigned i = 0; i < b->routes; ++i)
        if (b->route[i].dev == dev && b->route[i].pin == pin) {
            b->route[i].kind = kind;
            b->route[i].number = number;
            return 1;
        }
    if (b->routes == MX_BUS_ROUTES) return 0;
    if ((kind == MX_ROUTE_IRQ && (number < 0 || number > 7)) || (kind == MX_ROUTE_DRQ && (number < 0 || number > 3))) return 0;
    mx_bus_route_entry *r = &b->route[b->routes++];
    r->dev = dev;
    r->pin = pin;
    r->kind = kind;
    r->number = number;
    add_chip(b, dev);
    return 1;
}

const mx_bus_route_entry *mx_bus_route_of(const mx_bus *b, const mx_dev *dev, int pin) {
    for (unsigned i = 0; i < b->routes; ++i)
        if (b->route[i].dev == dev && b->route[i].pin == pin) return &b->route[i];
    return NULL;
}

/* --- cycles --------------------------------------------------------------------------- */

const mx_bus_window *mx_bus_window_at(const mx_bus *b, unsigned space, uint32_t address) {
    const unsigned w = space == MX_SPACE_IO ? b->port[address & (MX_BUS_PORTS - 1u)]
                                            : b->page[(address & 0xFFFFFu) >> MX_BUS_PAGE_BITS].window;
    return w ? &b->window[w - 1u] : NULL;
}

uint8_t mx_bus_mem_read_slow(mx_bus *b, uint32_t address) {
    address &= 0xFFFFFu;
    const mx_bus_page *p = &b->page[address >> MX_BUS_PAGE_BITS];
    if (p->data) return p->data[address & (MX_BUS_PAGE_SIZE - 1u)];
    if (!p->window) return 0xFF;
    const mx_bus_window *w = &b->window[p->window - 1u];
    return (uint8_t)w->dev->ops->read(w->dev, MX_SPACE_MEM, address - w->first, 1);
}

void mx_bus_mem_write_slow(mx_bus *b, uint32_t address, uint8_t value) {
    address &= 0xFFFFFu;
    const mx_bus_page *p = &b->page[address >> MX_BUS_PAGE_BITS];
    if (p->data) { if (p->writable) p->data[address & (MX_BUS_PAGE_SIZE - 1u)] = value; return; }
    if (!p->window) return;
    const mx_bus_window *w = &b->window[p->window - 1u];
    w->dev->ops->write(w->dev, MX_SPACE_MEM, address - w->first, 1, value);
}

/* An 8086 word cycle at an even address: both bytes lie in one page. */
uint16_t mx_bus_mem_read16(mx_bus *b, uint32_t address) {
    address &= 0xFFFFFu;
    const mx_bus_page *p = &b->page[address >> MX_BUS_PAGE_BITS];
    if (p->data) return mx_get16(p->data + (address & (MX_BUS_PAGE_SIZE - 1u)));
    if (p->window) {
        const mx_bus_window *w = &b->window[p->window - 1u];
        if (w->flags & MX_BUS_WORD) return (uint16_t)w->dev->ops->read(w->dev, MX_SPACE_MEM, address - w->first, 2);
    }
    return (uint16_t)(mx_bus_mem_read_slow(b, address) | mx_bus_mem_read_slow(b, address + 1u) << 8);
}

void mx_bus_mem_write16(mx_bus *b, uint32_t address, uint16_t value) {
    address &= 0xFFFFFu;
    const mx_bus_page *p = &b->page[address >> MX_BUS_PAGE_BITS];
    if (p->data) { if (p->writable) mx_put16(p->data + (address & (MX_BUS_PAGE_SIZE - 1u)), value); return; }
    if (p->window) {
        const mx_bus_window *w = &b->window[p->window - 1u];
        if (w->flags & MX_BUS_WORD) { w->dev->ops->write(w->dev, MX_SPACE_MEM, address - w->first, 2, value); return; }
    }
    mx_bus_mem_write_slow(b, address, (uint8_t)value);
    mx_bus_mem_write_slow(b, address + 1u, (uint8_t)(value >> 8));
}

uint8_t mx_bus_io_read(mx_bus *b, uint16_t port) {
    const unsigned p = port & (MX_BUS_PORTS - 1u), wi = b->port[p];
    if (!wi) return 0xFF;
    const mx_bus_window *w = &b->window[wi - 1u];
    return (uint8_t)w->dev->ops->read(w->dev, MX_SPACE_IO, p - w->first, 1);
}

void mx_bus_io_write(mx_bus *b, uint16_t port, uint8_t value) {
    const unsigned p = port & (MX_BUS_PORTS - 1u), wi = b->port[p];
    if (!wi) return;
    const mx_bus_window *w = &b->window[wi - 1u];
    w->dev->ops->write(w->dev, MX_SPACE_IO, p - w->first, 1, value);
}

uint16_t mx_bus_io_read16(mx_bus *b, uint16_t port) {
    const unsigned p = port & (MX_BUS_PORTS - 1u), wi = b->port[p];
    if (wi && (b->window[wi - 1u].flags & MX_BUS_WORD) && b->port[(p + 1u) & (MX_BUS_PORTS - 1u)] == wi) {
        const mx_bus_window *w = &b->window[wi - 1u];
        return (uint16_t)w->dev->ops->read(w->dev, MX_SPACE_IO, p - w->first, 2);
    }
    return (uint16_t)(mx_bus_io_read(b, port) | mx_bus_io_read(b, (uint16_t)(port + 1u)) << 8);
}

void mx_bus_io_write16(mx_bus *b, uint16_t port, uint16_t value) {
    const unsigned p = port & (MX_BUS_PORTS - 1u), wi = b->port[p];
    if (wi && (b->window[wi - 1u].flags & MX_BUS_WORD) && b->port[(p + 1u) & (MX_BUS_PORTS - 1u)] == wi) {
        const mx_bus_window *w = &b->window[wi - 1u];
        w->dev->ops->write(w->dev, MX_SPACE_IO, p - w->first, 2, value);
        return;
    }
    mx_bus_io_write(b, port, (uint8_t)value);
    mx_bus_io_write(b, (uint16_t)(port + 1u), (uint8_t)(value >> 8));
}

uint32_t mx_bus_wait(mx_bus *b, unsigned space, uint32_t address, int write, uint64_t now) {
    const mx_bus_window *w = mx_bus_window_at(b, space, address);
    if (!w || !w->dev->ops->wait) return 0;
    const uint32_t a = space == MX_SPACE_IO ? (address & (MX_BUS_PORTS - 1u)) : (address & 0xFFFFFu);
    return w->dev->ops->wait(w->dev, space, a - w->first, write, now);
}

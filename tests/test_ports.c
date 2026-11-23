/* The serial and parallel adapter on the board (core/ports_card.h): a small
 * program in a made-up ROM does what a guest does, through the bus's I/O
 * cycles and the 8086's own interrupt entry: it programs the 8259, the
 * adapter's baud 8253 and the SIO, sends three characters and takes them back
 * by IRQ 4 on a loopback plug. A character printed through the printer port
 * reaches the host. MezinXT's own code. */
#include "ports_card.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define CHECK_EQ(a, b) do { unsigned long long va_ = (a), vb_ = (b); if (va_ != vb_) { printf("FAIL %s:%d: %s = %llx, expected %llx\n", __FILE__, __LINE__, #a, va_, vb_); failures++; } } while (0)

#define OSC_US(n) ((uint64_t)(n) * 157500ull / 11000ull)

/* The guest: F000:E000, with its interrupt routine at E200 and the text it sends at E100. */
static const uint8_t PROGRAM[] = {
    0xFA, 0x31, 0xC0, 0x8E, 0xD0, 0xBC, 0x00, 0x70, 0x8E, 0xD8,         /* cli; ss = ds = 0; sp = 7000h */
    0xC7, 0x06, 0x30, 0x00, 0x00, 0xE2, 0xC7, 0x06, 0x32, 0x00, 0x00, 0xF0, /* vector 0Ch (IRQ 4) -> F000:E200 */
    0xB0, 0x13, 0xE6, 0x20, 0xB0, 0x08, 0xE6, 0x21,                     /* 8259: ICW1 (edge, ICW4), ICW2 = 08h */
    0xB0, 0x09, 0xE6, 0x21, 0xB0, 0xEF, 0xE6, 0x21,                     /* ICW4, only IRQ 4 unmasked */
    0xBA, 0x57, 0x02, 0xB0, 0x36, 0xEE,                                 /* baud 8253: counter 0, lsb/msb, mode 3 */
    0xBA, 0x54, 0x02, 0xB0, 0x0C, 0xEE, 0x30, 0xC0, 0xEE,               /* count 12 */
    0xBA, 0x53, 0x02, 0xB0, 0x02, 0xEE, 0xB0, 0x40, 0xEE,               /* SIO B: WR2 = 40h (the vector) */
    0xBA, 0x51, 0x02, 0xB0, 0x18, 0xEE,                                 /* SIO A: channel reset */
    0xB0, 0x04, 0xEE, 0xB0, 0x44, 0xEE,                                 /* WR4: x16 clock, one stop bit, no parity */
    0xB0, 0x03, 0xEE, 0xB0, 0xC1, 0xEE,                                 /* WR3: 8 bits, receiver on */
    0xB0, 0x05, 0xEE, 0xB0, 0xEA, 0xEE,                                 /* WR5: DTR, 8 bits, transmitter on, RTS */
    0xB0, 0x01, 0xEE, 0xB0, 0x18, 0xEE,                                 /* WR1: interrupt on every received character */
    0xFB,                                                               /* sti */
    0xBE, 0x00, 0xE1, 0xB9, 0x03, 0x00,                                 /* si = text, cx = 3 */
    0x2E, 0xAC, 0x88, 0xC3,                                             /* next: al = cs:[si++]; bl = al */
    0xBA, 0x51, 0x02, 0xEC, 0xA8, 0x04, 0x74, 0xFB,                     /* wait: in al, 251h; test al, 4; jz wait */
    0xBA, 0x50, 0x02, 0x88, 0xD8, 0xEE, 0xE2, 0xEC,                     /* out 250h, bl; loop next */
    0xF4, 0xEB, 0xFD,                                                   /* hlt; jmp hlt */
};
static const uint8_t TEXT[] = {0x55, 0xA5, 0x3C};
static const uint8_t HANDLER[] = {      /* F000:E200: the SIO's vector and character stored, its service ended, EOI */
    0x50, 0x52, 0x53, 0xBA, 0x58, 0x02, 0xEC, 0xA2, 0x20, 0x05,         /* push ax, dx, bx; in al, 258h; [520h] = al */
    0xBA, 0x50, 0x02, 0xEC, 0x88, 0xC4,                                 /* in al, 250h (the character); ah = al */
    0xBA, 0x51, 0x02, 0xB0, 0x38, 0xEE, 0x88, 0xE0,                     /* WR0: reset the highest IUS; al = ah */
    0x8B, 0x1E, 0x00, 0x05, 0x88, 0x87, 0x10, 0x05,                     /* bx = [500h]; [bx + 510h] = al */
    0x43, 0x89, 0x1E, 0x00, 0x05, 0xB0, 0x20, 0xE6, 0x20,               /* [500h] = bx + 1; EOI */
    0x5B, 0x5A, 0x58, 0xCF,                                             /* pop; iret */
};

/* --- the host's side ------------------------------------------------------------------------- */

typedef struct fake_host {
    mx_serial_host h;
    uint8_t loop[MX_PORT_COUNT];
    uint8_t sent[MX_PORT_COUNT][64];
    unsigned sent_count[MX_PORT_COUNT];
    int dtr[MX_PORT_COUNT], rts[MX_PORT_COUNT];
    unsigned output_calls;
} fake_host;

static void fh_transmit(void *ctx, unsigned port, uint8_t byte) {
    fake_host *f = ctx;
    if (port < MX_PORT_COUNT && f->sent_count[port] < sizeof f->sent[0]) f->sent[port][f->sent_count[port]++] = byte;
}
static void fh_outputs(void *ctx, unsigned port, int dtr, int rts) {
    fake_host *f = ctx;
    f->dtr[port] = dtr;
    f->rts[port] = rts;
    f->output_calls++;
}
static int fh_loopback(void *ctx, unsigned port) { return ((fake_host *)ctx)->loop[port]; }
static void fh_init(fake_host *f) {
    memset(f, 0, sizeof *f);
    f->h.ctx = f;
    f->h.transmit = fh_transmit;
    f->h.outputs = fh_outputs;
    f->h.loopback = fh_loopback;
}

/* --- the board ------------------------------------------------------------------------------ */

static unsigned vectors[256];
static void trace(void *ctx, int kind, uint32_t address, uint32_t value) {
    (void)ctx; (void)value;
    if (kind == MX_JUNIOR_TRACE_INTERRUPT && address < 256) vectors[address]++;
}

/* guest 0: the program above; 1: a CPU that stays halted with interrupts off (the test drives the bus itself). */
static mx_junior *make_board(mx_ports_card **card, int idle) {
    static uint8_t rom[65536];
    memset(rom, 0xF4, sizeof rom);
    if (idle) rom[0xE000] = 0xFA;
    else {
        memcpy(rom + 0xE000, PROGRAM, sizeof PROGRAM);
        memcpy(rom + 0xE100, TEXT, sizeof TEXT);
        memcpy(rom + 0xE200, HANDLER, sizeof HANDLER);
    }
    static const uint8_t reset[] = {0xEA, 0x00, 0xE0, 0x00, 0xF0};
    memcpy(rom + 0xFFF0, reset, sizeof reset);
    mx_junior_options o;
    memset(&o, 0, sizeof o);
    o.cpu = MX_JUNIOR_8086;
    o.clock_num = 3;
    o.clock_den = 1;
    o.ram_kb = 256;
    o.drives = 2;
    o.display = MX_JUNIOR_DISPLAY_NONE;
    char error[128];
    mx_junior *j = mx_junior_create(&o, rom, sizeof rom, error, sizeof error);
    CHECK(j != NULL);
    if (!j) exit(1);
    *card = mx_ports_card_attach(j);
    CHECK(*card != NULL);
    if (!*card) exit(1);
    memset(vectors, 0, sizeof vectors);
    mx_junior_set_trace(j, trace, NULL);
    return j;
}

static unsigned guest_count(mx_junior *j) { return mx_junior_peek(j, 0x500) | (unsigned)mx_junior_peek(j, 0x501) << 8; }

/* --- the loopback ---------------------------------------------------------------------------- */

static void test_loopback(void) {
    mx_ports_card *card;
    fake_host host;
    mx_junior *j = make_board(&card, 0);
    fh_init(&host);
    host.loop[MX_PORT_SIO_A] = 1;
    mx_junior_reset(j, 1);
    mx_ports_card_set_host(card, &host.h);
    mx_ports_card_modem(card, MX_PORT_SIO_A, 1, 1, 1, 0);       /* the loopback end's lines, as the host reports them */
    for (unsigned t = 0; t < 8000 && guest_count(j) < 3; t += 50) mx_junior_run(j, OSC_US(50));
    CHECK_EQ(guest_count(j), 3);
    CHECK(mx_junior_peek(j, 0x510) == 0x55 && mx_junior_peek(j, 0x511) == 0xA5 && mx_junior_peek(j, 0x512) == 0x3C);
    CHECK_EQ(vectors[0x0C], 3);                                 /* the 8259 gave the vector of IRQ 4, once a character */
    CHECK_EQ(host.sent_count[MX_PORT_SIO_A], 0);                /* the plug keeps the characters inside */
    mx_junior_destroy(j);
    mx_ports_card_free(card);
}

/* --- the printer port -------------------------------------------------------------------------- */

static void test_printer(void) {
    mx_ports_card *card;
    fake_host host;
    mx_junior *j = make_board(&card, 1);
    mx_bus *bus = mx_junior_bus(j);
    fh_init(&host);
    mx_junior_reset(j, 1);
    mx_ports_card_set_host(card, &host.h);
    mx_ports_card_modem(card, MX_PORT_LPT, 1, 1, 1, 0);         /* a printer on line */
    CHECK_EQ(mx_bus_io_read(bus, 0x379), 0xDF);                 /* ready */
    for (const char *p = "Hello\r\n"; *p; ++p) {
        mx_bus_io_write(bus, 0x378, (uint8_t)*p);
        mx_bus_io_write(bus, 0x37A, 0x0D);                      /* strobe */
        mx_bus_io_write(bus, 0x37A, 0x0C);
        mx_junior_run(j, OSC_US(200));
    }
    CHECK_EQ(host.sent_count[MX_PORT_LPT], 7);
    CHECK(!memcmp(host.sent[MX_PORT_LPT], "Hello\r\n", 7));
    mx_junior_destroy(j);
    mx_ports_card_free(card);
}

int main(void) {
    test_loopback();
    test_printer();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    puts("ports: OK");
    return 0;
}

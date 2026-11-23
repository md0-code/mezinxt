/* MezinXT - the FEPER JUNIOR-XT's main board (junior.h).
 *
 * Written from the Junior's technical manual (chapter 1: clocks, cycle
 * lengths, refresh, DMA, interrupts, NMI; the I/O address table; the 8255
 * bit and DIP switch tables; the DMA channel table) and, where the manual
 * says the board repeats it, the IBM PC/XT Technical Reference's system
 * board description. MezinXT's own code. */
#include "junior.h"

#include "cpu8086.h"
#include "fpu8087.h"
#include "i8237.h"
#include "i8253.h"
#include "i8255.h"
#include "i8259.h"
#include "drivesnd.h"
#include "speaker.h"
#include "xtkbd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_BASE 0xF0000u
#define ROM_SIZE 0x10000u

/* Port B bits (manual: the 8255 bit table). */
enum {
    PB_GATE2 = 0x01, PB_SPEAKER = 0x02, PB_SWITCH_HIGH = 0x08, PB_NO_PARITY = 0x10, PB_NO_IOCHK = 0x20,
    PB_KBD_CLOCK = 0x40, PB_KBD_CLEAR = 0x80
};

/* A board register block in the I/O map: the 8255's front (inputs sampled
 * at the read), the DMA page registers, the NMI mask. */
typedef struct reg_dev {
    mx_dev dev;
    mx_junior *j;
} reg_dev;

struct mx_junior {
    mx_junior_options opt;
    mx_hw_host host;
    mx_cpu_bus cpu_bus;
    mx_cpu8086 cpu;
    mx_sched sched;
    mx_bus bus;
    mx_i8259 pic;
    mx_i8253 pit;
    mx_i8255 ppi;
    mx_i8237 dma;
    mx_xtkbd kbd;
    mx_speaker spk;
    mx_drivesnd drv;            /* the drives' sound, mixed with the speaker's (off unless the board turns it on) */
    reg_dev ppi_front, page_reg, nmi_reg;
    uint8_t *ram;
    uint8_t rom[ROM_SIZE];
    uint8_t switches;
    uint8_t pb;                 /* port B as the 8255 drives it */
    uint8_t nmi_mask;           /* port A0h */
    uint8_t parity, iochk;      /* the RAM parity and I/O channel check latches (port C bits 7, 6) */
    uint8_t fpu_int;            /* the 8087's INT output */
    mx_fpu8087 fpu;             /* fitted when opt.fpu */
    uint8_t nmi;                /* the NMI line */
    uint8_t refresh_req;        /* the refresh request flip-flop (DREQ0) */
    uint8_t out1;               /* OUT1 as last reported */
    uint8_t refresh_cycle;      /* the DMA cycle in progress is channel 0's */
    uint32_t irq_routes[8];     /* card routes asserting each IR line (bit = route index) */
    uint64_t clk_base, osc_base;
    uint64_t due_osc, due_clock;    /* the scheduler's next deadline, and as a CPU clock count */
    mx_junior_display display;
    mx_junior_trace_fn trace;
    void *trace_ctx;
};

/* --- time ----------------------------------------------------------------------------- */

/* The CPU's clock count is read from its struct (cpu8086.h's mx_cpu_clock
 * without the call): the board asks for it on every free bus clock. */
static inline uint64_t board_time(const mx_junior *j) {
    const uint64_t clocks = j->cpu.clock - j->clk_base;
    return j->osc_base + (j->opt.clock_den == 1 ? clocks * j->opt.clock_num : clocks * j->opt.clock_num / j->opt.clock_den);
}

/* The first CPU clock at which board_time reaches OSC time t. */
static uint64_t clock_at(const mx_junior *j, uint64_t t) {
    if (t == MX_NEVER) return UINT64_MAX;
    if (t <= j->osc_base) return j->clk_base;
    const uint64_t d = (t - j->osc_base) * j->opt.clock_den;
    return j->clk_base + (d + j->opt.clock_num - 1u) / j->opt.clock_num;
}

/* Whether a chip deadline has come by the CPU's present clock (the
 * deadline is converted to a clock count once per change). */
static inline int event_due(mx_junior *j) {
    if (j->sched.next != j->due_osc) {
        j->due_osc = j->sched.next;
        j->due_clock = clock_at(j, j->due_osc);
    }
    return j->cpu.clock >= j->due_clock;
}

/* Chip deadlines up to the CPU's present clock: run before a cycle that
 * reaches a chip, so the chip sees every event before it. */
static inline void catch_up(mx_junior *j) {
    const uint64_t t = board_time(j);
    if (mx_sched_due(&j->sched, t)) mx_sched_run(&j->sched, t);
}

/* --- the NMI ------------------------------------------------------------------------------ */

static void update_nmi(mx_junior *j) {
    const uint8_t n = (uint8_t)((j->nmi_mask & 0x80u) && (j->parity || j->iochk || j->fpu_int));
    if (n == j->nmi) return;
    j->nmi = n;
    mx_cpu_set_nmi(&j->cpu, n);
}

/* --- port B ----------------------------------------------------------------------------- */

static void set_speaker_watch(mx_junior *j) {
    /* OUT2 is reported (an event per edge) only while the speaker listens;
     * port C bit 5 samples it at the read. */
    mx_i8253_watch(&j->pit, 3u | (j->pb & PB_SPEAKER ? 4u : 0u));
}

static void port_b(mx_junior *j, int bit, int level) {
    const uint8_t m = (uint8_t)(1u << bit);
    j->pb = (uint8_t)(level ? j->pb | m : j->pb & ~m);
    switch (m) {
    case PB_GATE2: j->pit.dev.ops->input(&j->pit.dev, MX_I8253_GATE2, level); break;
    case PB_SPEAKER:
        set_speaker_watch(j);
        j->spk.dev.ops->input(&j->spk.dev, MX_SPEAKER_DATA, level);
        break;
    case PB_NO_PARITY: if (level) { j->parity = 0; update_nmi(j); } break;
    case PB_NO_IOCHK: if (level) { j->iochk = 0; update_nmi(j); } break;
    case PB_KBD_CLOCK: j->kbd.dev.ops->input(&j->kbd.dev, MX_XTKBD_IN_CLOCK, level); break;
    case PB_KBD_CLEAR: j->kbd.dev.ops->input(&j->kbd.dev, MX_XTKBD_IN_CLEAR, level); break;
    default: break;
    }
}

/* Port C: the switch nibble port B bit 3 selects, the speaker drive, OUT2,
 * the two check latches. */
static uint8_t port_c(mx_junior *j) {
    uint8_t v = (uint8_t)(j->pb & PB_SWITCH_HIGH ? j->switches >> 4 : j->switches & 0x0Fu);
    if (mx_i8253_out(&j->pit, 2)) v |= (uint8_t)(j->pb & PB_SPEAKER ? 0x30u : 0x20u);
    if (j->iochk) v |= 0x40u;
    if (j->parity) v |= 0x80u;
    return v;
}

/* --- the board's register blocks -------------------------------------------------------- */

static uint32_t ppi_read(void *dev, unsigned space, uint32_t offset, unsigned size) {
    mx_junior *j = ((reg_dev *)dev)->j;
    const uint32_t reg = offset & 3u;
    (void)size;
    if (reg == 0) j->ppi.dev.ops->input(&j->ppi.dev, MX_I8255_PORT(MX_I8255_PORT_A), mx_xtkbd_data(&j->kbd));
    else if (reg == 2) j->ppi.dev.ops->input(&j->ppi.dev, MX_I8255_PORT(MX_I8255_PORT_C), port_c(j));
    return j->ppi.dev.ops->read(&j->ppi.dev, space, reg, 1);
}
static void ppi_write(void *dev, unsigned space, uint32_t offset, unsigned size, uint32_t value) {
    mx_junior *j = ((reg_dev *)dev)->j;
    (void)size;
    j->ppi.dev.ops->write(&j->ppi.dev, space, offset & 3u, 1, value);
}

static uint32_t wo_read(void *dev, unsigned space, uint32_t offset, unsigned size) {
    (void)dev; (void)space; (void)offset; (void)size;
    return 0xFFu;                       /* write-only registers: open bus */
}
static void page_write(void *dev, unsigned space, uint32_t offset, unsigned size, uint32_t value) {
    (void)space; (void)size;
    mx_dma_page_write(&((reg_dev *)dev)->j->dma, offset & 3u, (uint8_t)value);
}
static void nmi_write(void *dev, unsigned space, uint32_t offset, unsigned size, uint32_t value) {
    mx_junior *j = ((reg_dev *)dev)->j;
    (void)space; (void)offset; (void)size;
    j->nmi_mask = (uint8_t)value;
    update_nmi(j);
}
static void reg_event(void *dev, uint64_t now) { (void)dev; (void)now; }
static void reg_reset(void *dev) { (void)dev; }

static const mx_dev_ops PPI_FRONT_OPS = {ppi_read, ppi_write, reg_event, reg_reset, NULL, NULL, NULL};
static const mx_dev_ops PAGE_OPS = {wo_read, page_write, reg_event, reg_reset, NULL, NULL, NULL};
static const mx_dev_ops NMI_OPS = {wo_read, nmi_write, reg_event, reg_reset, NULL, NULL, NULL};

/* --- the host the chips see ------------------------------------------------------------- */

static uint64_t h_now(void *ctx) {
    const mx_junior *j = ctx;
    return j->sched.in_event ? j->sched.event_time : board_time(j);
}

static void h_schedule(void *ctx, void *dev, uint64_t when) {
    mx_junior *j = ctx;
    mx_sched_set(&j->sched, dev, when);
}

static void card_line(mx_junior *j, mx_dev *dev, int pin, int state) {
    const mx_bus_route_entry *r = mx_bus_route_of(&j->bus, dev, pin);
    if (!r) return;
    switch (r->kind) {
    case MX_ROUTE_IRQ: {
        const uint32_t bit = 1u << (r - j->bus.route);
        const uint32_t before = j->irq_routes[r->number];
        j->irq_routes[r->number] = state ? before | bit : before & ~bit;
        if (!before != !j->irq_routes[r->number]) j->pic.dev.ops->input(&j->pic.dev, r->number, state != 0);
        break;
    }
    case MX_ROUTE_DRQ:
        mx_i8237_attach(&j->dma, r->number, dev);
        j->dma.dev.ops->input(&j->dma.dev, MX_I8237_DREQ(r->number), state);
        break;
    case MX_ROUTE_IOCHK:
        if (state && !(j->pb & PB_NO_IOCHK)) { j->iochk = 1; update_nmi(j); }
        break;
    default: break;
    }
}

static void h_line(void *ctx, void *device, int pin, int state) {
    mx_junior *j = ctx;
    mx_dev *dev = device;
    if (dev == &j->pit.dev) {
        if (pin == MX_I8253_OUT0) j->pic.dev.ops->input(&j->pic.dev, MX_I8259_IR0, state);
        else if (pin == MX_I8253_OUT1) {
            /* OUT1's rising edge sets the refresh request flip-flop. */
            if (state && !j->out1 && !j->refresh_req) {
                j->refresh_req = 1;
                j->dma.dev.ops->input(&j->dma.dev, MX_I8237_DREQ(0), 1);
            }
            j->out1 = (uint8_t)(state != 0);
        } else if (pin == MX_I8253_OUT2) j->spk.dev.ops->input(&j->spk.dev, MX_SPEAKER_TIMER, state);
    } else if (dev == &j->ppi.dev) {
        if (pin >= MX_I8255_PIN(MX_I8255_PORT_B, 0) && pin <= MX_I8255_PIN(MX_I8255_PORT_B, 7))
            port_b(j, pin - MX_I8255_PIN(MX_I8255_PORT_B, 0), state);
    } else if (dev == &j->pic.dev) {
        mx_cpu_set_intr(&j->cpu, state);
    } else if (dev == &j->kbd.dev) {
        if (pin == MX_XTKBD_OUT_IRQ) j->pic.dev.ops->input(&j->pic.dev, MX_I8259_IR1, state);
    } else if (dev == &j->dma.dev) {
        if (pin == MX_I8237_DACK(0) && state) {
            /* DACK0 clears the refresh request. */
            j->refresh_cycle = 1;
            if (j->refresh_req) {
                j->refresh_req = 0;
                j->dma.dev.ops->input(&j->dma.dev, MX_I8237_DREQ(0), 0);
            }
        }
    } else if (dev != &j->spk.dev) {
        card_line(j, dev, pin, state);
    }
}

/* The 8237 as bus master: byte-wide, through the memory map. */
static int h_dma(void *ctx, uint32_t address, void *data, uint32_t length, int write) {
    mx_junior *j = ctx;
    uint8_t *p = data;
    for (uint32_t i = 0; i < length; ++i) {
        if (write) mx_bus_mem_write(&j->bus, address + i, p[i]);
        else p[i] = mx_bus_mem_read(&j->bus, address + i);
    }
    return 1;
}

/* --- the CPU's bus --------------------------------------------------------------------- */

static uint8_t c_mem_read(void *ctx, uint32_t address) { return mx_bus_mem_read(&((mx_junior *)ctx)->bus, address); }
static void c_mem_write(void *ctx, uint32_t address, uint8_t value) { mx_bus_mem_write(&((mx_junior *)ctx)->bus, address, value); }
static uint16_t c_mem_read16(void *ctx, uint32_t address) { return mx_bus_mem_read16(&((mx_junior *)ctx)->bus, address); }
static void c_mem_write16(void *ctx, uint32_t address, uint16_t value) { mx_bus_mem_write16(&((mx_junior *)ctx)->bus, address, value); }

static uint8_t c_io_read(void *ctx, uint16_t port) {
    mx_junior *j = ctx;
    if (event_due(j)) catch_up(j);
    const uint8_t v = mx_bus_io_read(&j->bus, port);
    if (j->trace) j->trace(j->trace_ctx, MX_JUNIOR_TRACE_IO_READ, port, v);
    return v;
}
static void c_io_write(void *ctx, uint16_t port, uint8_t value) {
    mx_junior *j = ctx;
    if (event_due(j)) catch_up(j);
    if (j->trace) j->trace(j->trace_ctx, MX_JUNIOR_TRACE_IO_WRITE, port, value);
    mx_bus_io_write(&j->bus, port, value);
}
static uint16_t c_io_read16(void *ctx, uint16_t port) {
    mx_junior *j = ctx;
    if (event_due(j)) catch_up(j);
    const uint16_t v = mx_bus_io_read16(&j->bus, port);
    if (j->trace) j->trace(j->trace_ctx, MX_JUNIOR_TRACE_IO_READ, port, v);
    return v;
}
static void c_io_write16(void *ctx, uint16_t port, uint16_t value) {
    mx_junior *j = ctx;
    if (event_due(j)) catch_up(j);
    if (j->trace) j->trace(j->trace_ctx, MX_JUNIOR_TRACE_IO_WRITE, port, value);
    mx_bus_io_write16(&j->bus, port, value);
}

static uint8_t c_inta(void *ctx) {
    mx_junior *j = ctx;
    if (event_due(j)) catch_up(j);
    const uint8_t v = mx_i8259_inta(&j->pic);
    if (j->trace) j->trace(j->trace_ctx, MX_JUNIOR_TRACE_INTERRUPT, v, 0);
    return v;
}

/* I/O CH RDY to the CPU's READY (schematic sheet 2: the wait flip-flop U94,
 * preset while I/O CH RDY is low and cleared by U97 a clock after it was
 * set, so it follows the release at once; sheet 1: its output on the
 * 8284's AEN1 with ASYNC grounded). The 8284 takes a rising READY in its
 * first stage at a rising CLK edge, two thirds into a T-state, and passes
 * it to the CPU at the next falling edge; the CPU samples READY at the
 * rising edge of T3 and of each Tw. So a release that the board can sample
 * `release` OSC periods after the start of T2 ends the wait states with the
 * T-state after the first one whose rising edge comes at or after it: the
 * wait states are the smallest w with (w + 2/3) clocks >= `release`. */
unsigned mx_junior_ready_waits(const mx_junior *j, uint32_t release) {
    const uint64_t num = j->opt.clock_num, need = 3u * (uint64_t)release * j->opt.clock_den, first = 2u * num;
    return need <= first ? 0u : (unsigned)((need - first + 3u * num - 1u) / (3u * num));
}

/* Wait states: one on every I/O cycle (the board's), plus what a chip's
 * window asks for: OSC periods rounded up to CPU clocks, or through I/O CH
 * RDY (overlapping the board's own I/O wait state, which the same
 * flip-flop makes). The chip is asked at T1 about the command that goes
 * active with the next clock. */
static unsigned c_wait(void *ctx, unsigned kind, uint32_t address, int write) {
    mx_junior *j = ctx;
    unsigned space, base;
    switch (kind) {
    case MX_BUS_IOR:
    case MX_BUS_IOW:
        space = MX_SPACE_IO;
        base = 1;
        break;
    case MX_BUS_CODE:
    case MX_BUS_MEMR:
    case MX_BUS_MEMW:
        if (j->bus.page[(address & 0xFFFFFu) >> MX_BUS_PAGE_BITS].data) return 0;
        space = MX_SPACE_MEM;
        base = 0;
        break;
    default:
        return 0;
    }
    const mx_bus_window *w = mx_bus_window_at(&j->bus, space, address);
    if (!w || !w->dev->ops->wait) return base;
    const uint64_t clocks = j->cpu.clock + 1u - j->clk_base;
    const uint64_t t2 = j->osc_base + clocks * j->opt.clock_num / j->opt.clock_den;
    const uint32_t extra = mx_bus_wait(&j->bus, space, address, write, t2);
    if (w->flags & MX_BUS_IOCHRDY) {
        const unsigned n = mx_junior_ready_waits(j, extra);
        return n > base ? n : base;
    }
    return base + (unsigned)(((uint64_t)extra * j->opt.clock_den + j->opt.clock_num - 1u) / j->opt.clock_num);
}

/* Bus hold for DMA: one 8237 cycle per grant, in CPU clocks (the 8237 runs
 * on the CPU clock); a refresh cycle is one clock shorter (manual 1.x: four
 * clocks against five for the other DMA cycles). */
static unsigned c_hold(void *ctx) {
    mx_junior *j = ctx;
    if (event_due(j)) catch_up(j);
    if (!j->dma.hrq) return 0;
    j->refresh_cycle = 0;
    unsigned n = mx_i8237_bus(&j->dma);
    if (n > 1 && j->refresh_cycle) n--;
    return n;
}

/* The 8087 beside the CPU: it decodes the ESC instructions the CPU fetches,
 * takes the operand address from the CPU's read cycle and runs its own
 * memory transfers on the bus; BUSY drives the CPU's TEST pin; INT joins the
 * NMI logic (as on the IBM PC/XT: unlatched, behind the NMI mask only). */
static void c_esc(void *ctx, const mx_cpu_esc *e) {
    mx_junior *j = ctx;
    mx_fpu_esc(&j->fpu, e->opcode, e->modrm, e->address, e->insn_address, e->clock);
}
static int c_test(void *ctx) {
    const mx_junior *j = ctx;
    return mx_fpu_busy(&j->fpu, j->cpu.clock);
}
static uint8_t f_read(void *ctx, uint32_t address) { return mx_bus_mem_read(&((mx_junior *)ctx)->bus, address); }
static void f_write(void *ctx, uint32_t address, uint8_t value) { mx_bus_mem_write(&((mx_junior *)ctx)->bus, address, value); }
static void f_int(void *ctx, int level) {
    mx_junior *j = ctx;
    j->fpu_int = (uint8_t)(level != 0);
    update_nmi(j);
}

static void c_trace(void *ctx, const mx_cpu_cycle *cy) {
    mx_junior *j = ctx;
    if (cy->ale && cy->status == MX_BUS_CODE && j->trace) j->trace(j->trace_ctx, MX_JUNIOR_TRACE_FETCH, cy->address, 0);
}

/* --- life cycle ------------------------------------------------------------------------- */

static uint8_t dip_switches(const mx_junior_options *o) {
    /* SW1 off: normal. SW3-4: the board's banks of 64 KiB, 1 to 4 (ON ON, OFF ON, ON OFF, OFF OFF); the
     * 384 KiB module is not on the switches: a board of 640 KiB says four banks. */
    const unsigned banks = o->ram_kb >= 256 ? 4u : o->ram_kb / 64u;
    uint8_t sw = (uint8_t)(0x01u | ((banks - 1u) & 3u) << 2);
    if (o->fpu) sw |= 0x02u;
    sw |= (uint8_t)((o->display & 3) << 4);
    sw |= (uint8_t)(((o->drives - 1u) & 3u) << 6);
    return sw;
}

void mx_junior_reset(mx_junior *j, int power_on) {
    if (power_on) memset(j->ram, 0, (size_t)j->opt.ram_kb * 1024u);
    j->nmi_mask = 0;
    j->parity = j->iochk = 0;
    if (j->opt.fpu) mx_fpu_reset(&j->fpu);
    update_nmi(j);
    memset(j->irq_routes, 0, sizeof j->irq_routes);
    /* The board's chips and the cards: the 8237, 8259 and 8253 first (in
     * the order they were mapped), then the 8255 (port B's pins fall),
     * the keyboard and the speaker. */
    for (unsigned i = 0; i < j->bus.chips; ++i) j->bus.chip[i]->ops->reset(j->bus.chip[i]);
    j->ppi.dev.ops->reset(&j->ppi.dev);
    j->kbd.dev.ops->reset(&j->kbd.dev);
    j->spk.dev.ops->reset(&j->spk.dev);
    if (j->refresh_req) {
        j->refresh_req = 0;
        j->dma.dev.ops->input(&j->dma.dev, MX_I8237_DREQ(0), 0);
    }
    j->pit.dev.ops->input(&j->pit.dev, MX_I8253_GATE2, j->pb & PB_GATE2);
    set_speaker_watch(j);
    mx_cpu_reset(&j->cpu);
}

mx_junior *mx_junior_create(const mx_junior_options *o, const uint8_t *bios, size_t bios_size, char *error, size_t capacity) {
    if (bios_size != ROM_SIZE) { if (capacity) snprintf(error, capacity, "The BIOS image must be 64 KiB"); return NULL; }
    if (o->ram_kb != 64 && o->ram_kb != 128 && o->ram_kb != 192 && o->ram_kb != 256 && o->ram_kb != 640) {
        if (capacity) snprintf(error, capacity, "The board holds 64, 128, 192 or 256 KiB, or 640 KiB with the 384 KiB module");
        return NULL;
    }
    if (o->drives < 1 || o->drives > 4) { if (capacity) snprintf(error, capacity, "The board drives 1 to 4 floppy drives"); return NULL; }
    if (!o->clock_num || !o->clock_den) { if (capacity) snprintf(error, capacity, "No CPU clock"); return NULL; }
    mx_junior *j = calloc(1, sizeof *j);
    if (j) j->ram = calloc((size_t)o->ram_kb, 1024u);
    if (!j || !j->ram) {
        if (j) free(j);
        if (capacity) snprintf(error, capacity, "Out of memory");
        return NULL;
    }
    j->opt = *o;
    memcpy(j->rom, bios, ROM_SIZE);
    j->switches = dip_switches(o);
    j->host.ctx = j;
    j->host.now = h_now;
    j->host.schedule = h_schedule;
    j->host.line = h_line;
    j->host.dma = h_dma;
    mx_sched_init(&j->sched);

    mx_bus_init(&j->bus);
    mx_bus_map_direct(&j->bus, 0, o->ram_kb * 1024u, j->ram, 1);
    mx_bus_map_direct(&j->bus, ROM_BASE, ROM_SIZE, j->rom, 0);

    mx_i8259_init(&j->pic, &j->host);
    mx_i8253_init(&j->pit, &j->host, MX_I8253_PIT_NUM, MX_I8253_PIT_DEN);
    mx_i8255_init(&j->ppi, &j->host, "i8255");
    mx_i8237_init(&j->dma, &j->host, "i8237");
    j->dma.cycle_clocks = 5;
    mx_xtkbd_init(&j->kbd, &j->host, "keyboard");
    mx_speaker_init(&j->spk, &j->host);
    mx_drivesnd_init(&j->drv);
    reg_dev *regs[3] = {&j->ppi_front, &j->page_reg, &j->nmi_reg};
    const mx_dev_ops *ops[3] = {&PPI_FRONT_OPS, &PAGE_OPS, &NMI_OPS};
    const char *names[3] = {"8255 front", "DMA page registers", "NMI mask"};
    for (int i = 0; i < 3; ++i) {
        regs[i]->dev.ops = ops[i];
        regs[i]->dev.host = &j->host;
        regs[i]->dev.name = names[i];
        regs[i]->j = j;
    }
    /* The on-board decode, 32 ports a block (manual: I/O address table). */
    mx_bus_map_io(&j->bus, &j->dma.dev, 0x00, 0x1F, 0);
    mx_bus_map_io(&j->bus, &j->pic.dev, 0x20, 0x3F, 0);
    mx_bus_map_io(&j->bus, &j->pit.dev, 0x40, 0x5F, 0);
    mx_bus_map_io(&j->bus, &j->ppi_front.dev, 0x60, 0x7F, 0);
    mx_bus_map_io(&j->bus, &j->page_reg.dev, 0x80, 0x9F, 0);
    mx_bus_map_io(&j->bus, &j->nmi_reg.dev, 0xA0, 0xBF, 0);

    mx_cpu_bus *b = &j->cpu_bus;
    b->ctx = j;
    b->mem_read = c_mem_read;
    b->mem_write = c_mem_write;
    b->io_read = c_io_read;
    b->io_write = c_io_write;
    if (o->cpu == MX_JUNIOR_8086) {
        b->mem_read16 = c_mem_read16;
        b->mem_write16 = c_mem_write16;
        b->io_read16 = c_io_read16;
        b->io_write16 = c_io_write16;
    }
    b->inta = c_inta;
    b->wait = c_wait;
    b->hold = c_hold;
    if (o->fpu) {
        const mx_fpu_host fh = {j, f_read, f_write, f_int};
        mx_fpu_init(&j->fpu, &fh);
        b->esc = c_esc;
        b->test = c_test;
    }
    mx_cpu_init(&j->cpu, o->cpu == MX_JUNIOR_8086 ? MX_CPU_8086 : MX_CPU_8088, b);
    j->clk_base = mx_cpu_clock(&j->cpu);
    j->osc_base = 0;
    mx_junior_reset(j, 1);
    return j;
}

void mx_junior_destroy(mx_junior *j) {
    if (!j) return;
    free(j->ram);
    free(j);
}

/* --- running ------------------------------------------------------------------------------ */

int mx_junior_run(mx_junior *j, uint64_t ticks) {
    const uint64_t target = board_time(j) + ticks;
    const uint64_t end = clock_at(j, target);
    for (;;) {
        if (event_due(j)) catch_up(j);
        if (j->cpu.clock >= end) break;
        mx_cpu_step(&j->cpu);
    }
    return mx_cpu_halted(&j->cpu) && !(mx_cpu_get(&j->cpu, MX_FLAGS) & MX_IF);
}

uint64_t mx_junior_time(const mx_junior *j) { return board_time(j); }

/* --- expansion, input, sound, checks ----------------------------------------------------- */

mx_bus *mx_junior_bus(mx_junior *j) { return &j->bus; }
const mx_hw_host *mx_junior_host(mx_junior *j) { return &j->host; }

void mx_junior_set_display(mx_junior *j, const mx_junior_display *display) {
    if (display) j->display = *display;
    else memset(&j->display, 0, sizeof j->display);
}

int mx_junior_frame(mx_junior *j, struct mx_frame *frame) {
    return j->display.frame ? j->display.frame(j->display.ctx, frame) : 0;
}

int mx_junior_key(mx_junior *j, uint8_t make_code, int down) { return mx_xtkbd_key(&j->kbd, make_code, down); }

size_t mx_junior_audio(mx_junior *j, int16_t *stereo, size_t frames) {
    const size_t n = mx_speaker_render(&j->spk, board_time(j), stereo, frames);
    /* The drives' sound goes in the frames the speaker just made, by their place on the speaker's frame clock. */
    if (j->drv.enabled) mx_drivesnd_mix(&j->drv, mx_speaker_position(&j->spk) - n, stereo, n);
    return n;
}

size_t mx_junior_audio_free(mx_junior *j, int16_t *stereo, size_t frames) {
    const uint64_t now = board_time(j);
    int16_t spare[512 * 2];
    while (mx_speaker_render(&j->spk, now, spare, 512) == 512) {}
    return mx_drivesnd_free(&j->drv, now, stereo, frames);
}

mx_drivesnd *mx_junior_drivesnd(mx_junior *j) { return &j->drv; }

void mx_junior_set_drive_sound(mx_junior *j, int enabled, unsigned volume) { mx_drivesnd_set(&j->drv, enabled, volume); }

void mx_junior_set_trace(mx_junior *j, mx_junior_trace_fn fn, void *ctx) {
    j->trace = fn;
    j->trace_ctx = ctx;
    j->cpu_bus.trace = fn ? c_trace : NULL;
}

uint8_t mx_junior_peek(mx_junior *j, uint32_t address) {
    const mx_bus_page *p = &j->bus.page[(address & 0xFFFFFu) >> MX_BUS_PAGE_BITS];
    if (p->data) return p->data[address & (MX_BUS_PAGE_SIZE - 1u)];
    return p->window ? mx_bus_mem_read_slow(&j->bus, address) : 0xFF;
}

uint8_t mx_junior_switches(const mx_junior *j) { return j->switches; }

void mx_junior_parity_error(mx_junior *j) {
    if (j->pb & PB_NO_PARITY) return;
    j->parity = 1;
    update_nmi(j);
}

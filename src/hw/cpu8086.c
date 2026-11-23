/* MezinXT - the Intel 8086 and 8088 (cpu8086.h).
 *
 * Written from Intel's 8086 Family User's Manual (instruction set,
 * instruction timings, bus cycles, the queue status and segment status
 * pins, interrupts) and the iAPX 86/88 User's Manual; the behaviour of
 * undocumented opcodes, of flags Intel leaves undefined and the clock-level
 * placement of bus cycles follow SingleStepTests' hardware captures of an
 * 8088 and an 8086 (tests/cpu_suite.py). MezinXT's own code.
 *
 * Structure: the execution unit (EU) runs an instruction as a sequence of
 * clocks; every clock ends with tick(), which advances the bus interface
 * unit (BIU) one T-state. The EU reads instruction bytes from the queue
 * (stalling while it is empty), and asks the BIU for memory and I/O cycles
 * (stalling until a read's data arrives, or a write's last cycle starts).
 *
 * BIU rules (as the captures show them):
 *  - A bus cycle is T1 T2 T3 [Tw...] T4. Data moves on T3 (or the last Tw);
 *    a fetched byte enters the queue at the end of T4.
 *  - On the transfer clock the next cycle is chosen: a pending EU request,
 *    else a code fetch when the queue will have room (one byte on the 8088,
 *    two on the 8086, counting the bytes in flight), else none.
 *  - A cycle that was not chosen that way starts three clocks after it was
 *    wanted (two Ti in between, the address cycles): an EU request made
 *    after the decision, a fetch after a queue read freed room or after a
 *    flush. A fetch whose room appeared only on T3/T4 of a code fetch is
 *    wanted on T4 when the queue then has room for two fetches, else a
 *    clock later. An EU request arriving while a fetch waits to start
 *    preempts it: on the clock the fetch was wanted (or on the T4 before a
 *    chosen fetch) it starts its own three-clock count; during the fetch's
 *    first address clock it waits for the second, and starts its count
 *    there.
 *  - The microcode suspends prefetching before control transfers (SUSP)
 *    and flushes the queue at their end, which restarts fetching at the new
 *    IP. */
#include "cpu8086.h"

#include <string.h>

typedef mx_cpu8086 cpu;

enum { SEG_ES = 0, SEG_CS = 1, SEG_SS = 2, SEG_DS = 3 };
enum { NEXT_NONE, NEXT_FETCH, NEXT_EU };
#define OP_NONE 0xFFu

/* The segment status S4-S3 for a segment register. */
static const uint8_t SEG_STATUS[4] = {MX_SEG_ES, MX_SEG_CS, MX_SEG_SS, MX_SEG_DS};

static uint32_t lin(uint16_t seg, uint16_t off) { return (((uint32_t)seg << 4) + off) & 0xFFFFFu; }

/* --- the bus interface unit ----------------------------------------------------- */

static int can_fetch(const cpu *c, unsigned inflight) {
    if (c->bsusp) return 0;
    return c->qlen + c->q_taken + inflight + (c->model == MX_CPU_8086 ? 2u : 1u) <= c->qsize;
}

static int rq_pending(const cpu *c) { return c->rq_active && c->rq_phase < c->rq_phases; }

static uint8_t mem_rd(cpu *c, uint32_t a) { return c->bus->mem_read(c->bus->ctx, a & 0xFFFFFu); }

/* Starts T1 of a code fetch or of the EU's next cycle. */
static void biu_begin(cpu *c, int what) {
    if (what == NEXT_FETCH) {
        c->bop = MX_BUS_CODE;
        c->bseg = MX_SEG_CS;
        c->baddr = lin(c->s[SEG_CS], c->pc);
        c->bwidth = (c->model == MX_CPU_8086 && !(c->baddr & 1u)) ? 2 : 1;
        c->bhe = c->model == MX_CPU_8086 ? (uint8_t)(c->bwidth == 2 || (c->baddr & 1u) ? 0 : 1) : 1;
        c->pc = (uint16_t)(c->pc + c->bwidth);
        c->bdiscard = 0;
        c->blast = 0;
    } else {
        unsigned phase = c->rq_phase++;
        c->bop = c->rq_op;
        if (c->rq_op == MX_BUS_IOR || c->rq_op == MX_BUS_IOW) {
            c->baddr = (uint16_t)(c->rq_off + phase);
            c->bseg = MX_SEG_CS;
        } else if (c->rq_op == MX_BUS_INTA || c->rq_op == MX_BUS_HALT) {
            c->baddr = 0;
            c->bseg = MX_SEG_CS;
        } else {
            c->baddr = lin(c->rq_base, (uint16_t)(c->rq_off + phase));
            c->bseg = c->rq_seg;
        }
        c->bwidth = (c->rq_word && c->rq_phases == 1 && c->model == MX_CPU_8086) ? 2 : 1;
        if (c->model == MX_CPU_8086)
            c->bhe = (uint8_t)((c->bwidth == 2 || (c->baddr & 1u)) ? 0 : 1);
        else
            c->bhe = 1;
        c->blast = c->rq_phase == c->rq_phases;
        if (c->rq_op == MX_BUS_MEMW || c->rq_op == MX_BUS_IOW)
            c->bdata = c->bwidth == 2 ? c->rq_data : (uint16_t)(phase ? c->rq_data >> 8 : c->rq_data & 0xFFu);
    }
    c->bwait = c->bus->wait ? c->bus->wait(c->bus->ctx, c->bop, c->baddr,
                                           c->bop == MX_BUS_MEMW || c->bop == MX_BUS_IOW) : 0;
    c->bt = MX_T_1;
}

/* The data transfer of the current cycle (T3 or the last Tw). */
static void biu_transfer(cpu *c) {
    const mx_cpu_bus *b = c->bus;
    uint32_t a = c->baddr;
    switch (c->bop) {
    case MX_BUS_CODE:
        if (c->bwidth == 2) c->bdata = (uint16_t)(mem_rd(c, a) | mem_rd(c, a + 1) << 8);
        else c->bdata = mem_rd(c, a);
        break;
    case MX_BUS_MEMR:
        if (c->bwidth == 2) c->bdata = b->mem_read16 ? b->mem_read16(b->ctx, a) : (uint16_t)(mem_rd(c, a) | mem_rd(c, a + 1) << 8);
        else c->bdata = mem_rd(c, a);
        break;
    case MX_BUS_MEMW:
        if (c->bwidth == 2) {
            if (b->mem_write16) b->mem_write16(b->ctx, a, c->bdata);
            else { b->mem_write(b->ctx, a, (uint8_t)c->bdata); b->mem_write(b->ctx, (a + 1) & 0xFFFFFu, (uint8_t)(c->bdata >> 8)); }
        } else b->mem_write(b->ctx, a, (uint8_t)c->bdata);
        break;
    case MX_BUS_IOR:
        if (c->bwidth == 2) c->bdata = b->io_read16 ? b->io_read16(b->ctx, (uint16_t)a)
                                                    : (uint16_t)(b->io_read(b->ctx, (uint16_t)a) | b->io_read(b->ctx, (uint16_t)(a + 1)) << 8);
        else c->bdata = b->io_read(b->ctx, (uint16_t)a);
        break;
    case MX_BUS_IOW:
        if (c->bwidth == 2) {
            if (b->io_write16) b->io_write16(b->ctx, (uint16_t)a, c->bdata);
            else { b->io_write(b->ctx, (uint16_t)a, (uint8_t)c->bdata); b->io_write(b->ctx, (uint16_t)(a + 1), (uint8_t)(c->bdata >> 8)); }
        } else b->io_write(b->ctx, (uint16_t)a, (uint8_t)c->bdata);
        break;
    case MX_BUS_INTA:
        c->bdata = c->rq_word ? (b->inta ? b->inta(b->ctx) : 0xFFu) : 0xFFu;
        break;
    default:
        break;
    }
    if (c->bop != MX_BUS_CODE && c->bop != MX_BUS_MEMW && c->bop != MX_BUS_IOW && c->bop != MX_BUS_HALT) {
        /* A read: the byte or word goes to the EU's request. */
        if (c->bwidth == 2) c->rq_data = c->bdata;
        else if (c->rq_phases == 2 && c->rq_phase == 2) c->rq_data = (uint16_t)((c->rq_data & 0xFFu) | (c->bdata & 0xFFu) << 8);
        else c->rq_data = (uint16_t)(c->bdata & 0xFFu);
        if (c->blast) c->rq_done = 1;
    }
    /* Choose the next cycle: an EU request made before this clock, a
     * fetch, or nothing. */
    c->bfull = 0;
    if (rq_pending(c) && c->rq_clock < c->clock) c->bnext = NEXT_EU;
    else if (can_fetch(c, c->bop == MX_BUS_CODE && !c->bdiscard ? c->bwidth : 0)) c->bnext = NEXT_FETCH;
    else {
        c->bnext = NEXT_NONE;
        /* Stopped for want of room: only a queue read while the bus is
         * idle (or the next bus cycle) starts prefetching again. */
        if (!c->bsusp) c->bfull = 1;
    }
}

/* The board's bus hold (DMA), asked whenever the bus is free between
 * cycles (never while LOCK is asserted): the answer is the clocks the bus
 * stays held, from this clock on (`on_t4` 0) or from the next one (asked on
 * a T4, which is still the CPU's). Each answer is one DMA cycle charged
 * whole; the hold is asked again when it ends. */
static int ask_hold(cpu *c, int on_t4) {
    if (!c->bus->hold || c->lock) return 0;
    const unsigned n = c->bus->hold(c->bus->ctx);
    if (!n) return 0;
    c->bhold = on_t4 ? n : n - 1u;
    return 1;
}

/* With the bus free, start the wanted cycle after two Ti (and any hold). */
static void biu_idle(cpu *c, int on_t4) {
    if (c->bhold) { c->bhold--; return; }
    if (c->bdelay) {
        if (c->bpending == NEXT_FETCH && rq_pending(c)) {
            c->bpending = NEXT_EU;
            c->bdelay = c->bdelay == 2 ? 3 : 2;
            return;
        }
        /* A fetch is committed on its last clock before T1: until then a
         * SUSP (or a full queue) cancels it. */
        if (c->bpending == NEXT_FETCH && (c->qlen + (c->model == MX_CPU_8086 ? 2u : 1u) > c->qsize || (c->bsusp && c->bdelay >= 2))) {
            c->bdelay = 0;
            c->bpending = NEXT_NONE;
            return;
        }
        if (--c->bdelay == 0) {
            if (ask_hold(c, 0)) {
                c->bdelay = 1;
                return;
            }
            biu_begin(c, c->bpending);
            c->bpending = NEXT_NONE;
        }
        return;
    }
    if (rq_pending(c)) { c->bpending = NEXT_EU; c->bdelay = 2; }
    else if (!c->bfull && can_fetch(c, 0)) { c->bpending = NEXT_FETCH; c->bdelay = 2; }
    else ask_hold(c, on_t4);   /* an idle bus (a full queue, HLT) is free for DMA */
}

static void q_push(cpu *c, uint8_t b) {
    c->q[(c->qhead + c->qlen) & 7u] = b;
    c->qlen++;
}

/* One clock: the pins of this clock, then the BIU's next T-state. */
static void tick(cpu *c) {
    const mx_cpu_bus *b = c->bus;
    uint8_t t = c->bt;
    int transfer = (t == MX_T_3 && c->bwait == 0) || (t == MX_T_W && c->bwait == 1);
    if (transfer) biu_transfer(c);
    c->q_taken = 0;   /* reads of this clock count from here on */
    mx_cpu_cycle cy;
    if (b->trace) {
        memset(&cy, 0, sizeof cy);
        cy.tstate = t;
        cy.address = c->baddr;
        cy.status = MX_BUS_PASV;
        cy.segment = MX_SEG_NONE;
        cy.bhe = c->bhe;
        cy.lock = c->lock;
        if (t == MX_T_1) { cy.ale = 1; cy.status = c->bop; }
        else if (t == MX_T_2) cy.status = c->bop;
        if (t != MX_T_I && t != MX_T_1) cy.segment = c->bseg;
        if (c->bop == MX_BUS_INTA || c->bop == MX_BUS_HALT) cy.segment = (t == MX_T_I || t == MX_T_1) ? MX_SEG_NONE : MX_SEG_CS;
        if (t == MX_T_2 || t == MX_T_3 || t == MX_T_W) {
            switch (c->bop) {
            case MX_BUS_CODE: case MX_BUS_MEMR: cy.lines = MX_LINE_MRDC; break;
            case MX_BUS_MEMW: cy.lines = (uint8_t)(t == MX_T_2 ? MX_LINE_AMWC : MX_LINE_AMWC | MX_LINE_MWTC); break;
            case MX_BUS_IOR: cy.lines = MX_LINE_IORC; break;
            case MX_BUS_IOW: cy.lines = (uint8_t)(t == MX_T_2 ? MX_LINE_AIOWC : MX_LINE_AIOWC | MX_LINE_IOWC); break;
            default: break;
            }
        }
        if (transfer) cy.data = (c->bwidth == 1 && (c->baddr & 1u) && c->model == MX_CPU_8086) ? (uint16_t)(c->bdata << 8) : c->bdata;
        cy.qop = c->qs_op;
        cy.qbyte = c->qs_byte;
    }
    c->clock++;
    c->qs_op = c->qs_now;
    c->qs_byte = c->qs_now_byte;
    c->qs_now = MX_Q_NONE;
    c->qs_now_byte = 0;
    c->q_taken = 0;
    c->lock = c->lock_next;
    switch (t) {
    case MX_T_1:
        c->bt = MX_T_2;
        if ((c->bop == MX_BUS_MEMW || c->bop == MX_BUS_IOW) && c->blast) c->rq_done = 1;
        break;
    case MX_T_2:
        c->bt = MX_T_3;
        break;
    case MX_T_3:
    case MX_T_W:
        if (transfer) c->bt = MX_T_4;
        else { c->bt = MX_T_W; if (t == MX_T_W) c->bwait--; }
        break;
    case MX_T_4:
        c->t4_end = c->clock;
        if (c->bop == MX_BUS_CODE && !c->bdiscard) {
            q_push(c, (uint8_t)c->bdata);
            if (c->bwidth == 2) q_push(c, (uint8_t)(c->bdata >> 8));
        }
        c->bop = c->bop == MX_BUS_CODE ? MX_BUS_CODE : c->bop;
        if (c->bnext != NEXT_NONE && ask_hold(c, 0)) {
            /* DMA takes the bus between two back-to-back cycles: the chosen
             * cycle waits out the hold (the clock after it asks again). */
            c->bpending = (c->bnext != NEXT_EU && rq_pending(c)) ? NEXT_EU : c->bnext;
            c->bt = MX_T_I;
            c->bdelay = 1;
        } else if (c->bnext != NEXT_EU && rq_pending(c)) {
            /* A request made after the decision: two Ti, then its T1. */
            c->bt = MX_T_I;
            c->bpending = NEXT_EU;
            c->bdelay = 2;
        } else if (c->bnext == NEXT_FETCH) {
            biu_begin(c, NEXT_FETCH);
        } else if (c->bnext == NEXT_EU) {
            biu_begin(c, NEXT_EU);
        } else {
            c->bt = MX_T_I;
            c->bdelay = 0;
            c->bpending = NEXT_NONE;
            if (c->bread3 && !c->bsusp && !rq_pending(c) && can_fetch(c, 0)) {
                /* A queue read on the deciding clock made room after all. */
                c->bfull = 0;
                c->bpending = NEXT_FETCH;
                c->bdelay = (c->qsize - c->qlen >= 2u * (c->model == MX_CPU_8086 ? 2u : 1u)) ? 2 : 3;
            } else biu_idle(c, 1);
        }
        c->bread3 = 0;
        c->bnext = NEXT_NONE;
        break;
    default:
        biu_idle(c, 0);
        break;
    }
    if (b->trace) {
        cy.qlen = (uint8_t)c->qlen;   /* at the end of the clock */
        b->trace(b->ctx, &cy);
    }
}

static void clk(cpu *c, unsigned n) {
    while (n--) tick(c);
}

/* --- the execution unit's side of the bus ---------------------------------------- */

static uint8_t q_read(cpu *c, int first) {
    while (c->qlen == 0) {
        if (c->bsusp && c->bt == MX_T_I) c->bsusp = 0;   /* never wait on a suspended queue */
        tick(c);
    }
    uint8_t v = c->q[c->qhead];
    if (c->bt == MX_T_I) c->bfull = 0;
    if (c->bt == MX_T_3 || c->bt == MX_T_4) c->bread3 = 1;
    c->qhead = (c->qhead + 1) & 7u;
    c->qlen--;
    c->ip++;
    c->qs_now = first ? MX_Q_FIRST : MX_Q_SUBSEQUENT;
    c->qs_now_byte = v;
    c->q_last = v;
    c->q_taken = 1;
    tick(c);
    return v;
}

/* A queue read whose micro-instruction also suspends prefetching. */
static uint8_t q_read_susp(cpu *c) {
    while (c->qlen == 0) tick(c);
    c->bsusp = 1;
    return q_read(c, 0);
}

static uint16_t q_read16(cpu *c) {
    uint16_t lo = q_read(c, 0);
    return (uint16_t)(lo | q_read(c, 0) << 8);
}

static void request(cpu *c, uint8_t op, int seg, uint16_t off, int word, uint16_t data) {
    c->rq_active = 1;
    c->rq_done = 0;
    c->rq_op = op;
    c->rq_seg = seg >= 0 ? SEG_STATUS[seg] : MX_SEG_CS;
    c->rq_base = seg >= 0 ? c->s[seg] : 0;
    c->rq_off = off;
    c->rq_word = (uint8_t)word;
    c->rq_data = data;
    c->rq_phase = 0;
    c->rq_clock = c->clock;
    c->rq_phases = (uint8_t)(!word ? 1 : (c->model == MX_CPU_8086 && !(off & 1u)) ? 1 : 2);
}

static uint16_t finish(cpu *c) {
    do tick(c); while (!c->rq_done);
    c->rq_active = 0;
    return c->rq_data;
}

static uint16_t rd(cpu *c, int seg, uint16_t off, int w) { request(c, MX_BUS_MEMR, seg, off, w, 0); return finish(c); }
static void wr(cpu *c, int seg, uint16_t off, int w, uint16_t v) { request(c, MX_BUS_MEMW, seg, off, w, v); finish(c); }
static uint16_t in(cpu *c, uint16_t port, int w) { request(c, MX_BUS_IOR, -1, port, w, 0); return finish(c); }
static void out(cpu *c, uint16_t port, int w, uint16_t v) { request(c, MX_BUS_IOW, -1, port, w, v); finish(c); }

/* SUSP: no more prefetching until the flush. */
static void susp(cpu *c) { c->bsusp = 1; }

/* CORR: the microcode takes IP back from the BIU (the fetch pointer less
 * the queue); it waits for a code fetch in progress to reach T4. */
static void corr(cpu *c) {
    while ((c->bt == MX_T_I && c->bdelay && c->bpending == NEXT_FETCH) || (c->bt == MX_T_4 && c->bnext == NEXT_FETCH) || c->bt == MX_T_1 || c->bt == MX_T_2 || c->bt == MX_T_3 || c->bt == MX_T_W) tick(c);
    tick(c);
}

/* FLUSH: empty the queue, fetch from the new IP. */
static void flush(cpu *c) {
    c->qlen = 0;
    c->qhead = 0;
    c->pc = c->ip;
    c->bsusp = 0;
    c->bfull = 0;
    if (c->bt != MX_T_I && c->bop == MX_BUS_CODE) c->bdiscard = 1;
    if (c->bt == MX_T_I) { c->bdelay = 0; c->bpending = NEXT_NONE; }
    c->qs_now = MX_Q_EMPTY;
    c->qs_now_byte = c->q_last;
}

/* --- registers and flags ---------------------------------------------------------- */

static uint8_t get8(const cpu *c, unsigned r) { return (uint8_t)(r < 4 ? c->r[r] : c->r[r - 4] >> 8); }
static void set8(cpu *c, unsigned r, uint8_t v) {
    if (r < 4) c->r[r] = (uint16_t)((c->r[r] & 0xFF00u) | v);
    else c->r[r - 4] = (uint16_t)((c->r[r - 4] & 0x00FFu) | (uint16_t)v << 8);
}
static uint16_t getr(const cpu *c, unsigned r, int w) { return w ? c->r[r] : get8(c, r); }
static void setr(cpu *c, unsigned r, int w, uint16_t v) { if (w) c->r[r] = v; else set8(c, r, (uint8_t)v); }

static int parity(uint8_t v) { v ^= (uint8_t)(v >> 4); v ^= (uint8_t)(v >> 2); v ^= (uint8_t)(v >> 1); return !(v & 1u); }
static void setf(cpu *c, uint16_t f, int on) { c->flags = (uint16_t)(on ? c->flags | f : c->flags & ~f); }
static void szp(cpu *c, uint16_t r, int w) {
    uint16_t v = w ? r : (uint16_t)(r & 0xFFu);
    setf(c, MX_SF, (v & (w ? 0x8000u : 0x80u)) != 0);
    setf(c, MX_ZF, v == 0);
    setf(c, MX_PF, parity((uint8_t)v));
}
static int cond(const cpu *c, unsigned n) {
    uint16_t f = c->flags;
    int r;
    switch (n >> 1) {
    case 0: r = (f & MX_OF) != 0; break;
    case 1: r = (f & MX_CF) != 0; break;
    case 2: r = (f & MX_ZF) != 0; break;
    case 3: r = (f & (MX_CF | MX_ZF)) != 0; break;
    case 4: r = (f & MX_SF) != 0; break;
    case 5: r = (f & MX_PF) != 0; break;
    case 6: r = !(f & MX_SF) != !(f & MX_OF); break;
    default: r = (f & MX_ZF) || (!(f & MX_SF) != !(f & MX_OF)); break;
    }
    return (n & 1u) ? !r : r;
}

/* The eight ALU operations of opcodes 00h-3Fh and 80h-83h. */
static uint16_t alu(cpu *c, unsigned op, uint16_t a, uint16_t b, int w) {
    uint32_t mask = w ? 0xFFFFu : 0xFFu, sign = w ? 0x8000u : 0x80u, r, cin = c->flags & MX_CF;
    a &= (uint16_t)mask;
    b &= (uint16_t)mask;
    switch (op & 7u) {
    case 0: case 2:
        r = (uint32_t)a + b + (op == 2 ? cin : 0);
        setf(c, MX_CF, r > mask);
        setf(c, MX_OF, ((a ^ r) & (b ^ r) & sign) != 0);
        setf(c, MX_AF, ((a ^ b ^ r) & 0x10u) != 0);
        break;
    case 3: case 5: case 7:
        r = (uint32_t)a - b - (op == 3 ? cin : 0);
        setf(c, MX_CF, (r & ~mask) != 0);
        setf(c, MX_OF, ((a ^ b) & (a ^ r) & sign) != 0);
        setf(c, MX_AF, ((a ^ b ^ r) & 0x10u) != 0);
        break;
    default:
        r = (op == 1) ? (uint32_t)(a | b) : (op == 4) ? (uint32_t)(a & b) : (uint32_t)(a ^ b);
        c->flags &= (uint16_t)~(MX_CF | MX_OF | MX_AF);
        break;
    }
    szp(c, (uint16_t)r, w);
    return (uint16_t)(r & mask);
}

static uint16_t incdec(cpu *c, uint16_t a, int dec, int w) {
    uint16_t cf = c->flags & MX_CF;
    uint16_t r = alu(c, dec ? 5u : 0u, a, 1, w);
    c->flags = (uint16_t)((c->flags & ~MX_CF) | cf);
    return r;
}

/* --- effective addresses ---------------------------------------------------------- */

/* Reads the modrm byte; for a memory operand computes the address with the
 * microcode's clocks, returning on the clock an operand read would be
 * requested. */
static void modrm(cpu *c) {
    static const uint8_t EA_CLOCKS[8] = {7, 8, 8, 7, 5, 5, 5, 5};
    uint8_t m = q_read(c, 0);
    c->modrm = m;
    c->mod = m >> 6;
    c->reg = (m >> 3) & 7u;
    c->rm = m & 7u;
    c->ea_mem = c->mod != 3;
    if (!c->ea_mem) return;
    uint16_t off;
    int seg = SEG_DS;
    if (c->mod == 0 && c->rm == 6) {
        clk(c, 1);
        off = q_read16(c);
        clk(c, 1);
    } else {
        switch (c->rm) {
        case 0: off = (uint16_t)(c->r[MX_BX] + c->r[MX_SI]); break;
        case 1: off = (uint16_t)(c->r[MX_BX] + c->r[MX_DI]); break;
        case 2: off = (uint16_t)(c->r[MX_BP] + c->r[MX_SI]); seg = SEG_SS; break;
        case 3: off = (uint16_t)(c->r[MX_BP] + c->r[MX_DI]); seg = SEG_SS; break;
        case 4: off = c->r[MX_SI]; break;
        case 5: off = c->r[MX_DI]; break;
        case 6: off = c->r[MX_BP]; seg = SEG_SS; break;
        default: off = c->r[MX_BX]; break;
        }
        unsigned n = EA_CLOCKS[c->rm];
        if (c->mod == 0) clk(c, n - 2);
        else if (c->mod == 1) { clk(c, n - 2); off = (uint16_t)(off + (int8_t)q_read(c, 0)); clk(c, 3); }
        else { clk(c, n - 2); off = (uint16_t)(off + q_read16(c)); clk(c, 2); }
    }
    c->ea_seg = c->seg_override >= 0 ? c->seg_override : seg;
    c->ea_off = off;
    c->last_ea = off;
}

static uint16_t rm_read(cpu *c, int w) { return c->ea_mem ? rd(c, c->ea_seg, c->ea_off, w) : getr(c, c->rm, w); }
static void rm_write(cpu *c, int w, uint16_t v) { if (c->ea_mem) wr(c, c->ea_seg, c->ea_off, w, v); else setr(c, c->rm, w, v); }
static int dseg(const cpu *c) { return c->seg_override >= 0 ? c->seg_override : SEG_DS; }

/* --- stack, jumps, interrupts ----------------------------------------------------- */

static void push(cpu *c, uint16_t v) {
    c->r[MX_SP] = (uint16_t)(c->r[MX_SP] - 2);
    wr(c, SEG_SS, c->r[MX_SP], 1, v);
}
static uint16_t pop(cpu *c) {
    uint16_t v = rd(c, SEG_SS, c->r[MX_SP], 1);
    c->r[MX_SP] = (uint16_t)(c->r[MX_SP] + 2);
    return v;
}

/* The interrupt sequence from the vector read on (INT n, exceptions,
 * NMI, INTR, single step). `ret` is the IP pushed. */
static void interrupt(cpu *c, uint8_t vector, uint16_t ret) {
    if (c->model == MX_CPU_8086) {
        /* The 8086's interrupt microcode (unlike the 8088's) suspends
         * prefetching before the vector read: a bus cycle in progress
         * runs to its end, and the read is requested two micro-
         * instructions later (three clocks after that T4), or a clock
         * after the entry on an idle bus. */
        susp(c);
        clk(c, 1);
        while (c->bt != MX_T_I || c->clock < c->t4_end + 2u) tick(c);
    }
    uint16_t nip = rd(c, -1, (uint16_t)(vector * 4u), 1);
    susp(c);
    clk(c, 1);
    uint16_t ncs = rd(c, -1, (uint16_t)(vector * 4u + 2), 1);
    clk(c, 2);
    c->r[MX_SP] = (uint16_t)(c->r[MX_SP] - 2);
    wr(c, SEG_SS, c->r[MX_SP], 1, c->flags);
    c->flags &= (uint16_t)~(MX_IF | MX_TF);
    clk(c, 6);
    c->r[MX_SP] = (uint16_t)(c->r[MX_SP] - 2);
    wr(c, SEG_SS, c->r[MX_SP], 1, c->s[SEG_CS]);
    clk(c, 5);
    c->s[SEG_CS] = ncs;
    c->ip = nip;
    flush(c);
    clk(c, 3);
    c->r[MX_SP] = (uint16_t)(c->r[MX_SP] - 2);
    wr(c, SEG_SS, c->r[MX_SP], 1, ret);
    clk(c, 1);
}

static void jump(cpu *c, uint16_t ip) { c->ip = ip; flush(c); }

/* --- multiply and divide ---------------------------------------------------------- */

static void mul(cpu *c, uint16_t src, int w, int sgn) {
    if (!w) {
        int32_t p = sgn ? (int32_t)(int8_t)get8(c, 0) * (int8_t)src : (int32_t)get8(c, 0) * (uint8_t)src;
        if (sgn && c->rep) p = -p;
        c->r[MX_AX] = (uint16_t)p;
        int ext = sgn ? ((int16_t)(uint16_t)p != (int8_t)(uint8_t)p) : (c->r[MX_AX] >> 8) != 0;
        setf(c, MX_CF | MX_OF, ext);
        szp(c, (uint16_t)(sgn ? c->r[MX_AX] >> 8 : c->r[MX_AX] >> 8), 0);
    } else {
        int64_t p = sgn ? (int64_t)(int16_t)c->r[MX_AX] * (int16_t)src : (int64_t)c->r[MX_AX] * src;
        if (sgn && c->rep) p = -p;
        c->r[MX_AX] = (uint16_t)p;
        c->r[MX_DX] = (uint16_t)((uint64_t)p >> 16);
        int ext = sgn ? ((int32_t)(uint32_t)p != (int16_t)(uint16_t)p) : c->r[MX_DX] != 0;
        setf(c, MX_CF | MX_OF, ext);
        szp(c, c->r[MX_DX], 1);
    }
}

/* Clocks of the multiply and divide microcode (reg forms, from the first
 * byte to the next instruction's), as the captures show them: the multiply
 * loop costs a clock per set bit of the (absolute) accumulator and one when
 * the product fits the low half; the divide loop a clock per subtracting
 * step that shifted out no carry, two more when the last step subtracted;
 * signs cost a fixed amount. */
#define MULDIV_MEM 1u
static unsigned popcount(uint32_t v) { unsigned n = 0; while (v) { n += v & 1u; v >>= 1; } return n; }

static unsigned mul_clocks(const cpu *c, uint16_t src, uint16_t acc, int w, int sgn) {
    uint16_t mask = w ? 0xFFFFu : 0xFFu, sign = w ? 0x8000u : 0x80u;
    uint16_t a = acc & mask;
    unsigned fits = !(c->flags & MX_CF);   /* the flags are the product's */
    src &= mask;
    if (!sgn) return (w ? 117u : 69u) + popcount(a) + fits;
    int an = (a & sign) != 0, sn = (src & sign) != 0;
    unsigned base = !an && !sn ? 79u : an && sn ? 80u : !an ? 90u : 93u;
    /* The most negative source stays negative when the microcode negates
     * it, which skips a clock of the sign fix-up when only it was
     * negative. */
    if (!an && src == sign) base--;
    uint16_t abs_a = an ? (uint16_t)((0u - a) & mask) : a;
    return base + (w ? 48u : 0u) + popcount(abs_a) + fits;
}

/* The divide loop's variable clocks for num / d (no overflow). */
static unsigned div_loop(uint32_t num, uint16_t d, int w) {
    unsigned nb = w ? 16u : 8u, v = 0, last = 0;
    uint32_t mask = w ? 0xFFFFu : 0xFFu, hi = (num >> nb) & mask, lo = num & mask;
    for (unsigned i = 0; i < nb; i++) {
        unsigned carry = (hi >> (nb - 1)) & 1u;
        hi = ((hi << 1) | (lo >> (nb - 1))) & mask;
        lo = (lo << 1) & mask;
        if (carry || hi >= d) { hi = (hi - d) & mask; lo |= 1u; v += carry ? 0u : 1u; last = 1; }
        else last = 0;
    }
    return v + 2 * last;
}

/* Clocks to the end, or with `fails` to the type 0 request. */
static unsigned div_clocks(uint32_t num, uint16_t src, int w, int sgn, int fails) {
    unsigned nb = w ? 16u : 8u;
    uint32_t mask = w ? 0xFFFFu : 0xFFu, nmask = w ? 0xFFFFFFFFu : 0xFFFFu;
    src &= (uint16_t)mask;
    num &= nmask;
    if (!sgn) return fails ? 16u : (w ? 144u : 80u) + div_loop(num, src, w);
    unsigned nn = (num >> (2 * nb - 1)) & 1u, dn = (src >> (nb - 1)) & 1u;
    uint32_t an = nn ? ((0u - num) & nmask) : num;
    uint16_t ad = dn ? (uint16_t)((0u - src) & mask) : src;
    if (fails && ((an >> nb) & mask) >= ad) return 26u + 4u * nn - dn;
    unsigned n = (w ? 165u : 101u) + 4u * nn - dn + div_loop(an, ad, w);
    return fails ? n - 4u : n;
}

/* 0 on a divide error. */
static int divide(cpu *c, uint16_t src, int w, int sgn) {
    if (!w) {
        uint16_t ax = c->r[MX_AX];
        if (!(uint8_t)src) return 0;
        if (!sgn) {
            unsigned q = ax / (uint8_t)src, r = ax % (uint8_t)src;
            if (q > 0xFFu) return 0;
            c->r[MX_AX] = (uint16_t)(q | r << 8);
        } else {
            int32_t a = (int16_t)ax, d = (int8_t)src, q = a / d, r = a % d;
            if (c->rep) q = -q;
            if (q > 127 || q < -127) return 0;
            c->r[MX_AX] = (uint16_t)((uint8_t)q | (uint16_t)(uint8_t)r << 8);
        }
    } else {
        uint32_t a = (uint32_t)c->r[MX_DX] << 16 | c->r[MX_AX];
        if (!src) return 0;
        if (!sgn) {
            uint32_t q = a / src, r = a % src;
            if (q > 0xFFFFu) return 0;
            c->r[MX_AX] = (uint16_t)q;
            c->r[MX_DX] = (uint16_t)r;
        } else {
            int64_t s = (int32_t)a, d = (int16_t)src, q = s / d, r = s % d;
            if (c->rep) q = -q;
            if (q > 32767 || q < -32767) return 0;
            c->r[MX_AX] = (uint16_t)q;
            c->r[MX_DX] = (uint16_t)r;
        }
    }
    return 1;
}

/* --- shifts and rotates ----------------------------------------------------------- */

static uint16_t shift(cpu *c, unsigned op, uint16_t v, unsigned count, int w) {
    uint16_t mask = w ? 0xFFFFu : 0xFFu, msb = w ? 0x8000u : 0x80u;
    unsigned cf;
    v &= mask;
    while (count--) {
        switch (op) {
        case 0: cf = (v & msb) != 0; v = (uint16_t)(((v << 1) | cf) & mask); setf(c, MX_CF, cf); setf(c, MX_OF, !(v & msb) != !cf); break;
        case 1: cf = v & 1u; v = (uint16_t)((v >> 1) | (cf ? msb : 0)); setf(c, MX_CF, cf); setf(c, MX_OF, !(v & msb) != !(v & (msb >> 1))); break;
        case 2: cf = (v & msb) != 0; v = (uint16_t)(((v << 1) | (c->flags & MX_CF)) & mask); setf(c, MX_CF, cf); setf(c, MX_OF, !(v & msb) != !cf); break;
        case 3: cf = v & 1u; v = (uint16_t)((v >> 1) | ((c->flags & MX_CF) ? msb : 0)); setf(c, MX_CF, cf); setf(c, MX_OF, !(v & msb) != !(v & (msb >> 1))); break;
        case 4: cf = (v & msb) != 0; v = (uint16_t)((v << 1) & mask); setf(c, MX_CF, cf); setf(c, MX_OF, !(v & msb) != !cf); szp(c, v, w); setf(c, MX_AF, 0); break;
        case 5: cf = v & 1u; setf(c, MX_OF, (v & msb) != 0); v = (uint16_t)(v >> 1); setf(c, MX_CF, cf); szp(c, v, w); setf(c, MX_AF, 0); break;
        case 6: v = mask; c->flags &= (uint16_t)~(MX_CF | MX_OF | MX_AF); szp(c, v, w); break;
        default: cf = v & 1u; v = (uint16_t)((v >> 1) | (v & msb)); setf(c, MX_CF, cf); setf(c, MX_OF, 0); szp(c, v, w); setf(c, MX_AF, 0); break;
        }
    }
    return v;
}

/* --- string instructions ---------------------------------------------------------- */

/* One element of MOVS/CMPS/STOS/LODS/SCAS (opcode A4h-AFh): its bus
 * cycles, from the first request to the last cycle's return. */
static void string_once(cpu *c, uint8_t op, int w) {
    int16_t d = (int16_t)((c->flags & MX_DF) ? -(1 + w) : (1 + w));
    uint16_t a, b;
    switch (op & 0xFEu) {
    case 0xA4:
        a = rd(c, dseg(c), c->r[MX_SI], w);
        clk(c, 1);
        wr(c, SEG_ES, c->r[MX_DI], w, a);
        c->r[MX_SI] = (uint16_t)(c->r[MX_SI] + d);
        c->r[MX_DI] = (uint16_t)(c->r[MX_DI] + d);
        break;
    case 0xA6:
        a = rd(c, dseg(c), c->r[MX_SI], w);
        clk(c, 2);
        b = rd(c, SEG_ES, c->r[MX_DI], w);
        alu(c, 7, a, b, w);
        c->r[MX_SI] = (uint16_t)(c->r[MX_SI] + d);
        c->r[MX_DI] = (uint16_t)(c->r[MX_DI] + d);
        break;
    case 0xAA:
        wr(c, SEG_ES, c->r[MX_DI], w, c->r[MX_AX]);
        c->r[MX_DI] = (uint16_t)(c->r[MX_DI] + d);
        break;
    case 0xAC:
        a = rd(c, dseg(c), c->r[MX_SI], w);
        setr(c, MX_AX, w, a);
        c->r[MX_SI] = (uint16_t)(c->r[MX_SI] + d);
        break;
    default:
        b = rd(c, SEG_ES, c->r[MX_DI], w);
        alu(c, 7, getr(c, MX_AX, w), b, w);
        c->r[MX_DI] = (uint16_t)(c->r[MX_DI] + d);
        break;
    }
}

static int irq_pending(const cpu *c) { return c->nmi_pending || (c->intr && (c->flags & MX_IF)); }

/* Clocks per kind (MOVS CMPS STOS LODS SCAS): before the first access
 * without and with a repeat prefix, after the last access without a
 * prefix, between repeats, and after the last repeat. */
static const uint8_t STR_PRE[5] = {2, 3, 2, 2, 4}, STR_REP_PRE[5] = {9, 10, 9, 9, 11};
static const uint8_t STR_POST[5] = {4, 4, 4, 3, 4}, STR_NEXT[5] = {6, 8, 6, 7, 9}, STR_LAST[5] = {5, 6, 5, 6, 6};

static void string_op(cpu *c, uint8_t op) {
    int w = op & 1;
    unsigned kind = (op & 0xFEu) == 0xA4 ? 0 : (op & 0xFEu) == 0xA6 ? 1 : (op & 0xFEu) == 0xAA ? 2 : (op & 0xFEu) == 0xAC ? 3 : 4;
    if (!c->rep) {
        clk(c, STR_PRE[kind]);
        string_once(c, op, w);
        clk(c, STR_POST[kind]);
        return;
    }
    if (c->r[MX_CX] == 0) { clk(c, 6); return; }
    clk(c, STR_REP_PRE[kind]);
    for (;;) {
        string_once(c, op, w);
        c->r[MX_CX]--;
        if ((kind == 1 || kind == 4) && (((c->flags & MX_ZF) != 0) != (c->rep == 0xF3))) { clk(c, 5); return; }
        if (c->r[MX_CX] == 0) { clk(c, STR_LAST[kind]); return; }
        if (irq_pending(c)) {
            /* Interrupted: resume at the last prefix (the 8086 keeps only it). */
            clk(c, STR_LAST[kind]);
            c->ip = (uint16_t)(c->ip - 2);
            flush(c);
            return;
        }
        clk(c, STR_NEXT[kind]);
    }
}
/* --- BCD adjustments -------------------------------------------------------------- */

static void daa_das(cpu *c, int sub) {
    uint8_t al = get8(c, 0), old = al;
    int cf = (c->flags & MX_CF) != 0, af = (c->flags & MX_AF) != 0;
    if ((al & 0x0Fu) > 9 || af) {
        al = (uint8_t)(sub ? al - 6 : al + 6);
        setf(c, MX_AF, 1);
    } else setf(c, MX_AF, 0);
    if (old > (af ? 0x9Fu : 0x99u) || cf) {
        al = (uint8_t)(sub ? al - 0x60 : al + 0x60);
        setf(c, MX_CF, 1);
    } else setf(c, MX_CF, 0);
    set8(c, 0, al);
    szp(c, al, 0);
}

static void aaa_aas(cpu *c, int sub) {
    uint8_t al = get8(c, 0);
    if ((al & 0x0Fu) > 9 || (c->flags & MX_AF)) {
        set8(c, 0, (uint8_t)(sub ? al - 6 : al + 6));
        set8(c, 4, (uint8_t)(get8(c, 4) + (sub ? -1 : 1)));
        setf(c, MX_AF | MX_CF, 1);
    } else setf(c, MX_AF | MX_CF, 0);
    set8(c, 0, get8(c, 0) & 0x0Fu);
}

/* --- the instructions ------------------------------------------------------------- */

static int is_prefix(uint8_t op) {
    return op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E || op == 0xF0 || op == 0xF1 || op == 0xF2 || op == 0xF3;
}

/* Returns 0, or the vector of an exception the instruction raised (with
 * 0x100 set when it is a divide error). */
static int execute(cpu *c, uint8_t op) {
    int w = op & 1, d = (op >> 1) & 1;
    uint16_t v, t;
    switch (op) {
    /* ALU r/m, reg and reg, r/m; ALU acc, imm */
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x38: case 0x39: case 0x3A: case 0x3B: {
        unsigned aop = (op >> 3) & 7u;
        modrm(c);
        if (d) {
            v = rm_read(c, w);
            clk(c, c->ea_mem ? 3 : 1);
            t = alu(c, aop, getr(c, c->reg, w), v, w);
            if (aop != 7) setr(c, c->reg, w, t);
        } else {
            v = rm_read(c, w);
            t = alu(c, aop, v, getr(c, c->reg, w), w);
            if (!c->ea_mem) { clk(c, 1); if (aop != 7) setr(c, c->rm, w, t); }
            else if (aop == 7) clk(c, 3);
            else { clk(c, 5); rm_write(c, w, t); clk(c, 1); }
        }
        break;
    }
    case 0x04: case 0x05: case 0x0C: case 0x0D: case 0x14: case 0x15: case 0x1C: case 0x1D:
    case 0x24: case 0x25: case 0x2C: case 0x2D: case 0x34: case 0x35: case 0x3C: case 0x3D: {
        unsigned aop = (op >> 3) & 7u;
        clk(c, 1);
        v = w ? q_read16(c) : q_read(c, 0);
        t = alu(c, aop, getr(c, MX_AX, w), v, w);
        if (aop != 7) setr(c, MX_AX, w, t);
        if (!w) clk(c, 1);
        break;
    }
    /* segment register push and pop */
    case 0x06: case 0x0E: case 0x16: case 0x1E:
        clk(c, 4);
        push(c, c->s[op >> 3]);
        clk(c, 1);
        break;
    case 0x07: case 0x0F: case 0x17: case 0x1F:
        clk(c, 1);
        c->s[op >> 3] = pop(c);
        c->inhibit = 1;
        break;
    case 0x27: clk(c, 3); daa_das(c, 0); break;
    case 0x2F: clk(c, 3); daa_das(c, 1); break;
    case 0x37: case 0x3F:
        clk(c, ((get8(c, 0) & 0x0Fu) > 9 || (c->flags & MX_AF)) ? 7 : 8);
        aaa_aas(c, op == 0x3F);
        break;
    /* INC, DEC, PUSH, POP reg16 */
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        clk(c, 1);
        c->r[op & 7u] = incdec(c, c->r[op & 7u], op & 8u, 1);
        break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        clk(c, 4);
        c->r[MX_SP] = (uint16_t)(c->r[MX_SP] - 2);
        wr(c, SEG_SS, c->r[MX_SP], 1, c->r[op & 7u]);
        clk(c, 1);
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        clk(c, 1);
        v = pop(c);
        c->r[op & 7u] = v;
        break;
    /* Jcc (60h-6Fh are their aliases) */
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
    case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F:
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        clk(c, 1);
        v = (uint16_t)(int8_t)q_read(c, 0);
        if (cond(c, op & 15u)) {
            clk(c, 1);
            susp(c);
            clk(c, 1);
            corr(c);
            clk(c, 3);
            jump(c, (uint16_t)(c->ip + v));
            clk(c, 1);
        } else clk(c, 1);
        break;
    /* group 1: ALU r/m, imm (82h is 80h) */
    case 0x80: case 0x81: case 0x82: case 0x83: {
        modrm(c);
        unsigned aop = c->reg;
        v = rm_read(c, w);
        if (c->ea_mem) clk(c, 2);
        if (op == 0x81) t = q_read16(c);
        else { t = q_read(c, 0); if (op == 0x83) t = (uint16_t)(int8_t)t; }
        uint16_t r = alu(c, aop, v, t, w);
        if (!c->ea_mem) { if (op != 0x81) clk(c, 1); if (aop != 7) setr(c, c->rm, w, r); }
        else if (aop == 7) clk(c, op == 0x81 ? 1 : 2);
        else { clk(c, op == 0x81 ? 2 : 3); rm_write(c, w, r); clk(c, 1); }
        break;
    }
    case 0x84: case 0x85:
        modrm(c);
        v = rm_read(c, w);
        alu(c, 4, v, getr(c, c->reg, w), w);
        clk(c, c->ea_mem ? 3 : 1);
        break;
    case 0x86: case 0x87:
        modrm(c);
        v = rm_read(c, w);
        if (c->ea_mem) { clk(c, 6); rm_write(c, w, getr(c, c->reg, w)); clk(c, 1); }
        else { clk(c, 2); setr(c, c->rm, w, getr(c, c->reg, w)); }
        setr(c, c->reg, w, v);
        break;
    /* MOV */
    case 0x88: case 0x89:
        modrm(c);
        if (c->ea_mem) { clk(c, 4); rm_write(c, w, getr(c, c->reg, w)); clk(c, 1); }
        else setr(c, c->rm, w, getr(c, c->reg, w));
        break;
    case 0x8A: case 0x8B:
        modrm(c);
        v = rm_read(c, w);
        setr(c, c->reg, w, v);
        if (c->ea_mem) clk(c, 2);
        break;
    case 0x8C:
        modrm(c);
        if (c->ea_mem) { clk(c, 3); rm_write(c, 1, c->s[c->reg & 3u]); clk(c, 1); }
        else setr(c, c->rm, 1, c->s[c->reg & 3u]);
        break;
    case 0x8D:
        modrm(c);
        if (!c->ea_mem) c->ea_off = c->last_ea;
        c->r[c->reg] = c->ea_off;
        clk(c, 2);
        break;
    case 0x8E:
        modrm(c);
        v = rm_read(c, 1);
        c->s[c->reg & 3u] = v;
        if (c->ea_mem) clk(c, 2);
        c->inhibit = 1;
        break;
    case 0x8F:
        modrm(c);
        clk(c, c->ea_mem ? 3 : 1);
        v = pop(c);
        clk(c, c->ea_mem ? 3 : 1);
        rm_write(c, 1, v);
        if (c->ea_mem) clk(c, 1);
        break;
    /* XCHG AX, reg; NOP is XCHG AX, AX */
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        clk(c, 2);
        v = c->r[op & 7u];
        c->r[op & 7u] = c->r[MX_AX];
        c->r[MX_AX] = v;
        break;
    case 0x98: clk(c, 1); c->r[MX_AX] = (uint16_t)(int8_t)get8(c, 0); break;
    case 0x99:
        clk(c, (c->r[MX_AX] & 0x8000u) ? 5 : 4);
        c->r[MX_DX] = (c->r[MX_AX] & 0x8000u) ? 0xFFFFu : 0;
        break;
    case 0x9A: {
        clk(c, 1);
        uint16_t nip = q_read16(c), ncs = q_read16(c);
        susp(c);
        corr(c);
        clk(c, 2);
        push(c, c->s[SEG_CS]);
        c->s[SEG_CS] = ncs;
        clk(c, 5);
        t = c->ip;
        jump(c, nip);
        clk(c, 3);
        push(c, t);
        clk(c, 1);
        break;
    }
    case 0x9B:
        clk(c, 2);
        while (c->bus->test && c->bus->test(c->bus->ctx)) {
            if (irq_pending(c)) { c->ip--; flush(c); return 0; }
            clk(c, 5);
        }
        break;
    case 0x9C: clk(c, 4); push(c, c->flags); clk(c, 1); break;
    case 0x9D:
        clk(c, 1);
        v = pop(c);
        c->flags = (uint16_t)((v & 0x0FD5u) | 0xF002u);
        break;
    case 0x9E:
        clk(c, 3);
        c->flags = (uint16_t)((c->flags & 0xFF00u) | (get8(c, 4) & 0xD5u) | 0x02u);
        break;
    case 0x9F: clk(c, 1); set8(c, 4, (uint8_t)c->flags); break;
    /* MOV acc, moffs */
    case 0xA0: case 0xA1:
        clk(c, 1);
        t = q_read16(c);
        v = rd(c, dseg(c), t, w);
        setr(c, MX_AX, w, v);
        break;
    case 0xA2: case 0xA3:
        clk(c, 1);
        t = q_read16(c);
        clk(c, 1);
        wr(c, dseg(c), t, w, c->r[MX_AX]);
        clk(c, 1);
        break;
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(c, op);
        break;
    case 0xA8: case 0xA9:
        clk(c, 1);
        v = w ? q_read16(c) : q_read(c, 0);
        alu(c, 4, getr(c, MX_AX, w), v, w);
        if (!w) clk(c, 1);
        break;
    /* MOV reg, imm */
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        clk(c, 1);
        set8(c, op & 7u, q_read(c, 0));
        clk(c, 1);
        break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        clk(c, 1);
        c->r[op & 7u] = q_read16(c);
        break;
    /* RET near (C0h/C1h are aliases) */
    case 0xC0: case 0xC1: case 0xC2: case 0xC3:
        t = 0;
        clk(c, 1);
        if (!(op & 1u)) { t = q_read16(c); clk(c, 1); }
        susp(c);
        v = pop(c);
        if (!(op & 1u)) { c->r[MX_SP] = (uint16_t)(c->r[MX_SP] + t); clk(c, 1); }
        clk(c, 1);
        jump(c, v);
        clk(c, 1);
        break;
    case 0xC4: case 0xC5:
        modrm(c);
        if (!c->ea_mem) { c->ea_off = c->last_ea; c->ea_seg = dseg(c); }
        v = rd(c, c->ea_seg, c->ea_off, 1);
        clk(c, 4);
        t = rd(c, c->ea_seg, (uint16_t)(c->ea_off + 2), 1);
        c->r[c->reg] = v;
        c->s[op == 0xC4 ? SEG_ES : SEG_DS] = t;
        break;
    case 0xC6: case 0xC7:
        modrm(c);
        if (c->ea_mem) clk(c, 2);
        v = w ? q_read16(c) : q_read(c, 0);
        if (c->ea_mem) { clk(c, w ? 1 : 2); rm_write(c, w, v); clk(c, 1); }
        else { if (!w) clk(c, 1); setr(c, c->rm, w, v); }
        break;
    /* RET far (C8h/C9h are aliases) */
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: {
        uint16_t n = 0;
        if (!(op & 1u)) { clk(c, 1); n = q_read16(c); clk(c, 1); }
        else clk(c, 3);
        susp(c);
        v = pop(c);
        clk(c, 3);
        t = pop(c);
        c->r[MX_SP] = (uint16_t)(c->r[MX_SP] + n);
        c->s[SEG_CS] = t;
        jump(c, v);
        clk(c, 1);
        break;
    }
    case 0xCC:
        clk(c, 7);
        interrupt(c, 3, c->ip);
        break;
    case 0xCD:
        clk(c, 1);
        v = q_read(c, 0);
        clk(c, 3);
        interrupt(c, (uint8_t)v, c->ip);
        break;
    case 0xCE:
        clk(c, 3);
        if (c->flags & MX_OF) { clk(c, 5); interrupt(c, 4, c->ip); }
        break;
    case 0xCF:
        clk(c, 3);
        susp(c);
        v = pop(c);
        clk(c, 3);
        t = pop(c);
        c->s[SEG_CS] = t;
        jump(c, v);
        clk(c, 2);
        v = pop(c);
        c->flags = (uint16_t)((v & 0x0FD5u) | 0xF002u);
        clk(c, 1);
        break;
    /* shifts and rotates (reg 6: SETMO/SETMOC) */
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        modrm(c);
        unsigned n = (op & 2u) ? get8(c, MX_CX) : 1u;
        v = rm_read(c, w);
        v = shift(c, c->reg, v, n, w);
        if (op & 2u) clk(c, 4 * n + (c->ea_mem ? 9 : 6));
        else if (c->ea_mem) clk(c, 4);
        if (c->ea_mem) { rm_write(c, w, v); clk(c, 1); }
        else setr(c, c->rm, w, v);
        break;
    }
    case 0xD4: {
        clk(c, 1);
        uint8_t base = q_read(c, 0);
        if (!base) { szp(c, 0, 0); clk(c, 11); return 0x100; }
        uint8_t al = get8(c, 0);
        set8(c, 4, (uint8_t)(al / base));
        set8(c, 0, (uint8_t)(al % base));
        szp(c, get8(c, 0), 0);
        clk(c, 74 + div_loop(al, base, 0));
        break;
    }
    case 0xD5: {
        clk(c, 1);
        uint8_t base = q_read(c, 0);
        uint8_t al = (uint8_t)alu(c, 0, get8(c, 0), (uint16_t)(get8(c, 4) * base), 0);
        set8(c, 0, al);
        set8(c, 4, 0);
        clk(c, 56 + popcount(base));
        break;
    }
    case 0xD6: clk(c, (c->flags & MX_CF) ? 3 : 2); set8(c, 0, (c->flags & MX_CF) ? 0xFFu : 0); break;
    case 0xD7:
        clk(c, 4);
        set8(c, 0, (uint8_t)rd(c, dseg(c), (uint16_t)(c->r[MX_BX] + get8(c, 0)), 0));
        break;
    /* ESC: the operand is read for the coprocessor */
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
        modrm(c);
        mx_cpu_esc e;
        memset(&e, 0, sizeof e);
        if (c->ea_mem) { e.address = lin(c->s[c->ea_seg], c->ea_off); e.value = rd(c, c->ea_seg, c->ea_off, 1); clk(c, 2); }
        if (c->bus->esc) {
            e.opcode = op;
            e.modrm = c->modrm;
            e.memory = c->ea_mem;
            e.insn_address = lin(c->s[SEG_CS], c->insn_ip);
            e.clock = c->clock;
            c->bus->esc(c->bus->ctx, &e);
        }
        break;
    }
    /* LOOPNE, LOOPE, LOOP, JCXZ */
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: {
        int take;
        clk(c, 3);
        v = (uint16_t)(int8_t)q_read(c, 0);
        if (op == 0xE3) take = c->r[MX_CX] == 0;
        else {
            c->r[MX_CX]--;
            take = c->r[MX_CX] != 0;
            if (op == 0xE0) take = take && !(c->flags & MX_ZF);
            if (op == 0xE1) take = take && (c->flags & MX_ZF);
        }
        if (!take) { clk(c, 1); break; }
        if (op != 0xE2) { clk(c, 1); susp(c); clk(c, 1); }
        else { susp(c); clk(c, 1); }
        corr(c);
        clk(c, 3);
        jump(c, (uint16_t)(c->ip + v));
        clk(c, 1);
        break;
    }
    case 0xE4: case 0xE5:
        clk(c, 1);
        t = q_read(c, 0);
        clk(c, 1);
        setr(c, MX_AX, w, in(c, t, w));
        break;
    case 0xE6: case 0xE7:
        clk(c, 1);
        t = q_read(c, 0);
        clk(c, 2);
        out(c, t, w, c->r[MX_AX]);
        clk(c, 1);
        break;
    case 0xE8:
        clk(c, 1);
        v = q_read(c, 0);
        v = (uint16_t)(v | q_read_susp(c) << 8);
        corr(c);
        clk(c, 3);
        t = c->ip;
        jump(c, (uint16_t)(c->ip + v));
        clk(c, 3);
        push(c, t);
        clk(c, 1);
        break;
    case 0xE9:
        clk(c, 1);
        v = q_read(c, 0);
        v = (uint16_t)(v | q_read_susp(c) << 8);
        corr(c);
        clk(c, 3);
        jump(c, (uint16_t)(c->ip + v));
        clk(c, 1);
        break;
    case 0xEA:
        clk(c, 1);
        v = q_read16(c);
        t = q_read(c, 0);
        t = (uint16_t)(t | q_read_susp(c) << 8);
        corr(c);
        clk(c, 1);
        c->s[SEG_CS] = t;
        jump(c, v);
        clk(c, 1);
        break;
    case 0xEB:
        clk(c, 1);
        v = (uint16_t)(int8_t)q_read(c, 0);
        susp(c);
        clk(c, 1);
        corr(c);
        clk(c, 3);
        jump(c, (uint16_t)(c->ip + v));
        clk(c, 1);
        break;
    case 0xEC: case 0xED:
        clk(c, 1);
        setr(c, MX_AX, w, in(c, c->r[MX_DX], w));
        break;
    case 0xEE: case 0xEF:
        clk(c, 2);
        out(c, c->r[MX_DX], w, c->r[MX_AX]);
        clk(c, 1);
        break;
    case 0xF4:
        clk(c, 1);
        c->halted = 1;
        break;
    case 0xF5: clk(c, 1); c->flags ^= MX_CF; break;
    /* group 3 (reg 1 is TEST) */
    case 0xF6: case 0xF7:
        modrm(c);
        v = rm_read(c, w);
        switch (c->reg) {
        case 0: case 1:
            clk(c, c->ea_mem ? 2 : 1);
            t = w ? q_read16(c) : q_read(c, 0);
            alu(c, 4, v, t, w);
            clk(c, c->ea_mem ? (w ? 1 : 2) : (w ? 0 : 1));
            break;
        case 2:
            clk(c, c->ea_mem ? 4 : 1);
            rm_write(c, w, (uint16_t)~v);
            if (c->ea_mem) clk(c, 1);
            break;
        case 3:
            clk(c, c->ea_mem ? 4 : 1);
            t = alu(c, 5, 0, v, w);
            rm_write(c, w, t);
            if (c->ea_mem) clk(c, 1);
            break;
        case 4: case 5: {
            uint16_t acc = c->r[MX_AX];
            mul(c, v, w, c->reg == 5);
            clk(c, mul_clocks(c, v, acc, w, c->reg == 5) - (c->ea_mem ? MULDIV_MEM : 2u));
            break;
        }
        default: {
            uint32_t num = w ? ((uint32_t)c->r[MX_DX] << 16 | c->r[MX_AX]) : c->r[MX_AX];
            int ok = divide(c, v, w, c->reg == 7);
            clk(c, div_clocks(num, v, w, c->reg == 7, !ok) - (c->ea_mem ? MULDIV_MEM : 2u));
            if (!ok) return 0x100;
            break;
        }
        }
        break;
    case 0xF8: case 0xF9: clk(c, 1); setf(c, MX_CF, op & 1); break;
    case 0xFA: case 0xFB:
        clk(c, 1);
        if (op & 1) { if (!(c->flags & MX_IF)) c->inhibit = 1; c->flags |= MX_IF; }
        else c->flags &= (uint16_t)~MX_IF;
        break;
    case 0xFC: case 0xFD: clk(c, 1); setf(c, MX_DF, op & 1); break;
    /* group 4 and 5 */
    case 0xFE: case 0xFF:
        modrm(c);
        switch (c->reg) {
        case 0: case 1:
            v = rm_read(c, w);
            v = incdec(c, v, c->reg, w);
            if (c->ea_mem) { clk(c, 4); rm_write(c, w, v); clk(c, 1); }
            else { clk(c, 1); setr(c, c->rm, w, v); }
            break;
        case 2:
            v = rm_read(c, w);
            if (!w) v = (uint16_t)(v | 0xFF00u);
            if (c->ea_mem) clk(c, 2);
            susp(c);
            corr(c);
            clk(c, 3);
            t = c->ip;
            jump(c, v);
            clk(c, 3);
            push(c, t);
            clk(c, 1);
            break;
        case 3: {
            if (!c->ea_mem) { c->ea_off = c->last_ea; c->ea_seg = dseg(c); }
            v = rd(c, c->ea_seg, c->ea_off, w);
            clk(c, 3);
            t = rd(c, c->ea_seg, (uint16_t)(c->ea_off + 2), w);
            clk(c, 1);
            susp(c);
            corr(c);
            clk(c, 2);
            push(c, c->s[SEG_CS]);
            c->s[SEG_CS] = t;
            clk(c, 5);
            uint16_t ret = c->ip;
            jump(c, v);
            clk(c, 3);
            push(c, ret);
            clk(c, 1);
            break;
        }
        case 4:
            v = rm_read(c, w);
            if (!w) v = (uint16_t)(v | 0xFF00u);
            if (c->ea_mem) clk(c, 2);
            susp(c);
            corr(c);
            jump(c, v);
            clk(c, 1);
            break;
        case 5:
            if (!c->ea_mem) { c->ea_off = c->last_ea; c->ea_seg = dseg(c); }
            v = rd(c, c->ea_seg, c->ea_off, w);
            susp(c);
            clk(c, 3);
            corr(c);
            t = rd(c, c->ea_seg, (uint16_t)(c->ea_off + 2), w);
            c->s[SEG_CS] = t;
            jump(c, v);
            clk(c, 1);
            break;        default:
            v = rm_read(c, w);
            clk(c, c->ea_mem ? 5 : 4);
            c->r[MX_SP] = (uint16_t)(c->r[MX_SP] - 2);
            if (!c->ea_mem && c->rm == MX_SP) v = c->r[MX_SP];
            wr(c, SEG_SS, c->r[MX_SP], 1, w ? v : (uint16_t)(v | 0xFF00u));
            clk(c, 1);
            break;
        }
        break;
    default:
        break;
    }
    return 0;
}

/* --- public interface ------------------------------------------------------------- */

void mx_cpu_init(mx_cpu8086 *c, int model, const mx_cpu_bus *bus) {
    memset(c, 0, sizeof *c);
    c->model = model == MX_CPU_8086 ? MX_CPU_8086 : MX_CPU_8088;
    c->bus = bus;
    c->qsize = c->model == MX_CPU_8086 ? 6 : 4;
    mx_cpu_reset(c);
}

void mx_cpu_reset(mx_cpu8086 *c) {
    memset(c->r, 0, sizeof c->r);
    memset(c->s, 0, sizeof c->s);
    c->s[SEG_CS] = 0xFFFFu;
    c->ip = 0;
    c->flags = 0xF002u;
    c->qlen = c->qhead = 0;
    c->pc = 0;
    c->bt = MX_T_I;
    c->bop = MX_BUS_PASV;
    c->bnext = NEXT_NONE;
    c->bdelay = 0;
    c->bpending = NEXT_NONE;
    c->bsusp = 0;
    c->bfull = 0;
    c->bdiscard = 0;
    c->bhold = 0;
    c->rq_active = 0;
    c->qs_op = c->qs_now = MX_Q_NONE;
    c->lock = c->lock_next = 0;
    c->halted = 0;
    c->inhibit = 0;
    c->nmi_pending = 0;
    c->trap_armed = 0;
}

static void service(cpu *c, int tf_before) {
    if (c->inhibit) { c->inhibit = 0; return; }
    if (c->nmi_pending) {
        c->nmi_pending = 0;
        c->halted = 0;
        clk(c, 2);
        interrupt(c, 2, c->ip);
    } else if (c->intr && (c->flags & MX_IF)) {
        c->halted = 0;
        clk(c, 1);
        c->lock_next = 1;
        request(c, MX_BUS_INTA, -1, 0, 0, 0);
        finish(c);
        clk(c, 2);
        request(c, MX_BUS_INTA, -1, 0, 1, 0);
        c->rq_phases = 1;
        c->lock_next = 0;
        uint8_t vector = (uint8_t)finish(c);
        clk(c, 1);
        interrupt(c, vector, c->ip);
    } else if (tf_before && (c->flags & MX_TF)) {
        clk(c, 2);
        interrupt(c, 1, c->ip);
    }
}

unsigned mx_cpu_step(mx_cpu8086 *c) {
    uint64_t start = c->clock;
    if (c->halted) {
        if (irq_pending(c)) service(c, 0);
        else tick(c);
        return (unsigned)(c->clock - start);
    }
    int tf = (c->flags & MX_TF) != 0;
    c->seg_override = -1;
    c->rep = 0;
    c->prefixes = 0;
    c->insn_ip = c->ip;
    uint8_t op;
    for (;;) {
        op = q_read(c, 1);
        if (!is_prefix(op)) break;
        c->prefixes++;
        if (op == 0xF2 || op == 0xF3) c->rep = op;
        else if (op == 0xF0 || op == 0xF1) c->lock_next = 1;
        else c->seg_override = (op >> 3) & 3;
        clk(c, 1);
    }
    c->opcode = op;
    int fault = execute(c, op);
    c->lock_next = 0;
    if (fault) {
        interrupt(c, (uint8_t)fault, c->ip);
        c->inhibit = 0;
    }
    if (!c->halted) service(c, tf);
    return (unsigned)(c->clock - start);
}

void mx_cpu_set_intr(mx_cpu8086 *c, int level) { c->intr = level != 0; }

void mx_cpu_set_nmi(mx_cpu8086 *c, int level) {
    if (level && !c->nmi_line) c->nmi_pending = 1;
    c->nmi_line = level != 0;
}

int mx_cpu_halted(const mx_cpu8086 *c) { return c->halted; }
uint64_t mx_cpu_clock(const mx_cpu8086 *c) { return c->clock; }

uint16_t mx_cpu_get(const mx_cpu8086 *c, int reg) {
    if (reg >= MX_AX && reg <= MX_DI) return c->r[reg];
    if (reg >= MX_ES && reg <= MX_DS) return c->s[reg - MX_ES];
    if (reg == MX_IP) return c->ip;
    if (reg == MX_FLAGS) return c->flags;
    return 0;
}

void mx_cpu_set(mx_cpu8086 *c, int reg, uint16_t value) {
    if (reg >= MX_AX && reg <= MX_DI) c->r[reg] = value;
    else if (reg >= MX_ES && reg <= MX_DS) c->s[reg - MX_ES] = value;
    else if (reg == MX_FLAGS) c->flags = (uint16_t)((value & 0x0FD5u) | 0xF002u);
    if (reg == MX_IP) c->ip = value;
    if (reg == MX_IP || reg == MX_CS) {
        c->qlen = c->qhead = 0;
        c->pc = c->ip;
        c->bsusp = 0;
        c->bfull = 0;
        c->bdelay = 0;
    }
}

void mx_cpu_set_queue(mx_cpu8086 *c, const uint8_t *bytes, unsigned n) {
    if (n > c->qsize) n = c->qsize;
    c->qhead = 0;
    c->qlen = 0;
    for (unsigned i = 0; i < n; i++) q_push(c, bytes[i]);
    c->pc = (uint16_t)(c->ip + n);
    c->bfull = !can_fetch(c, 0);
    c->bt = MX_T_I;
    c->bdelay = 0;
    c->bpending = NEXT_NONE;
    c->bnext = NEXT_NONE;
}

unsigned mx_cpu_queue(const mx_cpu8086 *c, uint8_t *out) {
    for (unsigned i = 0; i < c->qlen; i++) out[i] = c->q[(c->qhead + i) & 7u];
    return c->qlen;
}

/* The CPU conformance runner: SingleStepTests' 8088 and 8086 suites as
 * converted by tests/cpu_suite.py (the format is described there).
 *
 *   cpu_suite DIR [--sample N] [--only NAME] [--show K] [--quiet]
 *
 * Runs every file of DIR (or one), each test from its initial state (queue
 * contents installed as prefetched) through the test instruction and the
 * NOP after it, and compares:
 *   results  registers, flags under the file's undefined-flags mask, memory
 *            (a flags word the instruction pushed on the stack is compared
 *            under the same mask);
 *   cycles   the clocks from the instruction's first byte to the next
 *            instruction's, pin by pin: ALE and the latched address, bus
 *            status, T-state, segment status, the command lines, the data
 *            bus on transfers (only the driven half on the 8086), BHE on
 *            T1 (8086), the queue status and byte; and the final queue.
 * --sample N runs N tests per file spread over it; --show K prints the
 * first K failing tests of each kind with both traces (--brief: as events;
 * --grep TEXT: only tests whose name holds TEXT); --pass K prints K passing
 * ones; --test I prints test I of the file (with --only). Exit 1 when a
 * result fails. MezinXT's own code. */
#include "cpu8086.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CYCLES 20000
#define MAX_RAM 4096

typedef struct state {
    uint16_t regs[14];
    uint8_t qlen, queue[8];
    uint32_t nram;
    uint32_t addr[MAX_RAM];
    uint8_t val[MAX_RAM];
} state;

typedef struct cyc {
    uint32_t word;
    uint16_t data;
    uint8_t lines, qop, qbyte;
} cyc;

typedef struct test {
    uint32_t idx;
    char name[256];
    uint8_t nbytes, bytes[16];
    state init, fin;
    uint32_t ncycles;
    cyc cycles[MAX_CYCLES];
} test;

static uint8_t memory[1u << 20];
static uint32_t written[1u << 16];
static unsigned nwritten;

/* What the CPU did, clock by clock. */
static mx_cpu_cycle got[MAX_CYCLES * 2];
static uint8_t got_queue[MAX_CYCLES * 2][8];
static unsigned ngot;
static mx_cpu8086 cpu;

/* The capture harness fed the instruction's bytes to the first code
 * fetches and NOPs to every later one, wherever it pointed. */
static unsigned code_left;
static uint8_t b_read(void *ctx, uint32_t a) {
    (void)ctx;
    if (cpu.bop == MX_BUS_CODE) {
        if (!code_left) return 0x90;
        code_left--;
    }
    return memory[a & 0xFFFFFu];
}
static void b_write(void *ctx, uint32_t a, uint8_t v) { (void)ctx; memory[a & 0xFFFFFu] = v; if (nwritten < sizeof written / sizeof written[0]) written[nwritten++] = a & 0xFFFFFu; }
static uint8_t b_in(void *ctx, uint16_t p) { (void)ctx; (void)p; return 0xFF; }
static void b_out(void *ctx, uint16_t p, uint8_t v) { (void)ctx; (void)p; (void)v; }
static void b_trace(void *ctx, const mx_cpu_cycle *c) {
    (void)ctx;
    if (ngot < sizeof got / sizeof got[0]) {
        got[ngot] = *c;
        mx_cpu_queue(&cpu, got_queue[ngot]);
        ngot++;
    }
}
static const mx_cpu_bus BUS = {NULL, b_read, b_write, b_in, b_out, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, b_trace};

/* --- reading the converted files ------------------------------------------------- */

typedef struct reader { const uint8_t *p, *end; int bad; } reader;
static uint32_t u8(reader *r) { if (r->p + 1 > r->end) { r->bad = 1; return 0; } return *r->p++; }
static uint32_t u16(reader *r) { uint32_t v = u8(r); return v | u8(r) << 8; }
static uint32_t u32(reader *r) { uint32_t v = u16(r); return v | u16(r) << 16; }

static void read_state(reader *r, state *s) {
    for (int i = 0; i < 14; i++) s->regs[i] = (uint16_t)u16(r);
    s->qlen = (uint8_t)u8(r);
    for (unsigned i = 0; i < s->qlen; i++) { uint8_t b = (uint8_t)u8(r); if (i < 8) s->queue[i] = b; }
    s->nram = u32(r);
    for (uint32_t i = 0; i < s->nram; i++) {
        uint32_t a = u32(r);
        uint8_t v = (uint8_t)u8(r);
        if (i < MAX_RAM) { s->addr[i] = a; s->val[i] = v; }
    }
    if (s->nram > MAX_RAM) r->bad = 1;
}

static int read_test(reader *r, test *t) {
    t->idx = u32(r);
    uint32_t n = u16(r);
    for (uint32_t i = 0; i < n; i++) { char ch = (char)u8(r); if (i < sizeof t->name - 1) t->name[i] = ch; }
    t->name[n < sizeof t->name ? n : sizeof t->name - 1] = 0;
    t->nbytes = (uint8_t)u8(r);
    for (unsigned i = 0; i < t->nbytes; i++) { uint8_t b = (uint8_t)u8(r); if (i < sizeof t->bytes) t->bytes[i] = b; }
    read_state(r, &t->init);
    read_state(r, &t->fin);
    t->ncycles = u32(r);
    for (uint32_t i = 0; i < t->ncycles; i++) {
        cyc c;
        c.word = u32(r);
        c.data = (uint16_t)u16(r);
        c.lines = (uint8_t)u8(r);
        c.qop = (uint8_t)u8(r);
        c.qbyte = (uint8_t)u8(r);
        if (i < MAX_CYCLES) t->cycles[i] = c;
    }
    if (t->ncycles > MAX_CYCLES) r->bad = 1;
    return !r->bad;
}

/* --- running one test --------------------------------------------------------------- */

/* Registers in the file's order (ax bx cx dx cs ss ds es sp bp si di ip flags). */
static const int ORDER[14] = {MX_AX, MX_BX, MX_CX, MX_DX, MX_CS, MX_SS, MX_DS, MX_ES, MX_SP, MX_BP, MX_SI, MX_DI, MX_IP, MX_FLAGS};
static const char *const REG_NAMES[14] = {"ax", "bx", "cx", "dx", "cs", "ss", "ds", "es", "sp", "bp", "si", "di", "ip", "flags"};

typedef struct outcome {
    int result_ok, cycles_ok, queue_ok;
    char why[256];
    unsigned start, end;       /* the instruction's clocks in got[] */
} outcome;

static int is_prefix(uint8_t b) { return b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0xF0 || b == 0xF1 || b == 0xF2 || b == 0xF3; }

static unsigned count_prefixes(const test *t, int model) {
    /* The instruction's bytes start at CS:IP. */
    uint32_t base = ((uint32_t)t->init.regs[4] << 4);
    unsigned n = 0;
    (void)model;
    for (;;) {
        uint8_t b = memory[(base + (uint16_t)(t->init.regs[12] + n)) & 0xFFFFFu];
        if (!is_prefix(b) || n > 14) break;
        n++;
    }
    return n;
}

static void run_test(const test *t, int model, uint16_t mask, outcome *o) {
    memset(o, 0, sizeof *o);
    for (uint32_t i = 0; i < t->init.nram; i++) memory[t->init.addr[i] & 0xFFFFFu] = t->init.val[i];
    unsigned prefixes = count_prefixes(t, model);
    mx_cpu_init(&cpu, model, &BUS);
    for (int i = 0; i < 14; i++) mx_cpu_set(&cpu, ORDER[i], t->init.regs[i]);
    mx_cpu_set_queue(&cpu, t->init.queue, t->init.qlen);
    code_left = t->nbytes > t->init.qlen ? t->nbytes - t->init.qlen : 0;
    ngot = 0;
    mx_cpu_step(&cpu);
    uint16_t regs[14];
    for (int i = 0; i < 14; i++) regs[i] = mx_cpu_get(&cpu, ORDER[i]);
    mx_cpu_step(&cpu);

    /* Results. */
    o->result_ok = 1;
    for (int i = 0; i < 14 && o->result_ok; i++) {
        uint16_t want = t->fin.regs[i], have = regs[i];
        if (i == 13) { want &= mask; have &= mask; }
        if (want != have) {
            o->result_ok = 0;
            snprintf(o->why, sizeof o->why, "%s %04X, want %04X", REG_NAMES[i], have, want);
        }
    }
    /* A flags word pushed by the instruction (an interrupt) holds the
     * undefined flags too: find it at the final SS:SP+4. */
    uint32_t flags_at = 0xFFFFFFFFu;
    if (mask != 0xFFFF && regs[12] == 0x0400 && regs[4] == 0) {
        uint16_t sp = regs[8];
        flags_at = (((uint32_t)regs[5] << 4) + (uint16_t)(sp + 4)) & 0xFFFFFu;
    }
    for (uint32_t i = 0; i < t->fin.nram && o->result_ok; i++) {
        uint32_t a = t->fin.addr[i] & 0xFFFFFu;
        uint8_t want = t->fin.val[i], have = memory[a];
        if (a == flags_at) { want &= (uint8_t)mask; have &= (uint8_t)mask; }
        else if (a == ((flags_at + 1) & 0xFFFFFu) && flags_at != 0xFFFFFFFFu) { want &= (uint8_t)(mask >> 8); have &= (uint8_t)(mask >> 8); }
        if (want != have) {
            o->result_ok = 0;
            snprintf(o->why, sizeof o->why, "memory %05X = %02X, want %02X", a, have, want);
        }
    }

    /* The instruction's clocks: from its first queue read (QS shows it a
     * clock later) to the next instruction's. */
    unsigned firsts = 0, start = 0, end = ngot;
    for (unsigned i = 0; i < ngot; i++) {
        if (got[i].qop != MX_Q_FIRST) continue;
        if (firsts == 0) start = i;
        if (firsts == prefixes + 1) { end = i; break; }
        firsts++;
    }
    o->start = start;
    o->end = end;
    o->cycles_ok = 1;
    if (end - start != t->ncycles) {
        o->cycles_ok = 0;
        snprintf(o->why + strlen(o->why), sizeof o->why - strlen(o->why), "%s%u clocks, want %u", o->result_ok ? "" : "; ", end - start, t->ncycles);
    } else {
        for (unsigned i = 0; i < t->ncycles && o->cycles_ok; i++) {
            const cyc *w = &t->cycles[i];
            const mx_cpu_cycle *h = &got[start + i];
            unsigned ale = (w->word >> 29) & 1u, ts = (w->word >> 20) & 7u, st = (w->word >> 23) & 7u, seg = (w->word >> 26) & 7u;
            int ok = ale == h->ale && ts == h->tstate && st == h->status && w->qop == h->qop && (w->lines & 63u) == h->lines;
            if (ok && ale && (w->word & 0xFFFFFu) != (h->address & 0xFFFFFu)) ok = 0;
            if (ok && ale && model == MX_CPU_8086 && ((w->lines >> 6) & 1u) != h->bhe) ok = 0;
            if (ok && ts != 0 && ts != 1 && seg != h->segment) ok = 0;
            if (ok && w->qop != 0 && w->qbyte != h->qbyte) ok = 0;
            if (ok && ts == 3 && (w->lines & 0x3Fu)) {
                uint16_t dm = 0xFF;
                if (model == MX_CPU_8086) dm = h->bhe ? 0x00FF : (h->address & 1u) ? 0xFF00 : 0xFFFF;
                /* The flags word an interrupt pushes holds the undefined
                 * flags: compared under the same mask as the flags. */
                if (flags_at != 0xFFFFFFFFu) {
                    uint32_t a = got[start + i - 2].address & 0xFFFFFu;
                    uint16_t fm = (uint16_t)(model == MX_CPU_8086 ? ((a & 1u) ? (mask & 0xFF00u) | (mask >> 8)
                                                                               : mask)
                                                                  : 0xFFFFu);
                    if (model == MX_CPU_8088 && a == flags_at) fm = (uint16_t)(0xFF00u | (mask & 0xFFu));
                    else if (model == MX_CPU_8088 && a == ((flags_at + 1) & 0xFFFFFu)) fm = (uint16_t)(0xFF00u | (mask >> 8));
                    else if (model == MX_CPU_8086 && a == flags_at) fm = (uint16_t)((a & 1u) ? (mask << 8) | 0xFFu : mask);
                    else if (model == MX_CPU_8086 && a == ((flags_at + 1) & 0xFFFFFu)) fm = (uint16_t)((a & 1u) ? (mask & 0xFF00u) | 0xFFu : 0xFF00u | (mask >> 8));
                    else fm = 0xFFFFu;
                    dm &= fm;
                }
                if ((w->data & dm) != (h->data & dm)) ok = 0;
            }
            if (!ok) {
                o->cycles_ok = 0;
                snprintf(o->why + strlen(o->why), sizeof o->why - strlen(o->why), "%sclock %u differs", o->result_ok ? "" : "; ", i);
            }
        }
    }
    /* The queue after the next instruction's first byte was read. */
    o->queue_ok = 0;
    if (end > 0 && end <= ngot) {
        unsigned n = got[end - 1].qlen;
        o->queue_ok = n == t->fin.qlen && !memcmp(got_queue[end - 1], t->fin.queue, n < 8 ? n : 8);
    }

    /* Clean the memory for the next test. */
    for (uint32_t i = 0; i < t->init.nram; i++) memory[t->init.addr[i] & 0xFFFFFu] = 0x90;
    for (uint32_t i = 0; i < t->fin.nram; i++) memory[t->fin.addr[i] & 0xFFFFFu] = 0x90;
    for (unsigned i = 0; i < nwritten; i++) memory[written[i]] = 0x90;
    nwritten = 0;
}

/* --- printing --------------------------------------------------------------------------- */

static const char *const TS[] = {"Ti", "T1", "T2", "T3", "T4", "Tw", "?", "?"};
static const char *const ST[] = {"INTA", "IOR", "IOW", "HALT", "CODE", "MEMR", "MEMW", "PASV"};
static const char *const SG[] = {"ES", "SS", "CS", "DS", "--", "?", "?", "?"};
static const char QO[] = "-FSE";

static void lines_text(unsigned l, char *s) {
    s[0] = (l & 1u) ? 'R' : '-'; s[1] = (l & 2u) ? 'A' : '-'; s[2] = (l & 4u) ? 'W' : '-';
    s[3] = (l & 8u) ? 'R' : '-'; s[4] = (l & 16u) ? 'A' : '-'; s[5] = (l & 32u) ? 'W' : '-'; s[6] = 0;
}

static int brief;
static const char *const ST2[] = {"IA", "IR", "IW", "HA", "CO", "MR", "MW", "PA"};

/* Events as positions: queue reads S, flush E, T1 of each cycle, the end. */
static void events(char *out, size_t cap, unsigned n, int want, const test *t, const outcome *o) {
    size_t len = 0;
    out[0] = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned qop, ts, st;
        if (want) { const cyc *w = &t->cycles[i]; qop = w->qop; ts = (w->word >> 20) & 7u; st = (w->word >> 23) & 7u; }
        else { const mx_cpu_cycle *h = &got[o->start + i]; qop = h->qop; ts = h->tstate; st = h->status; }
        if ((qop == 2 || qop == 3) && len + 8 < cap) len += (size_t)snprintf(out + len, cap - len, "%c%d ", qop == 2 ? 'S' : 'E', (int)i - 1);
        if (ts == 1 && len + 8 < cap) len += (size_t)snprintf(out + len, cap - len, "%s%u ", ST2[st], i);
    }
    if (len + 8 < cap) snprintf(out + len, cap - len, "end%u", n);
}

static void show(const test *t, const outcome *o) {
    printf("  #%u %s: %s\n", t->idx, t->name, o->why);
    if (brief) {
        char a[2048], b[2048];
        events(a, sizeof a, t->ncycles, 1, t, o);
        events(b, sizeof b, o->end - o->start, 0, t, o);
        printf("    q%u want %s\n       got  %s\n", t->init.qlen, a, b);
        return;
    }
    unsigned n = t->ncycles > o->end - o->start ? t->ncycles : o->end - o->start;
    for (unsigned i = 0; i < n && i < 400; i++) {
        char a[64] = "", b[64] = "", l[8];
        if (i < t->ncycles) {
            const cyc *w = &t->cycles[i];
            unsigned ale = (w->word >> 29) & 1u;
            lines_text(w->lines, l);
            snprintf(a, sizeof a, "%s %-4s %s %s %c%02X %05X %04X", TS[(w->word >> 20) & 7u], ST[(w->word >> 23) & 7u],
                     SG[(w->word >> 26) & 7u], l, QO[w->qop & 3u], w->qbyte, ale ? w->word & 0xFFFFFu : 0, w->data);
        }
        if (o->start + i < o->end) {
            const mx_cpu_cycle *h = &got[o->start + i];
            lines_text(h->lines, l);
            snprintf(b, sizeof b, "%s %-4s %s %s %c%02X %05X %04X q%u", TS[h->tstate & 7u], ST[h->status & 7u], SG[h->segment & 7u], l,
                     QO[h->qop & 3u], h->qbyte, h->ale ? h->address : 0, h->data, h->qlen);
        }
        printf("   %3u %-40s | %s\n", i, a, b);
    }
    printf("   final queue want %u bytes, got %u\n", t->fin.qlen, o->end ? got[o->end - 1].qlen : 0);
}

/* --- main ------------------------------------------------------------------------------- */

static uint8_t *load(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = n > 0 ? malloc((size_t)n) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    *size = (size_t)n;
    return p;
}

static const char *NAMES[] = {
    "00", "01", "02", "03", "04", "05", "06", "07", "08", "09", "0A", "0B", "0C", "0D", "0E",
    "10", "11", "12", "13", "14", "15", "16", "17", "18", "19", "1A", "1B", "1C", "1D", "1E", "1F",
    "20", "21", "22", "23", "24", "25", "27", "28", "29", "2A", "2B", "2C", "2D", "2F",
    "30", "31", "32", "33", "34", "35", "37", "38", "39", "3A", "3B", "3C", "3D", "3F",
    "40", "41", "42", "43", "44", "45", "46", "47", "48", "49", "4A", "4B", "4C", "4D", "4E", "4F",
    "50", "51", "52", "53", "54", "55", "56", "57", "58", "59", "5A", "5B", "5C", "5D", "5E", "5F",
    "60", "61", "62", "63", "64", "65", "66", "67", "68", "69", "6A", "6B", "6C", "6D", "6E", "6F",
    "70", "71", "72", "73", "74", "75", "76", "77", "78", "79", "7A", "7B", "7C", "7D", "7E", "7F",
    "80.0", "80.1", "80.2", "80.3", "80.4", "80.5", "80.6", "80.7", "81.0", "81.1", "81.2", "81.3", "81.4", "81.5", "81.6", "81.7",
    "82.0", "82.1", "82.2", "82.3", "82.4", "82.5", "82.6", "82.7", "83.0", "83.1", "83.2", "83.3", "83.4", "83.5", "83.6", "83.7",
    "84", "85", "86", "87", "88", "89", "8A", "8B", "8C", "8D", "8E", "8F",
    "90", "91", "92", "93", "94", "95", "96", "97", "98", "99", "9A", "9C", "9D", "9E", "9F",
    "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7", "A8", "A9", "AA", "AB", "AC", "AD", "AE", "AF",
    "B0", "B1", "B2", "B3", "B4", "B5", "B6", "B7", "B8", "B9", "BA", "BB", "BC", "BD", "BE", "BF",
    "C0", "C1", "C2", "C3", "C4", "C5", "C6", "C7", "C8", "C9", "CA", "CB", "CC", "CD", "CE", "CF",
    "D0.0", "D0.1", "D0.2", "D0.3", "D0.4", "D0.5", "D0.6", "D0.7", "D1.0", "D1.1", "D1.2", "D1.3", "D1.4", "D1.5", "D1.6", "D1.7",
    "D2.0", "D2.1", "D2.2", "D2.3", "D2.4", "D2.5", "D2.6", "D2.7", "D3.0", "D3.1", "D3.2", "D3.3", "D3.4", "D3.5", "D3.6", "D3.7",
    "D4", "D5", "D6", "D7", "D8", "D9", "DA", "DB", "DC", "DD", "DE", "DF",
    "E0", "E1", "E2", "E3", "E4", "E5", "E6", "E7", "E8", "E9", "EA", "EB", "EC", "ED", "EE", "EF",
    "F5", "F6.0", "F6.1", "F6.2", "F6.3", "F6.4", "F6.5", "F6.6", "F6.7", "F7.0", "F7.1", "F7.2", "F7.3", "F7.4", "F7.5", "F7.6", "F7.7",
    "F8", "F9", "FA", "FB", "FC", "FD", "FE.0", "FE.1", "FF.0", "FF.1", "FF.2", "FF.3", "FF.4", "FF.5", "FF.6", "FF.7",
};

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: cpu_suite DIR [--sample N] [--only NAME] [--show K] [--quiet] [--results]\n");
        return 2;
    }
    const char *dir = argv[1], *only = NULL, *grep = NULL;
    unsigned sample = 0, show_max = 0, show_pass = 0;
    long show_idx = -1;
    int quiet = 0, results_only = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--sample") && i + 1 < argc) sample = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--show") && i + 1 < argc) show_max = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "--results")) results_only = 1;
        else if (!strcmp(argv[i], "--brief")) brief = 1;
        else if (!strcmp(argv[i], "--test") && i + 1 < argc) show_idx = atol(argv[++i]);
        else if (!strcmp(argv[i], "--pass") && i + 1 < argc) show_pass = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--grep") && i + 1 < argc) grep = argv[++i];
    }
    /* Bytes the tests do not list read as NOPs (the suites fill the code
     * after the instruction with 90h). */
    memset(memory, 0x90, sizeof memory);
    setvbuf(stdout, NULL, _IOLBF, 1 << 16);
    test *t = malloc(sizeof *t);
    if (!t) return 2;
    unsigned long long total = 0, res_ok = 0, cyc_ok = 0, q_ok = 0;
    unsigned files = 0, res_files = 0;
    int model = -1;
    for (size_t f = 0; f < sizeof NAMES / sizeof NAMES[0]; f++) {
        if (only && strcmp(only, NAMES[f])) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s.bin", dir, NAMES[f]);
        size_t size;
        uint8_t *data = load(path, &size);
        if (!data) { fprintf(stderr, "%s: cannot read\n", path); return 2; }
        reader r = {data, data + size, 0};
        if (size < 12 || memcmp(data, "MXS2", 4)) { fprintf(stderr, "%s: not a converted suite file (python tests/cpu_suite.py fetch)\n", path); return 2; }
        r.p += 4;
        model = (int)u8(&r);
        u8(&r);
        uint16_t mask = (uint16_t)u16(&r);
        uint32_t count = u32(&r);
        unsigned step = sample && sample < count ? count / sample : 1, n = 0, rok = 0, cok = 0, qok = 0, shown_r = 0, shown_c = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (!read_test(&r, t)) { fprintf(stderr, "%s: truncated at test %u\n", path, i); return 2; }
            if (i % step) continue;
            t->idx = i;
            outcome o;
            run_test(t, model, mask, &o);
            n++;
            rok += (unsigned)o.result_ok;
            cok += (unsigned)(o.cycles_ok && o.queue_ok);
            qok += (unsigned)o.queue_ok;
            if (show_idx >= 0) { if ((long)i == show_idx) show(t, &o); continue; }
            if (show_pass && o.result_ok && o.cycles_ok && o.queue_ok && (!grep || strstr(t->name, grep))) { show_pass--; show(t, &o); }
            if (grep && !strstr(t->name, grep)) continue;
            if (!o.result_ok && shown_r < show_max) { shown_r++; show(t, &o); }
            else if (!results_only && o.result_ok && !(o.cycles_ok && o.queue_ok) && shown_c < show_max) { shown_c++; show(t, &o); }
        }
        free(data);
        files++;
        res_files += rok == n;
        total += n; res_ok += rok; cyc_ok += cok; q_ok += qok;
        if (!quiet || rok != n)
            printf("%-5s %6u tests  results %6u  cycles %6u  queue %6u%s\n", NAMES[f], n, rok, cok, qok, rok == n ? "" : "  RESULTS FAIL");
    }
    printf("%s: %u files, %llu tests: results %llu (%.2f%%), cycles+queue %llu (%.2f%%), queue %llu\n",
           model == MX_CPU_8086 ? "8086" : "8088", files, total, res_ok, total ? 100.0 * (double)res_ok / (double)total : 0.0,
           cyc_ok, total ? 100.0 * (double)cyc_ok / (double)total : 0.0, q_ok);
    free(t);
    return res_ok == total ? 0 : 1;
}

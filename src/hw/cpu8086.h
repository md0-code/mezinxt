/* MezinXT - the Intel 8086 and 8088.
 *
 * An interpreter written from Intel's documentation (8086 Family User's
 * Manual, iAPX 86/88 User's Manual): every documented opcode, the
 * undocumented ones the 8088 and 8086 execute (aliases 60h-6Fh, C0h/C1h,
 * C8h/C9h, SALC D6h, POP CS 0Fh, the undefined group encodings), prefixes,
 * interruptible string repeats, divide error, single step, NMI, INTR with an
 * acknowledge cycle, HLT, WAIT and ESC (a hook for an 8087).
 *
 * The bus interface unit is modelled clock by clock: the prefetch queue (4
 * bytes filled a byte at a time on the 8088; 6 bytes filled a word at a time
 * on the 8086), bus cycles T1-T4 with wait states (Tw), idle states (Ti),
 * bus hold between cycles, queue flushes on control transfers. The CPU talks
 * to the board only through mx_cpu_bus; time is counted in CPU clocks (the
 * board's oscillator / 3 on the Junior: the board converts).
 *
 * Portable C11. MezinXT's own code. */
#ifndef MX_CPU8086_H
#define MX_CPU8086_H

#include <stdint.h>

enum { MX_CPU_8088 = 0, MX_CPU_8086 = 1 };

/* Registers, in the encoding order of the instruction set. */
enum {
    MX_AX, MX_CX, MX_DX, MX_BX, MX_SP, MX_BP, MX_SI, MX_DI,
    MX_ES, MX_CS, MX_SS, MX_DS, MX_IP, MX_FLAGS, MX_REG_COUNT
};

/* FLAGS bits. */
enum {
    MX_CF = 0x0001, MX_PF = 0x0004, MX_AF = 0x0010, MX_ZF = 0x0040, MX_SF = 0x0080,
    MX_TF = 0x0100, MX_IF = 0x0200, MX_DF = 0x0400, MX_OF = 0x0800
};

/* A clock as the pins show it (for traces and tests). */
enum { MX_T_I, MX_T_1, MX_T_2, MX_T_3, MX_T_4, MX_T_W };
/* Bus status S2-S0. */
enum { MX_BUS_INTA, MX_BUS_IOR, MX_BUS_IOW, MX_BUS_HALT, MX_BUS_CODE, MX_BUS_MEMR, MX_BUS_MEMW, MX_BUS_PASV };
/* Segment status S4-S3 (MX_SEG_NONE when not driven). */
enum { MX_SEG_ES, MX_SEG_SS, MX_SEG_CS, MX_SEG_DS, MX_SEG_NONE };
/* Queue status QS1-QS0: the queue operation of the previous clock. */
enum { MX_Q_NONE, MX_Q_FIRST, MX_Q_SUBSEQUENT, MX_Q_EMPTY };
/* Command lines an 8288 would decode from the status (active). */
enum { MX_LINE_MRDC = 1, MX_LINE_AMWC = 2, MX_LINE_MWTC = 4, MX_LINE_IORC = 8, MX_LINE_AIOWC = 16, MX_LINE_IOWC = 32 };

typedef struct mx_cpu_cycle {
    uint32_t address;       /* the address latched on T1 (valid with ale) */
    uint16_t data;          /* the data bus on T3/Tw of a transfer (16 bits on the 8086) */
    uint8_t tstate;         /* MX_T_* */
    uint8_t status;         /* MX_BUS_* */
    uint8_t segment;        /* MX_SEG_* */
    uint8_t ale;            /* 1 on T1 */
    uint8_t bhe;            /* BHE pin level on T1 (0: high byte driven; 8086) */
    uint8_t lines;          /* MX_LINE_* */
    uint8_t qop, qbyte;     /* QS1-QS0 (the previous clock's queue operation) and the byte read */
    uint8_t qlen;           /* bytes in the queue at the end of this clock */
    uint8_t lock;           /* LOCK pin asserted */
} mx_cpu_cycle;

/* An ESC instruction (D8h-DFh) as a coprocessor sees it on the bus: the
 * opcode and modrm bytes it decodes from the queue status, the 20-bit
 * address of the instruction (its first prefix) for the coprocessor's
 * instruction pointer, and for a memory operand the 20-bit address and the
 * word of the CPU's read cycle there (the 8087 latches the address from it
 * and runs any further transfers itself). clock is the CPU's clock count. */
typedef struct mx_cpu_esc {
    uint8_t opcode, modrm;
    uint8_t memory;             /* 1: a memory operand (address and value valid) */
    uint16_t value;
    uint32_t address, insn_address;
    uint64_t clock;
} mx_cpu_esc;

/* What the board provides. The CPU calls these during step; mx_cpu_clock()
 * is the clock of the transfer when a callback runs. */
typedef struct mx_cpu_bus {
    void *ctx;
    /* Memory and I/O byte cycles (20-bit address, 16-bit port). */
    uint8_t (*mem_read)(void *ctx, uint32_t address);
    void    (*mem_write)(void *ctx, uint32_t address, uint8_t value);
    uint8_t (*io_read)(void *ctx, uint16_t port);
    void    (*io_write)(void *ctx, uint16_t port, uint8_t value);
    /* Optional, 8086 only: a word cycle at an even address (NULL: two byte
     * calls are made for the one cycle). */
    uint16_t (*mem_read16)(void *ctx, uint32_t address);
    void     (*mem_write16)(void *ctx, uint32_t address, uint16_t value);
    uint16_t (*io_read16)(void *ctx, uint16_t port);
    void     (*io_write16)(void *ctx, uint16_t port, uint16_t value);
    /* Interrupt acknowledge: the vector the interrupt controller puts on
     * the bus in the second INTA cycle. NULL: vector 0xFF. */
    uint8_t (*inta)(void *ctx);
    /* Optional: wait states (clocks of READY low) for the cycle that starts
     * now. kind is MX_BUS_*; write 1 for a write. */
    unsigned (*wait)(void *ctx, unsigned kind, uint32_t address, int write);
    /* Optional: bus hold. Asked when the bus is free and a cycle is about
     * to start: clocks the board holds the bus (HOLD/HLDA, for DMA); 0
     * releases it. The CPU's execution unit keeps running meanwhile. */
    unsigned (*hold)(void *ctx);
    /* Optional: the 8087 (package P7b). Called for every ESC opcode after
     * the CPU's own operand read (mx_cpu_esc). */
    void (*esc)(void *ctx, const mx_cpu_esc *esc);
    /* Optional: the TEST pin for WAIT (1: busy, the CPU waits). NULL: idle. */
    int (*test)(void *ctx);
    /* Optional: one call per clock with the pins (tests and tracing). */
    void (*trace)(void *ctx, const mx_cpu_cycle *cycle);
} mx_cpu_bus;

/* The CPU state. A plain struct so the board can embed it; fields are
 * private to cpu8086.c - use the functions below. */
typedef struct mx_cpu8086 {
    int model;
    const mx_cpu_bus *bus;
    uint16_t r[8], s[4], ip, flags;
    uint64_t clock, t4_end;   /* t4_end: the clock count after the last T4 */
    /* prefetch queue */
    uint8_t q[8];
    unsigned qhead, qlen, qsize;
    uint16_t pc;
    /* bus interface unit */
    uint8_t bt, bop, bseg, bnext, bdelay, bpending, bsusp, bdiscard, bwidth, bhe, blast, bfull, bread3;
    unsigned bwait, bhold;
    uint32_t baddr;
    uint16_t bdata;
    uint8_t lock, lock_next;
    /* the execution unit's request */
    uint8_t rq_active, rq_op, rq_seg, rq_word, rq_phase, rq_done, rq_phases;
    uint16_t rq_base, rq_off, rq_data;
    uint64_t rq_clock;
    /* queue status shown on the next clock */
    uint8_t qs_op, qs_byte, qs_now, qs_now_byte, q_last, q_taken;
    /* decoding */
    uint8_t opcode, modrm, mod, reg, rm, ea_mem;
    int seg_override, rep, prefixes;
    uint16_t ea_off, last_ea, insn_ip;
    int ea_seg;
    /* interrupts */
    uint8_t intr, nmi_line, nmi_pending, inhibit, halted, trap_armed;
    uint64_t step_start;
} mx_cpu8086;

/* A CPU of model MX_CPU_8088 or MX_CPU_8086 on `bus` (kept, not copied),
 * reset. */
void mx_cpu_init(mx_cpu8086 *cpu, int model, const mx_cpu_bus *bus);
/* The RESET pin: FFFF:0000, flags clear, queue empty. */
void mx_cpu_reset(mx_cpu8086 *cpu);
/* Runs one instruction (with its prefixes) and then the interrupt it
 * raises or that is pending; a halted CPU idles a clock. Returns the
 * clocks it took. */
unsigned mx_cpu_step(mx_cpu8086 *cpu);
/* The interrupt lines: INTR (level, acknowledged through bus->inta) and
 * NMI (edge triggered on 0 -> 1). */
void mx_cpu_set_intr(mx_cpu8086 *cpu, int level);
void mx_cpu_set_nmi(mx_cpu8086 *cpu, int level);
/* 1 when halted (HLT) and waiting for an interrupt. */
int mx_cpu_halted(const mx_cpu8086 *cpu);
/* Clocks since init. */
uint64_t mx_cpu_clock(const mx_cpu8086 *cpu);
/* Registers (MX_AX ... MX_FLAGS). Setting CS or IP flushes the queue. */
uint16_t mx_cpu_get(const mx_cpu8086 *cpu, int reg);
void mx_cpu_set(mx_cpu8086 *cpu, int reg, uint16_t value);
/* Tests: replaces the queue with `bytes` (as if prefetched from CS:IP,
 * fetching resumes at IP + n) and lets the bus go idle. */
void mx_cpu_set_queue(mx_cpu8086 *cpu, const uint8_t *bytes, unsigned n);
/* The queue's bytes (at most 6) into out; returns how many. */
unsigned mx_cpu_queue(const mx_cpu8086 *cpu, uint8_t *out);

#endif /* MX_CPU8086_H */

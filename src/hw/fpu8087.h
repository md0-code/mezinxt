/* MezinXT - the Intel 8087 numeric data processor (the
 * catalogue's math processor; package P7b).
 *
 * Written from Intel's 8087 data sheet and the iAPX 86/88 numerics
 * description: the eight-register stack with its tag word, the control word
 * (precision, rounding, projective or affine infinity, exception masks, the
 * interrupt enable mask), the status word (condition codes, TOP, exception
 * flags, interrupt request, busy), 80-bit extended arithmetic, the integer,
 * real and packed BCD formats, the transcendental instructions with the
 * 8087's argument ranges, the control instructions and the real-mode
 * environment of 14 bytes. Arithmetic is integer-only (a 64-bit significand
 * with hand-made 128-bit intermediates), so results never depend on the
 * host's floating point.
 *
 * The 8087 watches the CPU's ESC instructions: the board passes each one
 * (opcode, modrm, the operand's and the instruction's 20-bit addresses) and
 * the 8087 reads and writes its memory operands through the host. It is busy
 * for a typical clock count per instruction (TEST pin for WAIT); its INT
 * output is the interrupt request ANDed with the enable mask.
 *
 * 8087 behaviour, unlike the 287 and 387: projective closure after FINIT;
 * unnormals are operands (they propagate through add, subtract, multiply and
 * as dividend; as divisor, square root, FPREM divisor or transcendental
 * operand they are invalid); a short or long real denormal loads as an
 * unnormal; every NaN operand of an arithmetic instruction is invalid; the
 * 287 and 387 additions (FSTSW AX, FSETPM, FPREM1, FUCOM, FSIN, FCOS,
 * FSINCOS) and the reserved encodings do nothing.
 *
 * Portable C11. MezinXT's own code. */
#ifndef MX_FPU8087_H
#define MX_FPU8087_H

#include <stdint.h>

/* A temporary real: sign and biased exponent (bias 16383) in se, the
 * significand with its explicit integer bit in m. */
typedef struct mx_f80 {
    uint64_t m;
    uint16_t se;
} mx_f80;

/* Status word. TOP is bits 11-13. */
enum {
    MX_FPU_IE = 0x0001, MX_FPU_DE = 0x0002, MX_FPU_ZE = 0x0004, MX_FPU_OE = 0x0008, MX_FPU_UE = 0x0010, MX_FPU_PE = 0x0020,
    MX_FPU_IR = 0x0080, MX_FPU_C0 = 0x0100, MX_FPU_C1 = 0x0200, MX_FPU_C2 = 0x0400, MX_FPU_C3 = 0x4000, MX_FPU_B = 0x8000
};
/* Control word: the masks are bits 0-5 in the order of the flags. */
enum { MX_FPU_IEM = 0x0080, MX_FPU_PC = 0x0300, MX_FPU_RC = 0x0C00, MX_FPU_IC = 0x1000 };

/* What the 8087 needs of the board: its memory cycles (20-bit addresses)
 * and the INT pin. */
typedef struct mx_fpu_host {
    void *ctx;
    uint8_t (*read)(void *ctx, uint32_t address);
    void (*write)(void *ctx, uint32_t address, uint8_t value);
    void (*interrupt)(void *ctx, int level);        /* optional */
} mx_fpu_host;

typedef struct mx_fpu8087 {
    mx_fpu_host host;
    mx_f80 reg[8];              /* the physical registers */
    uint16_t cw;
    uint16_t sw;                /* exception flags and condition codes (TOP, IR and B are kept apart) */
    uint8_t top;
    uint8_t empty;              /* one bit per physical register: tag 11 */
    uint32_t ip, dp;            /* the last non-control instruction's address and its operand's */
    uint16_t opcode;            /* its 11 opcode bits */
    uint64_t now;               /* the clock of the instruction being executed */
    uint64_t busy_until;        /* the clock at which the 8087 becomes idle */
    uint8_t int_line;
} mx_fpu8087;

/* An 8087 on `host` (copied), reset. */
void mx_fpu_init(mx_fpu8087 *f, const mx_fpu_host *host);
/* The RESET pin: as FINIT, idle. */
void mx_fpu_reset(mx_fpu8087 *f);
/* An ESC instruction the CPU decoded at CPU clock `clock`: opcode D8h-DFh,
 * its modrm byte, the 20-bit operand address (memory forms) and the 20-bit
 * address of the instruction (its first prefix). The instruction starts when
 * the 8087 is free (control instructions at once) and keeps it busy for its
 * typical clock count. */
void mx_fpu_esc(mx_fpu8087 *f, uint8_t opcode, uint8_t modrm, uint32_t address, uint32_t insn_address, uint64_t clock);
/* The BUSY output (the CPU's TEST pin): 1 while executing at `clock`. */
int mx_fpu_busy(const mx_fpu8087 *f, uint64_t clock);
/* Executes one instruction at once; returns its typical clock count. */
unsigned mx_fpu_execute(mx_fpu8087 *f, uint8_t opcode, uint8_t modrm, uint32_t address, uint32_t insn_address);
/* The status word as FSTSW would store it at `clock`, and the tag word. */
uint16_t mx_fpu_status_word(const mx_fpu8087 *f, uint64_t clock);
uint16_t mx_fpu_tag_word(const mx_fpu8087 *f);
/* Register ST(i) (tests). */
mx_f80 mx_fpu_st(const mx_fpu8087 *f, unsigned i);
int mx_fpu_st_empty(const mx_fpu8087 *f, unsigned i);

#endif /* MX_FPU8087_H */

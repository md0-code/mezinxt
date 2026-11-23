/* The CPU without downloads: a few hand-written cases from Intel's 8086
 * Family User's Manual run on a flat 1 MiB fake board. The hardware-captured
 * suites are run by cpu_suite (tests/cpu_suite.py). MezinXT's own code. */
#include "cpu8086.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint8_t mem[1u << 20];
static unsigned inta_cycles;
static uint8_t vector = 0x20;

static uint8_t m_rd(void *x, uint32_t a) { (void)x; return mem[a]; }
static void m_wr(void *x, uint32_t a, uint8_t v) { (void)x; mem[a] = v; }
static uint8_t io_rd(void *x, uint16_t p) { (void)x; (void)p; return 0xFF; }
static void io_wr(void *x, uint16_t p, uint8_t v) { (void)x; (void)p; (void)v; }
static uint8_t ack(void *x) { (void)x; inta_cycles++; return vector; }

static mx_cpu_bus bus = {NULL, m_rd, m_wr, io_rd, io_wr, NULL, NULL, NULL, NULL, ack, NULL, NULL, NULL, NULL, NULL};
static mx_cpu8086 cpu;

/* Loads code at 1000:0000 and points CS:IP at it; SS:SP = 2000:0100. */
static void load(int model, const uint8_t *code, size_t n) {
    memset(mem, 0, sizeof mem);
    memcpy(mem + 0x10000, code, n);
    mx_cpu_init(&cpu, model, &bus);
    mx_cpu_set(&cpu, MX_CS, 0x1000);
    mx_cpu_set(&cpu, MX_IP, 0);
    mx_cpu_set(&cpu, MX_SS, 0x2000);
    mx_cpu_set(&cpu, MX_SP, 0x0100);
    mx_cpu_set(&cpu, MX_DS, 0x3000);
    mx_cpu_set(&cpu, MX_ES, 0x4000);
}
static void steps(unsigned n) { while (n--) mx_cpu_step(&cpu); }
static uint16_t R(int r) { return mx_cpu_get(&cpu, r); }

int main(void) {
    for (int model = MX_CPU_8088; model <= MX_CPU_8086; model++) {
        const char *name = model ? "8086" : "8088";
        /* Reset: FFFF:0000, flags clear (manual: "System Reset"). */
        mx_cpu_init(&cpu, model, &bus);
        CHECK(R(MX_CS) == 0xFFFF && R(MX_IP) == 0 && R(MX_FLAGS) == 0xF002);

        /* ADD and the flags (manual, ADD): 7Fh + 1 overflows to 80h. */
        static const uint8_t add[] = {0xB0, 0x7F, 0x04, 0x01, 0xB8, 0xFF, 0xFF, 0x05, 0x01, 0x00};
        load(model, add, sizeof add);
        steps(2);
        CHECK(R(MX_AX) == 0x0080 && (R(MX_FLAGS) & (MX_OF | MX_SF | MX_AF)) == (MX_OF | MX_SF | MX_AF) && !(R(MX_FLAGS) & MX_CF));
        steps(2);
        CHECK(R(MX_AX) == 0 && (R(MX_FLAGS) & (MX_CF | MX_ZF | MX_PF)) == (MX_CF | MX_ZF | MX_PF) && !(R(MX_FLAGS) & MX_OF));

        /* DAA (manual's example): 79h + 35h = AEh, DAA gives 14h with CF. */
        static const uint8_t daa[] = {0xB0, 0x79, 0x04, 0x35, 0x27};
        load(model, daa, sizeof daa);
        steps(3);
        CHECK((R(MX_AX) & 0xFF) == 0x14 && (R(MX_FLAGS) & MX_CF));

        /* MUL, DIV, and DIV by zero raising type 0 with the next IP pushed. */
        static const uint8_t md[] = {0xB8, 0x34, 0x12, 0xBB, 0x10, 0x00, 0xF7, 0xE3, 0xF7, 0xF3, 0x31, 0xDB, 0xF6, 0xF3};
        load(model, md, sizeof md);
        mem[0] = 0x00; mem[1] = 0x05; mem[2] = 0x00; mem[3] = 0x00;   /* INT 0 -> 0000:0500 */
        steps(3);
        CHECK(R(MX_AX) == 0x2340 && R(MX_DX) == 0x0001 && (R(MX_FLAGS) & MX_CF));
        steps(1);
        CHECK(R(MX_AX) == 0x1234 && R(MX_DX) == 0);
        steps(2);
        CHECK(R(MX_CS) == 0 && R(MX_IP) == 0x0500 && R(MX_SP) == 0x00FA);
        steps(1);                    /* the last push completes during the next instruction */
        CHECK(mem[0x200FA] == 0x0E && mem[0x200FB] == 0x00 && mem[0x200FC] == 0x00 && mem[0x200FD] == 0x10);

        /* REP MOVSB copies CX bytes; REPE CMPSB stops at the difference. */
        static const uint8_t movs[] = {0xB9, 0x05, 0x00, 0xBE, 0x00, 0x00, 0xBF, 0x10, 0x00, 0xFC, 0xF3, 0xA4,
                                       0xB9, 0x05, 0x00, 0x31, 0xF6, 0x31, 0xFF, 0xF3, 0xA6};
        load(model, movs, sizeof movs);
        memcpy(mem + 0x30000, "HELLO", 5);
        memcpy(mem + 0x40000, "HELP", 4);
        steps(5);
        CHECK(!memcmp(mem + 0x40010, "HELLO", 5) && R(MX_CX) == 0 && R(MX_SI) == 5 && R(MX_DI) == 0x15);
        steps(4);
        CHECK(R(MX_CX) == 1 && R(MX_SI) == 4 && !(R(MX_FLAGS) & MX_ZF));

        /* INTR: acknowledged with the vector from the INTA cycle, IF and TF
         * cleared, flags/CS/IP pushed; IRET returns. Not taken with IF clear;
         * not between MOV SS and the next instruction. */
        static const uint8_t irq[] = {0xFA, 0x90, 0xFB, 0x90, 0x90, 0x8E, 0xD0, 0x90, 0x90};
        load(model, irq, sizeof irq);
        mem[0x80] = 0x00; mem[0x81] = 0x06; mem[0x82] = 0x00; mem[0x83] = 0x00;   /* INT 20h -> 0000:0600 */
        mem[0x600] = 0xCF;                                                       /* IRET */
        inta_cycles = 0;
        mx_cpu_set_intr(&cpu, 1);
        steps(2);
        CHECK(R(MX_IP) == 2 && inta_cycles == 0);
        steps(1);                    /* STI: one more instruction before INTR */
        CHECK(R(MX_IP) == 3 && R(MX_CS) == 0x1000);
        steps(1);                    /* NOP, then the interrupt */
        CHECK(inta_cycles == 1 && R(MX_CS) == 0 && R(MX_IP) == 0x600 && !(R(MX_FLAGS) & MX_IF));
        mx_cpu_set_intr(&cpu, 0);
        steps(1);
        CHECK(R(MX_CS) == 0x1000 && R(MX_IP) == 4 && (R(MX_FLAGS) & MX_IF));
        steps(1);
        mx_cpu_set_intr(&cpu, 1);
        mx_cpu_set(&cpu, MX_AX, 0x2000);
        steps(1);                    /* MOV SS, AX: no interrupt after it */
        CHECK(R(MX_IP) == 7 && R(MX_CS) == 0x1000 && inta_cycles == 1);
        steps(1);
        CHECK(R(MX_CS) == 0 && inta_cycles == 2);
        mx_cpu_set_intr(&cpu, 0);

        printf("%s: %s\n", name, failures ? "failures" : "OK");
    }
    return failures ? 1 : 0;
}

/* MezinXT - the Intel 8237A DMA controller.
 *
 * Four channels with base and current address and word count (16 bits, one
 * first/last flip-flop for all), the command, mode, request, mask, status
 * and temporary registers, master clear. Modes: demand, single, block and
 * cascade; verify, write (device to memory) and read (memory to device)
 * transfers; auto-initialise; address decrement; fixed and rotating
 * priority; the controller disable; memory-to-memory with channel 0 as the
 * source and channel 1 as the destination (channel 0 address hold
 * included). The compressed-timing bit shortens the cycle by one clock; the
 * extended-write and the DREQ/DACK sense bits are kept in the command
 * register but have no effect (pins are logical, see below).
 *
 * Registers: I/O offsets 0-15 as on the PC/XT (00h-0Fh): 0-7 address and
 * count of channels 0-3, 8 command (write) / status (read; the TC bits are
 * cleared by the read), 9 request, 0Ah single mask, 0Bh mode, 0Ch clear
 * flip-flop, 0Dh master clear (write) / temporary register (read), 0Eh clear
 * mask, 0Fh write all mask bits. Registers that cannot be read return 0xFF.
 * A word access is two byte accesses.
 *
 * Pins (state 1 is the logical assertion; the board handles wire polarity):
 *  - inputs, ops->input: MX_I8237_DREQ(n), n = 0-3.
 *  - outputs, host->line: MX_I8237_HRQ, MX_I8237_EOP (asserted during the
 *    cycle on which a channel reaches terminal count; the board wires it to
 *    the floppy controller's TC), MX_I8237_DACK(n) (asserted during a
 *    channel's cycle; for a cascade channel while its DREQ is asserted and
 *    it has won the bus).
 *
 * Memory side: the 8237 is the bus master, so its transfers go through
 * host->dma (byte-wide, 20-bit address). The address is the page register
 * (page[], 4 bits, the PC/XT's 74LS670 at 80h-83h) in A16-A19 with the
 * channel's 16-bit address below; the 16-bit address wraps inside its 64 KiB
 * page. mx_dma_page_write / mx_dma_page_read take the 74LS670's register
 * (port 80h + index): index 1 channel 2, 2 channel 3, 3 channel 1, 0
 * channel 0.
 *
 * Device side: mx_i8237_attach(chip, channel, dev) names the device whose
 * DACK the channel drives. A transfer cycle calls dev->ops->dack(dev,
 * channel, to_device, value, tc): a memory-read transfer passes the byte
 * read from memory to the device; a memory-write transfer takes the byte
 * the device supplies and writes it to memory; tc is 1 on the cycle that
 * reaches terminal count. A verify cycle calls neither the device nor the
 * memory. A channel with no device attached (the PC/XT's channel 0, the
 * DRAM refresh) touches nothing, so refresh cycles cost no memory access:
 * a memory-read transfer is skipped, a memory-write transfer writes 0xFF.
 * Every attached device may be the board's own trampoline.
 *
 * Time. The chip keeps no deadline: nothing happens between calls. The
 * board lets the CPU's bus-hold callback drive it:
 *
 *   hold(): if (!mx_i8237_hrq(dma)) return 0;
 *           clocks = mx_i8237_bus(dma);      // one DMA cycle, in DMA clocks
 *           return clocks converted to CPU clocks;
 *
 * HRQ (also the line MX_I8237_HRQ) is asserted while a channel wants the
 * bus. mx_i8237_bus runs one transfer cycle of the winning channel and
 * returns how many clocks of the 8237's clock it took (cycle_clocks, 4 by
 * default; the PC/XT's extra wait state makes it 5; compressed timing takes
 * one off). It returns 0 on the call that hands the bus back to the CPU
 * between two transfers of a single-mode channel (and when a block or
 * demand burst ends), so the CPU gets at least one bus cycle between single
 * transfers as on the real chip. Block mode keeps the bus until terminal
 * count whatever DREQ does; demand mode until DREQ drops (a device may drop
 * it from inside its dack callback through ops->input) or terminal count.
 * The refresh channel on the PC/XT (timer 1 -> DREQ0, DACK0 clears the
 * board's DREQ latch) costs one call to input, one to bus and a few line
 * calls per refresh.
 *
 * Portable C11. MezinXT's own code. */
#ifndef MX_I8237_H
#define MX_I8237_H

#include "hw.h"

#define MX_I8237_DREQ(n)  (n)           /* input pins 0-3 */
#define MX_I8237_HRQ      4             /* output pin */
#define MX_I8237_EOP      5             /* output pin */
#define MX_I8237_DACK(n)  (8 + (n))     /* output pins 8-11 */

typedef struct mx_i8237 {
    mx_dev dev;
    uint16_t base_addr[4], base_count[4], cur_addr[4], cur_count[4];
    uint8_t mode[4];
    uint8_t command, status, request, mask, temp, flipflop;
    uint8_t dreq;               /* DREQ pin levels, bits 0-3 */
    uint8_t dack;               /* DACK pins asserted, bits 0-3 */
    uint8_t hrq, eop;           /* output pin levels */
    int8_t owner;               /* the channel holding the bus in block or demand mode, -1 */
    uint8_t cpu_turn;           /* the next bus call gives the bus to the CPU */
    uint8_t prio;               /* the highest-priority channel (rotating priority) */
    uint8_t page[4];            /* A16-A19 of each channel */
    uint8_t cycle_clocks;       /* 8237 clocks per transfer cycle (board sets 5 on the PC/XT) */
    mx_dev *device[4];          /* the device on each channel's DACK, or NULL */
} mx_i8237;

/* Fills in the chip in its master-cleared state; no host call is made. The
 * board then calls dev.ops->reset to bring the lines in step. */
void mx_i8237_init(mx_i8237 *d, const mx_hw_host *host, const char *name);
void mx_i8237_attach(mx_i8237 *d, int channel, mx_dev *device);
/* HRQ: 1 while a channel wants the bus. */
int mx_i8237_hrq(const mx_i8237 *d);
/* One DMA cycle (see above); returns 8237 clocks held, 0 to give the CPU
 * the bus. Call only while HRQ is 1 and the CPU has released the bus. */
unsigned mx_i8237_bus(mx_i8237 *d);

/* The PC/XT's page register file (74LS670): port 80h + index. */
void mx_dma_page_write(mx_i8237 *d, unsigned index, uint8_t value);
uint8_t mx_dma_page_read(const mx_i8237 *d, unsigned index);

#endif /* MX_I8237_H */

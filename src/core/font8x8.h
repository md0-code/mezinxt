/* MezinXT's own fallback 8x8 font, used when the Junior's character ROM is
 * not in roms/: printable ASCII (20h-7Eh). mx_font8x8_build makes a whole
 * 256-glyph table in the character ROM's layout (8 bytes per glyph, bit 7
 * the leftmost pixel): 00h, 20h and FFh blank, DBh a full block, every
 * other code without a glyph a small hollow square, so the gaps show. */
#ifndef MX_FONT8X8_H
#define MX_FONT8X8_H

#include <stdint.h>

extern const uint8_t mx_font8x8_ascii[95][8];

void mx_font8x8_build(uint8_t font[2048]);

#endif /* MX_FONT8X8_H */

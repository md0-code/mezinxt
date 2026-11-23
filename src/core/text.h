/* The window's texts in English and Romanian (text_table.h): the setup
 * screen, its pop-ups and file picker, the messages over the machine's
 * picture, the test screen and the refusals of a disk image. One table of
 * ids; every sentence is a row of its own, so the word order of a language
 * is never built from pieces. Command-line help and the diagnostics on
 * stderr are not here: they stay English.
 *
 * The rows are kept in UTF-8; the texts handed out are one byte for each
 * cell of the Junior's character ROM, which holds no ă ș ț (and â î only as
 * the code page 437 accents, with no capitals), so Romanian is written
 * without diacritics (ă and â fold to a, î to i, ș to s, ț to t, and the
 * capitals likewise), as the Junior's own manual and the software of its
 * day did. A character ROM that
 * holds the whole set is used as it is: the code points of the ten letters
 * go in ROM_LETTERS (text.c), and mx_text_set_font then maps them.
 *
 * The language in use is a setting of the process (the shell sets it from
 * ui.language and the setup screen as the user changes it): mx_t(id) is a
 * text in that language. mx_text(lang, id) is one in a given language, for
 * the checks. MezinXT's own code. */
#ifndef MX_TEXT_H
#define MX_TEXT_H

#include "config.h"

#include <stddef.h>
#include <stdint.h>

enum {
#define X(id, en, ro) MX_T_##id,
#include "text_table.h"
#undef X
    MX_T_COUNT
};

/* English until set; a value that is not a language is English. */
void mx_text_set_language(uint32_t language);
uint32_t mx_text_language(void);

/* The text in a language, ready to draw: a pointer that stays valid until
 * mx_text_set_font is called again. An unknown id gives "". */
const char *mx_text(uint32_t language, int id);
/* The text in the language in use. */
const char *mx_t(int id);
#define MX_TXT(id) mx_t(MX_T_##id)
/* The row as written (UTF-8, with its diacritics). */
const char *mx_text_source(uint32_t language, int id);
/* snprintf of the text of id in the language in use with the given values;
 * the conversions are those of the row. */
int mx_text_format(char *out, size_t capacity, int id, ...);

/* UTF-8 turned into the bytes of the cells: ASCII as it is; the Romanian
 * letters as `letters` has them (ten codes: ă â î ș ț and their capitals,
 * 0 for a letter the font does not hold) or, where it has none, the plain
 * letter; any other character as '?'. letters NULL: no diacritics at all.
 * Returns the length of the result (cut to capacity - 1). */
size_t mx_text_convert(const char *utf8, const uint8_t letters[10], char *out, size_t capacity);

/* Tells the table which character ROM the texts are drawn with: when it
 * holds every Romanian letter at the codes of ROM_LETTERS they are drawn,
 * else all diacritics are dropped. 1 when the letters are used. NULL: the
 * font has none (the fallback font). */
int mx_text_set_font(const uint8_t *font);

#endif /* MX_TEXT_H */

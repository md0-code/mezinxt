/* The text-mode toolkit of the setup screen: an 80x25
 * buffer of cells (a character code and a CGA attribute: ink in the low
 * nibble, paper in the high one, bit 7 a bright paper, no blink) drawn into
 * a 640x200 frame with the Junior's own character ROM, the frame format the
 * machine produces (machine.h), so it passes through the same presenter and
 * monitor as the machine's picture. Box frames, labels, lists and forms with
 * choices, toggles, actions and text fields, a text input line, message
 * boxes and a file picker over an abstract file system. Input is abstracted
 * into events, so the keyboard and a game controller both drive it.
 *
 * No SDL and no OS call here: the host's folders reach the picker through
 * mx_tui_fs, filled in by the setup screen. MezinXT's own code. */
#ifndef MX_TUI_H
#define MX_TUI_H

#include "../core/machine.h"

#include <stddef.h>
#include <stdint.h>

#define MX_TUI_COLS 80
#define MX_TUI_ROWS 25
#define MX_TUI_WIDTH (MX_TUI_COLS * 8)
#define MX_TUI_HEIGHT (MX_TUI_ROWS * 8)

/* The CGA's 16 colours as attribute nibbles. */
enum {
    MX_BLACK, MX_BLUE, MX_GREEN, MX_CYAN, MX_RED, MX_MAGENTA, MX_BROWN, MX_LIGHT_GRAY,
    MX_DARK_GRAY, MX_LIGHT_BLUE, MX_LIGHT_GREEN, MX_LIGHT_CYAN, MX_LIGHT_RED, MX_LIGHT_MAGENTA, MX_YELLOW, MX_WHITE
};
#define MX_ATTR(ink, paper) ((uint8_t)(((paper) << 4) | (ink)))

typedef struct mx_tui_rect { int row, col, rows, cols; } mx_tui_rect;

typedef struct mx_tui_cell { uint8_t ch, attr; } mx_tui_cell;

typedef struct mx_tui {
    mx_tui_cell cell[MX_TUI_ROWS][MX_TUI_COLS];
    int ascii;                          /* the font has no box glyphs: frames and bars in plain ASCII */
    int cursor_row, cursor_col;         /* a blinking underline; row -1: none */
    uint8_t font[2048];                 /* 256 glyphs of 8 rows, bit 7 the leftmost pixel */
    uint8_t pixels[MX_TUI_WIDTH * MX_TUI_HEIGHT];
    uint64_t serial;                    /* changes whenever the pixels change */
    unsigned clipped;                   /* texts cut short since the count was cleared: put off the screen's edge, fitted with "...", wrapped
                                         * beyond their room (a path cut at its start with MX_FIT_START is data, not counted); for the checks */
} mx_tui;

/* --- the font ---------------------------------------------------------------------- */

/* 1 when the font draws the box, block and arrow glyphs the toolkit uses
 * (the code page 437 positions of the IBM character sets, which the
 * Junior's character ROM follows). */
int mx_tui_font_has_boxes(const uint8_t font[2048]);
/* A font of 2048 bytes, NULL for MezinXT's own fallback (ASCII only: frames
 * are then drawn with + - |). The cells are cleared. */
void mx_tui_init(mx_tui *t, const uint8_t *font);

/* --- cells -------------------------------------------------------------------------- */

mx_tui_rect mx_tui_rect_make(int row, int col, int rows, int cols);
/* The rectangle of a given size centred on the screen between rows top and bottom (inclusive). */
mx_tui_rect mx_tui_centered(int rows, int cols, int top, int bottom);
void mx_tui_clear(mx_tui *t, uint8_t attr);
void mx_tui_fill(mx_tui *t, mx_tui_rect r, uint8_t ch, uint8_t attr);
/* Text from (row, col), clipped at the screen's edge; returns the cells written. */
int mx_tui_put(mx_tui *t, int row, int col, uint8_t attr, const char *text);
/* Text in exactly `width` cells (blank padded), cut with "..." where too
 * long: at the end, or with MX_FIT_START at the start (paths keep their
 * tail). MX_FIT_RIGHT and MX_FIT_CENTER align a short text. */
enum { MX_FIT_RIGHT = 1, MX_FIT_CENTER = 2, MX_FIT_START = 4 };
void mx_tui_put_fit(mx_tui *t, int row, int col, int width, uint8_t attr, const char *text, int flags);
/* Text broken into lines of `width` cells at spaces, at most `rows` lines
 * (the rest is cut); each line is blank padded to the width. Returns the lines used. */
int mx_tui_put_wrapped(mx_tui *t, int row, int col, int width, int rows, uint8_t attr, const char *text);

enum { MX_BOX_SINGLE, MX_BOX_DOUBLE };
/* A frame on the edge of r (the inside is not touched), with a title
 * centred on the top edge. */
void mx_tui_box(mx_tui *t, mx_tui_rect r, int style, uint8_t attr, const char *title);
/* The shadow of a window: the two columns right of it and the row below, in dark gray. */
void mx_tui_shadow(mx_tui *t, mx_tui_rect r);
/* A vertical bar in column `col`: up and down arrows, a track and a thumb showing top/visible of total. */
void mx_tui_scrollbar(mx_tui *t, int row, int col, int height, size_t top, size_t visible, size_t total, uint8_t attr);
/* The cell at (row, col), NULL outside the screen. */
const mx_tui_cell *mx_tui_cell_at(const mx_tui *t, int row, int col);

/* The cells drawn into the 640x200 pixels (one byte per pixel, a colour 0-15);
 * the cursor is shown when cursor_on. */
void mx_tui_render(mx_tui *t, int cursor_on);
/* The frame as the machine's: 640x200, shown 4:3. */
void mx_tui_frame(const mx_tui *t, mx_frame *frame);

/* --- input -------------------------------------------------------------------------- */

typedef enum mx_tui_key {
    MX_KEY_NONE, MX_KEY_UP, MX_KEY_DOWN, MX_KEY_LEFT, MX_KEY_RIGHT, MX_KEY_ENTER, MX_KEY_ESCAPE,
    MX_KEY_PAGE_UP, MX_KEY_PAGE_DOWN, MX_KEY_HOME, MX_KEY_END, MX_KEY_TAB, MX_KEY_BACKTAB,
    MX_KEY_BACKSPACE, MX_KEY_DELETE, MX_KEY_CHAR
} mx_tui_key;

typedef struct mx_tui_event {
    mx_tui_key key;
    int ch;                             /* MX_KEY_CHAR: the character, 0x20-0x7E */
} mx_tui_event;

/* The event an XT key (set 1 make code) makes with or without shift, in the
 * US layout the XT keyboard prints; 0 when the key makes none. The keypad's
 * keys are the cursor keys, as on the 83-key keyboard without Num Lock. */
int mx_tui_xt_event(uint8_t make_code, int shift, mx_tui_event *event);

/* PREV and NEXT: the value of a CHOICE or TOGGLE row of a form goes back or on. */
typedef enum mx_tui_result { MX_TUI_NONE, MX_TUI_MOVED, MX_TUI_ACTIVATE, MX_TUI_PREV, MX_TUI_NEXT, MX_TUI_CANCEL } mx_tui_result;

/* --- colours -------------------------------------------------------------------------- */

typedef struct mx_tui_theme {
    uint8_t normal, selected, disabled, heading, value, action, frame, title, scroll;
} mx_tui_theme;
/* The setup screen's window (white on blue) and its pop-ups (black on gray). */
const mx_tui_theme *mx_tui_window_theme(void);
const mx_tui_theme *mx_tui_popup_theme(void);

/* --- lists and forms ------------------------------------------------------------------- */

/* One row of a list. TEXT: a plain entry (its value, when any, at the
 * right edge); HEADING and INFO are never selected; CHOICE is changed with
 * Page Up and Page Down in a form (and Enter); TOGGLE flips; ACTION runs;
 * FIELD is a text value edited on Enter. A disabled row is drawn dim and
 * skipped. */
enum { MX_ITEM_TEXT, MX_ITEM_HEADING, MX_ITEM_INFO, MX_ITEM_CHOICE, MX_ITEM_TOGGLE, MX_ITEM_ACTION, MX_ITEM_FIELD };
typedef struct mx_tui_item {
    int kind;
    int enabled;
    int on;                             /* TOGGLE */
    const char *label;
    const char *value;                  /* CHOICE, INFO, FIELD, TEXT (may be NULL) */
} mx_tui_item;

typedef struct mx_tui_list {
    const mx_tui_item *items;           /* not owned */
    size_t count;
    size_t selected, top;
    int rows;                           /* rows shown */
    int value_col;                      /* where values start, from the list's left edge */
    int form;                           /* the page keys change the selected row's value instead of paging (the setup pages; a pop-up's list pages) */
} mx_tui_list;

int mx_tui_item_selectable(const mx_tui_item *item);
/* Sets the rows; the selection stays when it still can be selected, else
 * moves to the nearest row that can. */
void mx_tui_list_set(mx_tui_list *list, const mx_tui_item *items, size_t count, int rows, int value_col);
void mx_tui_list_select(mx_tui_list *list, size_t index);
/* Up, down, home, end move, and so do Page Up and Page Down, a page at a
 * time; in a form (list->form) Page Up and Page Down on a CHOICE or TOGGLE
 * give MX_TUI_PREV or NEXT instead (left and right are the form's own: the
 * page of the screen); Enter gives ACTIVATE (a selected row only); Escape
 * CANCEL; a character jumps to the next row whose label starts with it. */
mx_tui_result mx_tui_list_event(mx_tui_list *list, mx_tui_event event);
void mx_tui_list_draw(mx_tui *t, mx_tui_list *list, mx_tui_rect area, const mx_tui_theme *theme);

/* --- the text input line ------------------------------------------------------------------ */

#define MX_TUI_INPUT_MAX 1024
typedef struct mx_tui_input {
    char text[MX_TUI_INPUT_MAX];
    size_t length, cursor, scroll, max;
    int fresh;                          /* the text as it was offered: shown highlighted, the first character typed replaces it */
} mx_tui_input;
void mx_tui_input_set(mx_tui_input *in, const char *text, size_t max);
/* Characters, backspace, delete, left, right, home, end edit (MOVED); while
 * the text is still as offered, a character replaces all of it and
 * backspace or delete clear it, as the prompts of the DOS utilities did.
 * Enter gives ACTIVATE, Escape CANCEL. */
mx_tui_result mx_tui_input_event(mx_tui_input *in, mx_tui_event event);
/* The line in `width` cells at (row, col), with the cursor. */
void mx_tui_input_draw(mx_tui *t, mx_tui_input *in, int row, int col, int width, uint8_t attr);

/* --- message and confirm boxes ------------------------------------------------------------- */

typedef struct mx_tui_dialog {
    char title[48], text[512];
    char button[3][16];
    int buttons, selected;
    int cancel;                         /* the button Escape picks, -1 none */
    int result;                         /* the button picked, after ACTIVATE */
} mx_tui_dialog;
/* buttons: names separated by '|' ("Restart|Back"); at most three. */
void mx_tui_dialog_set(mx_tui_dialog *d, const char *title, const char *text, const char *buttons, int selected, int cancel);
mx_tui_result mx_tui_dialog_event(mx_tui_dialog *d, mx_tui_event event);
/* Centred between rows top and bottom, with its shadow. */
void mx_tui_dialog_draw(mx_tui *t, const mx_tui_dialog *d, int top, int bottom);

/* --- the file picker -------------------------------------------------------------------------- */

typedef int (*mx_tui_entry_fn)(void *ctx, const char *name, int is_folder, uint64_t size);
/* The file system as the picker sees it. Paths are opaque strings: a root
 * ("C:\" or "/") keeps its separator, other folders have none at the end. */
typedef struct mx_tui_fs {
    void *ctx;
    /* The entries of a folder; 0 when it cannot be read. */
    int (*list)(void *ctx, const char *folder, mx_tui_entry_fn fn, void *fn_ctx);
    /* The roots (drives): each as a folder name. */
    int (*roots)(void *ctx, mx_tui_entry_fn fn, void *fn_ctx);
    /* The folder above; 0 at a root (the picker then shows the roots). */
    int (*parent)(void *ctx, const char *folder, char *out, size_t capacity);
    /* The path of a name inside a folder. */
    void (*join)(void *ctx, const char *folder, const char *name, char *out, size_t capacity);
} mx_tui_fs;

#define MX_TUI_PATH 1024
typedef struct mx_tui_picker_entry {
    char *name;
    int is_folder;
    uint64_t size;
    char size_text[16];
} mx_tui_picker_entry;

typedef struct mx_tui_picker {
    mx_tui_fs fs;
    char title[64];
    char filter[64];                    /* extensions with their dot, lower case, ';' apart: ".img;.ima"; empty: every file */
    char folder[MX_TUI_PATH];           /* "" while the roots are shown */
    char picked[MX_TUI_PATH];           /* the file, after ACTIVATE */
    char error[64];
    mx_tui_picker_entry *entries;
    mx_tui_item *items;
    size_t count, capacity;
    mx_tui_list list;
} mx_tui_picker;
/* Lists `folder` (the roots when it cannot be read and has no parent);
 * 0 when out of memory. */
int mx_tui_picker_open(mx_tui_picker *p, const mx_tui_fs *fs, const char *title, const char *folder, const char *filter);
void mx_tui_picker_close(mx_tui_picker *p);
/* Enter on a folder opens it, on a file picks it (ACTIVATE, p->picked);
 * Backspace goes up; Escape CANCEL. */
mx_tui_result mx_tui_picker_event(mx_tui_picker *p, mx_tui_event event);
void mx_tui_picker_draw(mx_tui *t, mx_tui_picker *p, int top, int bottom);

#endif /* MX_TUI_H */

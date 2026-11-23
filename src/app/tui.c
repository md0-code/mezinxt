/* The text-mode toolkit (tui.h). MezinXT's own code: the cell buffer, the
 * widgets and the glyph positions of the code page 437 box set, which the
 * Junior's character ROM follows (checked against the ROM by mx_tui_font_has_boxes). */
#include "tui.h"
#include "../core/font8x8.h"
#include "../core/text.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- glyphs ------------------------------------------------------------------------------ */

typedef struct box_chars { uint8_t tl, tr, bl, br, h, v; } box_chars;
static const box_chars BOX_SINGLE = {0xDA, 0xBF, 0xC0, 0xD9, 0xC4, 0xB3};
static const box_chars BOX_DOUBLE = {0xC9, 0xBB, 0xC8, 0xBC, 0xCD, 0xBA};
static const box_chars BOX_PLAIN = {'+', '+', '+', '+', '-', '|'};
static const box_chars BOX_PLAIN_DOUBLE = {'+', '+', '+', '+', '=', '|'};

#define GLYPH_BLOCK 0xDB
#define GLYPH_SHADE 0xB0
#define GLYPH_UP 0x18
#define GLYPH_DOWN 0x19

int mx_tui_font_has_boxes(const uint8_t font[2048]) {
    static const uint8_t needed[] = {0xC9, 0xBB, 0xC8, 0xBC, 0xDA, 0xBF, 0xC0, 0xD9, 0xC7, 0xB6, 0xB0, 0x18, 0x19};
    for (size_t i = 0; i < sizeof needed; ++i) {
        unsigned any = 0;
        for (int y = 0; y < 8; ++y) any |= font[needed[i] * 8 + y];
        if (!any) return 0;
    }
    const uint8_t *single_h = font + 0xC4 * 8, *double_h = font + 0xCD * 8, *single_v = font + 0xB3 * 8, *double_v = font + 0xBA * 8;
    int full_single = 0, full_double = 0;
    for (int y = 0; y < 8; ++y) {
        full_single += single_h[y] == 0xFF;
        full_double += double_h[y] == 0xFF;
        if (!single_v[y] || !double_v[y] || font[0xDB * 8 + y] != 0xFF) return 0;
    }
    return full_single >= 1 && full_double >= 2;
}

void mx_tui_init(mx_tui *t, const uint8_t *font) {
    memset(t, 0, sizeof *t);
    if (font) memcpy(t->font, font, sizeof t->font);
    else mx_font8x8_build(t->font);
    t->ascii = !font || !mx_tui_font_has_boxes(t->font);
    t->cursor_row = -1;
    mx_tui_clear(t, MX_ATTR(MX_LIGHT_GRAY, MX_BLACK));
}

/* --- cells ------------------------------------------------------------------------------- */

mx_tui_rect mx_tui_rect_make(int row, int col, int rows, int cols) {
    mx_tui_rect r = {row, col, rows, cols};
    return r;
}

mx_tui_rect mx_tui_centered(int rows, int cols, int top, int bottom) {
    mx_tui_rect r;
    r.rows = rows;
    r.cols = cols;
    r.row = top + (bottom - top + 1 - rows) / 2;
    r.col = (MX_TUI_COLS - cols) / 2;
    if (r.row < 0) r.row = 0;
    if (r.col < 0) r.col = 0;
    return r;
}

static mx_tui_cell *cell(mx_tui *t, int row, int col) {
    return row >= 0 && row < MX_TUI_ROWS && col >= 0 && col < MX_TUI_COLS ? &t->cell[row][col] : NULL;
}
const mx_tui_cell *mx_tui_cell_at(const mx_tui *t, int row, int col) {
    return row >= 0 && row < MX_TUI_ROWS && col >= 0 && col < MX_TUI_COLS ? &t->cell[row][col] : NULL;
}

void mx_tui_clear(mx_tui *t, uint8_t attr) {
    for (int r = 0; r < MX_TUI_ROWS; ++r)
        for (int c = 0; c < MX_TUI_COLS; ++c) {
            t->cell[r][c].ch = ' ';
            t->cell[r][c].attr = attr;
        }
}

void mx_tui_fill(mx_tui *t, mx_tui_rect r, uint8_t ch, uint8_t attr) {
    for (int y = 0; y < r.rows; ++y)
        for (int x = 0; x < r.cols; ++x) {
            mx_tui_cell *c = cell(t, r.row + y, r.col + x);
            if (c) { c->ch = ch; c->attr = attr; }
        }
}

int mx_tui_put(mx_tui *t, int row, int col, uint8_t attr, const char *text) {
    int n = 0;
    for (; *text; ++text, ++col) {
        mx_tui_cell *c = cell(t, row, col);
        if (!c) { ++t->clipped; continue; }
        c->ch = (uint8_t)*text;
        c->attr = attr;
        ++n;
    }
    return n;
}

void mx_tui_put_fit(mx_tui *t, int row, int col, int width, uint8_t attr, const char *text, int flags) {
    if (width <= 0) return;
    char shown[MX_TUI_PATH + 8];
    size_t length = strlen(text);
    if (length > (size_t)width) {
        if (!(flags & MX_FIT_START)) ++t->clipped;
        if (width <= 3) { memset(shown, '.', (size_t)width); shown[width] = 0; }
        else if (flags & MX_FIT_START) snprintf(shown, sizeof shown, "...%s", text + length - (size_t)(width - 3));
        else snprintf(shown, sizeof shown, "%.*s...", width - 3, text);
        length = strlen(shown);
    } else snprintf(shown, sizeof shown, "%s", text);
    mx_tui_fill(t, mx_tui_rect_make(row, col, 1, width), ' ', attr);
    int offset = 0;
    if (flags & MX_FIT_RIGHT) offset = width - (int)length;
    else if (flags & MX_FIT_CENTER) offset = (width - (int)length) / 2;
    mx_tui_put(t, row, col + offset, attr, shown);
}

void mx_tui_box(mx_tui *t, mx_tui_rect r, int style, uint8_t attr, const char *title) {
    if (r.rows < 2 || r.cols < 2) return;
    const box_chars *b = t->ascii ? (style == MX_BOX_DOUBLE ? &BOX_PLAIN_DOUBLE : &BOX_PLAIN) : (style == MX_BOX_DOUBLE ? &BOX_DOUBLE : &BOX_SINGLE);
    const int bottom = r.row + r.rows - 1, right = r.col + r.cols - 1;
    for (int x = r.col + 1; x < right; ++x) {
        mx_tui_cell *top = cell(t, r.row, x), *low = cell(t, bottom, x);
        if (top) { top->ch = b->h; top->attr = attr; }
        if (low) { low->ch = b->h; low->attr = attr; }
    }
    for (int y = r.row + 1; y < bottom; ++y) {
        mx_tui_cell *l = cell(t, y, r.col), *rr = cell(t, y, right);
        if (l) { l->ch = b->v; l->attr = attr; }
        if (rr) { rr->ch = b->v; rr->attr = attr; }
    }
    const struct { int row, col; uint8_t ch; } corners[4] = {{r.row, r.col, b->tl}, {r.row, right, b->tr}, {bottom, r.col, b->bl}, {bottom, right, b->br}};
    for (int i = 0; i < 4; ++i) {
        mx_tui_cell *c = cell(t, corners[i].row, corners[i].col);
        if (c) { c->ch = corners[i].ch; c->attr = attr; }
    }
    if (title && *title) {
        char text[96];
        const int room = r.cols - 6;
        if ((int)strlen(title) > room) ++t->clipped;
        if (room > 0) {
            snprintf(text, sizeof text, " %.*s ", room, title);
            const int n = (int)strlen(text);
            mx_tui_put(t, r.row, r.col + (r.cols - n) / 2, attr, text);
        }
    }
}

void mx_tui_shadow(mx_tui *t, mx_tui_rect r) {
    const uint8_t dark = MX_ATTR(MX_DARK_GRAY, MX_BLACK);
    for (int y = 1; y <= r.rows; ++y)
        for (int x = 0; x < 2; ++x) {
            mx_tui_cell *c = cell(t, r.row + y, r.col + r.cols + x);
            if (c) c->attr = dark;
        }
    for (int x = 2; x < r.cols + 2; ++x) {
        mx_tui_cell *c = cell(t, r.row + r.rows, r.col + x);
        if (c) c->attr = dark;
    }
}

void mx_tui_scrollbar(mx_tui *t, int row, int col, int height, size_t top, size_t visible, size_t total, uint8_t attr) {
    if (height < 3 || total <= visible) return;
    const int track = height - 2;
    mx_tui_cell *c = cell(t, row, col);
    if (c) { c->ch = t->ascii ? '^' : GLYPH_UP; c->attr = attr; }
    c = cell(t, row + height - 1, col);
    if (c) { c->ch = t->ascii ? 'v' : GLYPH_DOWN; c->attr = attr; }
    const size_t range = total - visible;
    int thumb = (int)((size_t)(track - 1) * (top > range ? range : top) / range);
    for (int i = 0; i < track; ++i) {
        c = cell(t, row + 1 + i, col);
        if (c) { c->ch = i == thumb ? (t->ascii ? '#' : GLYPH_BLOCK) : (t->ascii ? ':' : GLYPH_SHADE); c->attr = attr; }
    }
}

/* --- the picture ------------------------------------------------------------------------- */

void mx_tui_render(mx_tui *t, int cursor_on) {
    for (int r = 0; r < MX_TUI_ROWS; ++r)
        for (int c = 0; c < MX_TUI_COLS; ++c) {
            const uint8_t ch = t->cell[r][c].ch, attr = t->cell[r][c].attr;
            const uint8_t ink = attr & 15u, paper = (uint8_t)(attr >> 4);
            for (int y = 0; y < 8; ++y) {
                const uint8_t bits = t->font[ch * 8 + y];
                uint8_t *p = t->pixels + (size_t)(r * 8 + y) * MX_TUI_WIDTH + (size_t)c * 8;
                for (int x = 0; x < 8; ++x) p[x] = bits & (0x80u >> x) ? ink : paper;
            }
        }
    if (cursor_on && t->cursor_row >= 0 && t->cursor_row < MX_TUI_ROWS && t->cursor_col >= 0 && t->cursor_col < MX_TUI_COLS) {
        const uint8_t ink = t->cell[t->cursor_row][t->cursor_col].attr & 15u;
        for (int y = 6; y < 8; ++y)
            memset(t->pixels + (size_t)(t->cursor_row * 8 + y) * MX_TUI_WIDTH + (size_t)t->cursor_col * 8, ink, 8);
    }
    ++t->serial;
}

void mx_tui_frame(const mx_tui *t, mx_frame *f) {
    f->width = MX_TUI_WIDTH;
    f->height = MX_TUI_HEIGHT;
    f->pitch = MX_TUI_WIDTH;
    f->pixels = t->pixels;
    f->aspect_w = 4;
    f->aspect_h = 3;
    f->serial = t->serial;
    f->format = MX_FRAME_RGBI;
}

/* --- input ------------------------------------------------------------------------------- */

/* Set 1 make codes 02h-39h as the US layout prints them (IBM PC/XT
 * Technical Reference), unshifted and shifted; 0 for keys that type nothing. */
static const char PLAIN[0x3A] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '};
static const char SHIFTED[0x3A] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '};

int mx_tui_xt_event(uint8_t code, int shift, mx_tui_event *e) {
    e->key = MX_KEY_NONE;
    e->ch = 0;
    switch (code) {
    case 0x01: e->key = MX_KEY_ESCAPE; return 1;
    case 0x0E: e->key = MX_KEY_BACKSPACE; return 1;
    case 0x0F: e->key = shift ? MX_KEY_BACKTAB : MX_KEY_TAB; return 1;
    case 0x1C: e->key = MX_KEY_ENTER; return 1;
    case 0x47: e->key = MX_KEY_HOME; return 1;
    case 0x48: e->key = MX_KEY_UP; return 1;
    case 0x49: e->key = MX_KEY_PAGE_UP; return 1;
    case 0x4B: e->key = MX_KEY_LEFT; return 1;
    case 0x4D: e->key = MX_KEY_RIGHT; return 1;
    case 0x4F: e->key = MX_KEY_END; return 1;
    case 0x50: e->key = MX_KEY_DOWN; return 1;
    case 0x51: e->key = MX_KEY_PAGE_DOWN; return 1;
    case 0x53: e->key = MX_KEY_DELETE; return 1;
    default: break;
    }
    if (code < 0x3A) {
        const char ch = shift ? SHIFTED[code] : PLAIN[code];
        if (ch) { e->key = MX_KEY_CHAR; e->ch = ch; return 1; }
    }
    return 0;
}

/* --- colours ----------------------------------------------------------------------------- */

static const mx_tui_theme WINDOW = {
    MX_ATTR(MX_LIGHT_GRAY, MX_BLUE),    /* normal */
    MX_ATTR(MX_BLACK, MX_CYAN),         /* selected */
    MX_ATTR(MX_DARK_GRAY, MX_BLUE),     /* disabled */
    MX_ATTR(MX_YELLOW, MX_BLUE),        /* heading */
    MX_ATTR(MX_WHITE, MX_BLUE),         /* value */
    MX_ATTR(MX_LIGHT_CYAN, MX_BLUE),    /* action */
    MX_ATTR(MX_LIGHT_CYAN, MX_BLUE),    /* frame */
    MX_ATTR(MX_WHITE, MX_BLUE),         /* title */
    MX_ATTR(MX_CYAN, MX_BLUE),          /* scroll */
};
static const mx_tui_theme POPUP = {
    MX_ATTR(MX_BLACK, MX_LIGHT_GRAY),
    MX_ATTR(MX_WHITE, MX_BLACK),
    MX_ATTR(MX_DARK_GRAY, MX_LIGHT_GRAY),
    MX_ATTR(MX_BLUE, MX_LIGHT_GRAY),
    MX_ATTR(MX_BLACK, MX_LIGHT_GRAY),
    MX_ATTR(MX_BLUE, MX_LIGHT_GRAY),
    MX_ATTR(MX_WHITE, MX_LIGHT_GRAY),
    MX_ATTR(MX_BLACK, MX_LIGHT_GRAY),
    MX_ATTR(MX_BLACK, MX_LIGHT_GRAY),
};
const mx_tui_theme *mx_tui_window_theme(void) { return &WINDOW; }
const mx_tui_theme *mx_tui_popup_theme(void) { return &POPUP; }

/* --- lists and forms ----------------------------------------------------------------------- */

int mx_tui_item_selectable(const mx_tui_item *item) {
    return item->enabled && item->kind != MX_ITEM_HEADING && item->kind != MX_ITEM_INFO;
}

static int selectable_at(const mx_tui_list *l, size_t i) { return i < l->count && mx_tui_item_selectable(&l->items[i]); }

/* Whether any row but `index` can be selected before it (direction -1) or after it (1). */
static int selectable_beyond(const mx_tui_list *l, size_t index, int direction) {
    if (direction < 0) { for (size_t i = 0; i < index; ++i) if (selectable_at(l, i)) return 1; }
    else for (size_t i = index + 1; i < l->count; ++i) if (selectable_at(l, i)) return 1;
    return 0;
}

/* Moves the window so that the selected row is in it. Rows that cannot be selected (headings, notes, dimmed rows) are shown only
 * by scrolling, so they come with the selection next to them: the ones right above a selected row at the top of the window (its
 * section's heading), and, when no row before the selected one can be selected, all of the page above it (the page's top); the same
 * below the last row that can be selected (the page's end). */
static void scroll_into_view(mx_tui_list *l) {
    if (l->rows <= 0) return;
    const size_t rows = (size_t)l->rows;
    if (l->count <= rows) { l->top = 0; return; }
    if (l->selected < l->top) l->top = l->selected;
    if (l->selected >= l->top + rows) l->top = l->selected - rows + 1;
    if (l->top > l->count - rows) l->top = l->count - rows;
    if (!selectable_at(l, l->selected)) return;
    if (!selectable_beyond(l, l->selected, -1)) {
        l->top = l->selected < rows ? 0 : l->selected - rows + 1;
        return;
    }
    if (!selectable_beyond(l, l->selected, 1) && l->top + rows < l->count)
        l->top = l->selected < l->count - rows ? l->selected : l->count - rows;
    if (l->top == l->selected) {
        size_t above = 0;
        while (above + 1 < rows && above < l->top && !selectable_at(l, l->top - above - 1)) ++above;
        l->top -= above;
    }
}

void mx_tui_list_set(mx_tui_list *l, const mx_tui_item *items, size_t count, int rows, int value_col) {
    l->items = items;
    l->count = count;
    l->rows = rows;
    l->value_col = value_col;
    if (l->selected >= count) l->selected = count ? count - 1 : 0;
    if (!selectable_at(l, l->selected)) {
        size_t down = l->selected, up = l->selected;
        int found = 0;
        for (size_t i = l->selected; i < count && !found; ++i) if (selectable_at(l, i)) { down = i; found = 1; }
        if (!found) {
            for (size_t i = l->selected; i-- > 0;) if (selectable_at(l, i)) { up = i; found = 1; break; }
            if (found) down = up;
        }
        if (found) l->selected = down;
    }
    scroll_into_view(l);
}

void mx_tui_list_select(mx_tui_list *l, size_t index) {
    if (selectable_at(l, index)) l->selected = index;
    scroll_into_view(l);
}

static size_t step(const mx_tui_list *l, size_t from, int direction) {
    /* The next selectable row in a direction, wrapping round; `from` when there is none. */
    for (size_t n = 1; n <= l->count; ++n) {
        const size_t i = direction > 0 ? (from + n) % l->count : (from + l->count - n % l->count) % l->count;
        if (selectable_at(l, i)) return i;
    }
    return from;
}

mx_tui_result mx_tui_list_event(mx_tui_list *l, mx_tui_event e) {
    if (!l->count) return e.key == MX_KEY_ESCAPE ? MX_TUI_CANCEL : MX_TUI_NONE;
    const size_t before = l->selected;
    const mx_tui_item *current = selectable_at(l, l->selected) ? &l->items[l->selected] : NULL;
    switch (e.key) {
    case MX_KEY_UP: l->selected = step(l, l->selected, -1); break;
    case MX_KEY_DOWN: l->selected = step(l, l->selected, 1); break;
    case MX_KEY_HOME:
        for (size_t i = 0; i < l->count; ++i) if (selectable_at(l, i)) { l->selected = i; break; }
        break;
    case MX_KEY_END:
        for (size_t i = l->count; i-- > 0;) if (selectable_at(l, i)) { l->selected = i; break; }
        break;
    case MX_KEY_PAGE_UP:
    case MX_KEY_PAGE_DOWN: {
        if (l->form) {
            if (current && (current->kind == MX_ITEM_CHOICE || current->kind == MX_ITEM_TOGGLE)) return e.key == MX_KEY_PAGE_UP ? MX_TUI_PREV : MX_TUI_NEXT;
            return MX_TUI_NONE;
        }
        const size_t jump = l->rows > 1 ? (size_t)l->rows - 1 : 1;
        const int up = e.key == MX_KEY_PAGE_UP;
        size_t target = up ? (l->selected > jump ? l->selected - jump : 0) : (l->selected + jump < l->count ? l->selected + jump : l->count - 1);
        /* a row that cannot be selected gives way to the nearest one back towards where we came from */
        while (target != l->selected && !selectable_at(l, target)) target = up ? target + 1 : target - 1;
        if (selectable_at(l, target)) l->selected = target;
        break;
    }
    case MX_KEY_ENTER: return current ? MX_TUI_ACTIVATE : MX_TUI_NONE;
    case MX_KEY_ESCAPE: return MX_TUI_CANCEL;
    case MX_KEY_CHAR:
        if (e.ch == ' ' && current && current->kind == MX_ITEM_TOGGLE) return MX_TUI_ACTIVATE;
        if (e.ch > ' ') {
            const int want = e.ch >= 'A' && e.ch <= 'Z' ? e.ch - 'A' + 'a' : e.ch;
            for (size_t n = 1; n <= l->count; ++n) {
                const size_t i = (l->selected + n) % l->count;
                const char first = selectable_at(l, i) && l->items[i].label ? l->items[i].label[0] : 0;
                const int lower = first >= 'A' && first <= 'Z' ? first - 'A' + 'a' : first;
                if (lower && lower == want) { l->selected = i; break; }
            }
        }
        break;
    default: return MX_TUI_NONE;
    }
    scroll_into_view(l);
    return l->selected != before ? MX_TUI_MOVED : MX_TUI_NONE;
}

void mx_tui_list_draw(mx_tui *t, mx_tui_list *l, mx_tui_rect area, const mx_tui_theme *theme) {
    l->rows = area.rows;
    scroll_into_view(l);
    const int scrolled = l->count > (size_t)area.rows;
    const int width = area.cols - (scrolled ? 1 : 0);
    const int value_col = l->value_col > 0 ? l->value_col : width / 2;
    for (int i = 0; i < area.rows; ++i) {
        const int row = area.row + i;
        const size_t index = l->top + (size_t)i;
        mx_tui_fill(t, mx_tui_rect_make(row, area.col, 1, width), ' ', theme->normal);
        if (index >= l->count) continue;
        const mx_tui_item *it = &l->items[index];
        const int selected = index == l->selected && mx_tui_item_selectable(it);
        uint8_t attr = !it->enabled ? theme->disabled : it->kind == MX_ITEM_HEADING ? theme->heading : it->kind == MX_ITEM_ACTION ? theme->action : theme->normal;
        uint8_t vattr = !it->enabled ? theme->disabled : theme->value;
        if (selected) {
            attr = vattr = theme->selected;
            mx_tui_fill(t, mx_tui_rect_make(row, area.col, 1, width), ' ', theme->selected);
        }
        const char *label = it->label ? it->label : "";
        if (it->kind == MX_ITEM_HEADING) { mx_tui_put_fit(t, row, area.col, width, attr, label, 0); continue; }
        if (it->kind == MX_ITEM_TEXT) {
            const int value_width = it->value ? (int)strlen(it->value) : 0;
            const int label_width = width - 2 - (value_width ? value_width + 1 : 0);
            mx_tui_put_fit(t, row, area.col + 1, label_width, attr, label, MX_FIT_START);
            if (value_width) mx_tui_put_fit(t, row, area.col + width - 1 - value_width, value_width, selected ? attr : vattr, it->value, 0);
            continue;
        }
        const int label_width = it->kind == MX_ITEM_ACTION ? width - 2 : value_col - 2;
        mx_tui_put_fit(t, row, area.col + 1, label_width, attr, label, 0);
        const int room = width - value_col - 1;
        if (it->kind == MX_ITEM_ACTION || room <= 0) continue;
        char text[MX_TUI_PATH + 16];
        const char *value = it->value ? it->value : "";
        if (it->kind == MX_ITEM_CHOICE) snprintf(text, sizeof text, it->enabled ? "< %s >" : "%s", value);
        else if (it->kind == MX_ITEM_TOGGLE) snprintf(text, sizeof text, "[%c]%s%s", it->on ? 'X' : ' ', *value ? " " : "", value);
        else snprintf(text, sizeof text, "%s", value);
        mx_tui_put_fit(t, row, area.col + value_col, room, vattr, text, it->kind == MX_ITEM_FIELD || it->kind == MX_ITEM_INFO ? MX_FIT_START : 0);
    }
    if (scrolled) mx_tui_scrollbar(t, area.row, area.col + area.cols - 1, area.rows, l->top, (size_t)area.rows, l->count, theme->scroll);
}

/* --- the input line -------------------------------------------------------------------------- */

void mx_tui_input_set(mx_tui_input *in, const char *text, size_t max) {
    memset(in, 0, sizeof *in);
    in->max = max && max < MX_TUI_INPUT_MAX ? max : MX_TUI_INPUT_MAX - 1;
    snprintf(in->text, sizeof in->text, "%.*s", (int)in->max, text ? text : "");
    in->length = in->cursor = strlen(in->text);
    in->fresh = in->length > 0;
}

mx_tui_result mx_tui_input_event(mx_tui_input *in, mx_tui_event e) {
    if (in->fresh) {
        if ((e.key == MX_KEY_CHAR && e.ch >= 0x20 && e.ch < 0x7F) || e.key == MX_KEY_BACKSPACE || e.key == MX_KEY_DELETE) {
            in->text[0] = 0;
            in->length = in->cursor = in->scroll = 0;
            if (e.key != MX_KEY_CHAR) { in->fresh = 0; return MX_TUI_MOVED; }
        }
        if (e.key != MX_KEY_ENTER && e.key != MX_KEY_ESCAPE) in->fresh = 0;
    }
    switch (e.key) {
    case MX_KEY_ENTER: return MX_TUI_ACTIVATE;
    case MX_KEY_ESCAPE: return MX_TUI_CANCEL;
    case MX_KEY_LEFT: if (in->cursor) --in->cursor; return MX_TUI_MOVED;
    case MX_KEY_RIGHT: if (in->cursor < in->length) ++in->cursor; return MX_TUI_MOVED;
    case MX_KEY_HOME: in->cursor = 0; return MX_TUI_MOVED;
    case MX_KEY_END: in->cursor = in->length; return MX_TUI_MOVED;
    case MX_KEY_BACKSPACE:
        if (in->cursor) {
            memmove(in->text + in->cursor - 1, in->text + in->cursor, in->length - in->cursor + 1);
            --in->cursor;
            --in->length;
        }
        return MX_TUI_MOVED;
    case MX_KEY_DELETE:
        if (in->cursor < in->length) {
            memmove(in->text + in->cursor, in->text + in->cursor + 1, in->length - in->cursor);
            --in->length;
        }
        return MX_TUI_MOVED;
    case MX_KEY_CHAR:
        if (e.ch >= 0x20 && e.ch < 0x7F && in->length < in->max) {
            memmove(in->text + in->cursor + 1, in->text + in->cursor, in->length - in->cursor + 1);
            in->text[in->cursor++] = (char)e.ch;
            ++in->length;
        }
        return MX_TUI_MOVED;
    default: return MX_TUI_NONE;
    }
}

void mx_tui_input_draw(mx_tui *t, mx_tui_input *in, int row, int col, int width, uint8_t attr) {
    if (width < 2) return;
    /* The line scrolls so the cursor stays in view with one cell to spare at the right. */
    if (in->cursor < in->scroll) in->scroll = in->cursor;
    if (in->cursor >= in->scroll + (size_t)width - 1) in->scroll = in->cursor - (size_t)width + 2;
    if (in->fresh) attr = (uint8_t)((attr >> 4) | (attr << 4));       /* highlighted: it will be replaced */
    mx_tui_fill(t, mx_tui_rect_make(row, col, 1, width), ' ', attr);
    for (int i = 0; i < width && in->scroll + (size_t)i < in->length; ++i) {
        mx_tui_cell *c = cell(t, row, col + i);
        if (c) c->ch = (uint8_t)in->text[in->scroll + (size_t)i];
    }
    t->cursor_row = row;
    t->cursor_col = col + (int)(in->cursor - in->scroll);
}

/* --- message and confirm boxes -------------------------------------------------------------- */

void mx_tui_dialog_set(mx_tui_dialog *d, const char *title, const char *text, const char *buttons, int selected, int cancel) {
    memset(d, 0, sizeof *d);
    snprintf(d->title, sizeof d->title, "%s", title);
    snprintf(d->text, sizeof d->text, "%s", text);
    while (*buttons && d->buttons < 3) {
        const char *end = strchr(buttons, '|');
        const size_t n = end ? (size_t)(end - buttons) : strlen(buttons);
        snprintf(d->button[d->buttons++], sizeof d->button[0], "%.*s", (int)n, buttons);
        buttons += n + (end ? 1 : 0);
    }
    d->selected = selected >= 0 && selected < d->buttons ? selected : 0;
    d->cancel = cancel < d->buttons ? cancel : -1;
}

mx_tui_result mx_tui_dialog_event(mx_tui_dialog *d, mx_tui_event e) {
    switch (e.key) {
    case MX_KEY_LEFT:
    case MX_KEY_BACKTAB:
    case MX_KEY_UP: d->selected = (d->selected + d->buttons - 1) % d->buttons; return MX_TUI_MOVED;
    case MX_KEY_RIGHT:
    case MX_KEY_TAB:
    case MX_KEY_DOWN: d->selected = (d->selected + 1) % d->buttons; return MX_TUI_MOVED;
    case MX_KEY_ENTER: d->result = d->selected; return MX_TUI_ACTIVATE;
    case MX_KEY_ESCAPE:
        if (d->cancel < 0) return MX_TUI_NONE;
        d->result = d->cancel;
        return MX_TUI_ACTIVATE;
    default: return MX_TUI_NONE;
    }
}

/* The text broken into lines of at most `width` characters at spaces ('\n' breaks too). */
#define DIALOG_LINES 12
static int wrap(const char *text, int width, char lines[DIALOG_LINES][96], int *cut) {
    int n = 0;
    char line[96] = "";
    size_t used = 0;
    const char *p = text;
    while (*p && n < DIALOG_LINES) {
        if (*p == '\n') { snprintf(lines[n++], 96, "%s", line); line[0] = 0; used = 0; ++p; continue; }
        if (*p == ' ') { ++p; continue; }
        size_t word = 0;
        while (p[word] && p[word] != ' ' && p[word] != '\n') ++word;
        if (word > (size_t)width) { word = (size_t)width; if (cut) ++*cut; }
        if (used && used + 1 + word > (size_t)width) { snprintf(lines[n++], 96, "%s", line); line[0] = 0; used = 0; if (n == DIALOG_LINES) break; }
        if (used) line[used++] = ' ';
        memcpy(line + used, p, word);
        used += word;
        line[used] = 0;
        p += word;
        while (*p && *p != ' ' && *p != '\n') ++p;      /* the rest of a word too long for a line is dropped */
    }
    if (used && n < DIALOG_LINES) snprintf(lines[n++], 96, "%s", line);
    else if (used && cut) ++*cut;
    if (*p && n == DIALOG_LINES && cut) ++*cut;
    return n;
}

int mx_tui_put_wrapped(mx_tui *t, int row, int col, int width, int rows, uint8_t attr, const char *text) {
    char lines[DIALOG_LINES][96];
    if (width > 95) width = 95;
    int cut = 0;
    int n = wrap(text, width, lines, &cut);
    if (n > rows) { n = rows; ++cut; }
    t->clipped += (unsigned)cut;
    for (int i = 0; i < rows; ++i) mx_tui_put_fit(t, row + i, col, width, attr, i < n ? lines[i] : "", 0);
    return n;
}

void mx_tui_dialog_draw(mx_tui *t, const mx_tui_dialog *d, int top, int bottom) {
    char lines[DIALOG_LINES][96];
    int cut = 0;
    const int n = wrap(d->text, 50, lines, &cut);
    t->clipped += (unsigned)cut;
    int inner = (int)strlen(d->title) + 2, buttons = 0;
    for (int i = 0; i < n; ++i) if ((int)strlen(lines[i]) > inner) inner = (int)strlen(lines[i]);
    for (int i = 0; i < d->buttons; ++i) buttons += (int)strlen(d->button[i]) + 4 + (i ? 2 : 0);
    if (buttons > inner) inner = buttons;
    const mx_tui_theme *th = &POPUP;
    const mx_tui_rect r = mx_tui_centered(n + 5, inner + 6, top, bottom);
    mx_tui_fill(t, r, ' ', th->normal);
    mx_tui_box(t, r, MX_BOX_DOUBLE, th->frame, d->title);
    mx_tui_shadow(t, r);
    for (int i = 0; i < n; ++i) mx_tui_put_fit(t, r.row + 2 + i, r.col + 3, inner, th->normal, lines[i], MX_FIT_CENTER);
    int col = r.col + 3 + (inner - buttons) / 2;
    for (int i = 0; i < d->buttons; ++i) {
        char text[40];
        snprintf(text, sizeof text, "[ %s ]", d->button[i]);
        col += mx_tui_put(t, r.row + 2 + n + 1, col, i == d->selected ? th->selected : th->normal, text) + 2;
    }
}

/* --- the file picker --------------------------------------------------------------------------- */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }
static int compare_names(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) if (lower((unsigned char)*a) != lower((unsigned char)*b)) break;
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

static int compare_names_n(const char *a, const char *b, size_t n) {
    for (; n; ++a, ++b, --n) if (lower((unsigned char)*a) != lower((unsigned char)*b)) return lower((unsigned char)*a) - lower((unsigned char)*b);
    return 0;
}

static int entry_order(const void *x, const void *y) {
    const mx_tui_picker_entry *a = x, *b = y;
    if (a->is_folder != b->is_folder) return b->is_folder - a->is_folder;
    return compare_names(a->name, b->name);
}

static void clear_entries(mx_tui_picker *p) {
    for (size_t i = 0; i < p->count; ++i) free(p->entries[i].name);
    p->count = 0;
}

/* Whether `name` has one of the filter's extensions. */
static int matches(const char *filter, const char *name) {
    if (!*filter) return 1;
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    const size_t n = strlen(dot);
    for (const char *f = filter; *f;) {
        const char *end = strchr(f, ';');
        const size_t length = end ? (size_t)(end - f) : strlen(f);
        if (length == n && !compare_names_n(dot, f, n)) return 1;
        f += length + (end ? 1 : 0);
    }
    return 0;
}

typedef struct collect { mx_tui_picker *p; int failed; } collect;

static int add_entry(mx_tui_picker *p, const char *name, int is_folder, uint64_t size) {
    if (p->count == p->capacity) {
        const size_t capacity = p->capacity ? p->capacity * 2 : 64;
        mx_tui_picker_entry *e = realloc(p->entries, capacity * sizeof *e);
        if (!e) return 0;
        p->entries = e;
        p->capacity = capacity;
    }
    mx_tui_picker_entry *e = &p->entries[p->count];
    const size_t n = strlen(name);
    e->name = malloc(n + 1);
    if (!e->name) return 0;
    memcpy(e->name, name, n + 1);
    e->is_folder = is_folder;
    e->size = size;
    e->size_text[0] = 0;
    if (is_folder) snprintf(e->size_text, sizeof e->size_text, "%s", MX_TXT(PICK_DIR));
    else if (size >= 10u * 1024 * 1024) snprintf(e->size_text, sizeof e->size_text, "%uM", (unsigned)(size / (1024 * 1024)));
    else snprintf(e->size_text, sizeof e->size_text, "%uK", (unsigned)((size + 1023) / 1024));
    ++p->count;
    return 1;
}

static int collect_entry(void *ctx, const char *name, int is_folder, uint64_t size) {
    collect *c = ctx;
    if (!is_folder && !matches(c->p->filter, name)) return 1;
    if (c->p->count >= 20000) return 0;
    if (!add_entry(c->p, name, is_folder, size)) { c->failed = 1; return 0; }
    return 1;
}

/* Reads `folder` ("" the roots) into the list; `select` names the entry to
 * leave selected. 0 when out of memory. */
static int load(mx_tui_picker *p, const char *folder, const char *select) {
    collect c = {p, 0};
    clear_entries(p);
    p->error[0] = 0;
    snprintf(p->folder, sizeof p->folder, "%s", folder);
    int listed = 1;
    if (!*folder) listed = p->fs.roots(p->fs.ctx, collect_entry, &c);
    else listed = p->fs.list(p->fs.ctx, folder, collect_entry, &c);
    if (c.failed) return 0;
    if (!listed) snprintf(p->error, sizeof p->error, "%s", *folder ? MX_TXT(PICK_CANNOT_READ) : MX_TXT(PICK_NO_DRIVES));
    qsort(p->entries, p->count, sizeof *p->entries, entry_order);
    if (*folder && !add_entry(p, "..", 1, 0)) return 0;
    if (*folder) {      /* ".." first */
        p->entries[p->count - 1].size_text[0] = 0;
        mx_tui_picker_entry up = p->entries[p->count - 1];
        memmove(p->entries + 1, p->entries, (p->count - 1) * sizeof *p->entries);
        p->entries[0] = up;
    }
    mx_tui_item *items = realloc(p->items, (p->count ? p->count : 1) * sizeof *items);
    if (!items) return 0;
    p->items = items;
    size_t chosen = *folder && p->count > 1 ? 1 : 0;
    for (size_t i = 0; i < p->count; ++i) {
        p->items[i].kind = MX_ITEM_TEXT;
        p->items[i].enabled = 1;
        p->items[i].on = 0;
        p->items[i].label = p->entries[i].name;
        p->items[i].value = p->entries[i].size_text;
        if (select && !compare_names(p->entries[i].name, select)) chosen = i;
    }
    memset(&p->list, 0, sizeof p->list);
    mx_tui_list_set(&p->list, p->items, p->count, 12, 0);
    mx_tui_list_select(&p->list, chosen);
    return 1;
}

int mx_tui_picker_open(mx_tui_picker *p, const mx_tui_fs *fs, const char *title, const char *folder, const char *filter) {
    memset(p, 0, sizeof *p);
    p->fs = *fs;
    snprintf(p->title, sizeof p->title, "%s", title);
    snprintf(p->filter, sizeof p->filter, "%s", filter ? filter : "");
    for (char *f = p->filter; *f; ++f) *f = (char)lower((unsigned char)*f);
    if (load(p, folder, NULL)) {
        /* A folder that cannot be read falls back to the roots. */
        if (p->error[0] && *folder) return load(p, "", NULL);
        return 1;
    }
    mx_tui_picker_close(p);
    return 0;
}

void mx_tui_picker_close(mx_tui_picker *p) {
    clear_entries(p);
    free(p->entries);
    free(p->items);
    p->entries = NULL;
    p->items = NULL;
    p->capacity = 0;
}

/* The last part of a folder's path (after the final separator). */
static const char *last_part(const char *path) {
    const char *name = path;
    for (const char *s = path; *s; ++s) if ((*s == '/' || *s == '\\') && s[1]) name = s + 1;
    return name;
}

static void go_up(mx_tui_picker *p) {
    if (!*p->folder) return;
    char parent[MX_TUI_PATH];
    char came_from[MX_TUI_PATH];
    snprintf(came_from, sizeof came_from, "%s", last_part(p->folder));
    if (p->fs.parent(p->fs.ctx, p->folder, parent, sizeof parent)) load(p, parent, came_from);
    else load(p, "", p->folder);
}

mx_tui_result mx_tui_picker_event(mx_tui_picker *p, mx_tui_event e) {
    if (e.key == MX_KEY_BACKSPACE) { go_up(p); return MX_TUI_MOVED; }
    if (e.key == MX_KEY_ENTER && p->count) {
        const mx_tui_picker_entry *entry = &p->entries[p->list.selected];
        if (!strcmp(entry->name, "..") && *p->folder) { go_up(p); return MX_TUI_MOVED; }
        char path[MX_TUI_PATH];
        if (!*p->folder) snprintf(path, sizeof path, "%s", entry->name);
        else p->fs.join(p->fs.ctx, p->folder, entry->name, path, sizeof path);
        if (entry->is_folder) { load(p, path, NULL); return MX_TUI_MOVED; }
        snprintf(p->picked, sizeof p->picked, "%s", path);
        return MX_TUI_ACTIVATE;
    }
    const mx_tui_result r = mx_tui_list_event(&p->list, e);
    return r == MX_TUI_CANCEL ? MX_TUI_CANCEL : r == MX_TUI_MOVED ? MX_TUI_MOVED : MX_TUI_NONE;
}

void mx_tui_picker_draw(mx_tui *t, mx_tui_picker *p, int top, int bottom) {
    const mx_tui_theme *th = &POPUP;
    const int rows = bottom - top + 1 < 19 ? bottom - top + 1 : 19;
    const mx_tui_rect r = mx_tui_centered(rows, 64, top, bottom);
    mx_tui_fill(t, r, ' ', th->normal);
    mx_tui_box(t, r, MX_BOX_DOUBLE, th->frame, p->title);
    mx_tui_shadow(t, r);
    mx_tui_put_fit(t, r.row + 1, r.col + 2, r.cols - 4, th->heading, *p->folder ? p->folder : MX_TXT(PICK_DRIVES), MX_FIT_START);
    /* a rule under the folder, joined to the frame */
    const box_chars *b = t->ascii ? &BOX_PLAIN : &BOX_SINGLE;
    for (int x = 1; x < r.cols - 1; ++x) mx_tui_fill(t, mx_tui_rect_make(r.row + 2, r.col + x, 1, 1), b->h, th->frame);
    mx_tui_fill(t, mx_tui_rect_make(r.row + 2, r.col, 1, 1), t->ascii ? '+' : 0xC7, th->frame);
    mx_tui_fill(t, mx_tui_rect_make(r.row + 2, r.col + r.cols - 1, 1, 1), t->ascii ? '+' : 0xB6, th->frame);
    mx_tui_list_draw(t, &p->list, mx_tui_rect_make(r.row + 3, r.col + 1, r.rows - 5, r.cols - 2), th);
    char hint[80];
    if (p->error[0]) snprintf(hint, sizeof hint, "%s", p->error);
    else if (p->filter[0]) mx_text_format(hint, sizeof hint, MX_T_PICK_FILES, p->filter);
    else snprintf(hint, sizeof hint, "%s", MX_TXT(PICK_ALL_FILES));
    mx_tui_put_fit(t, r.row + r.rows - 2, r.col + 2, r.cols - 4, p->error[0] ? th->heading : th->normal, hint, 0);
}

/* Host keys to the 83-key XT keyboard's scan code set 1 make codes, by
 * position (IBM PC/XT Technical Reference, keyboard scan code table; the
 * Junior's keyboard is that layout). Each of the 83 keys has one host key
 * in the same place (the table); the host keys the XT keyboard lacks are
 * given the key whose job they do (the aliases): the cursor block and Home,
 * End, Insert, Delete, Page Up and Page Down are the keypad keys that carry
 * them on the XT, the right Ctrl and Alt are the only Ctrl and Alt, keypad
 * Enter is Enter and keypad / is /. F11, F12, Pause and Print Screen are
 * the shell's (the keyboard joystick, setup screen, pause, screenshot) and are not
 * here: they never reach the guest. MezinXT's own table. */
#include "desktop.h"

#include <SDL.h>

#define K(code, sdl, name) {code, SDL_SCANCODE_##sdl, name}
static const mx_key_def KEYS[] = {
    K(0x01, ESCAPE, "Esc"),
    K(0x02, 1, "1"), K(0x03, 2, "2"), K(0x04, 3, "3"), K(0x05, 4, "4"), K(0x06, 5, "5"), K(0x07, 6, "6"), K(0x08, 7, "7"),
    K(0x09, 8, "8"), K(0x0A, 9, "9"), K(0x0B, 0, "0"), K(0x0C, MINUS, "-"), K(0x0D, EQUALS, "="), K(0x0E, BACKSPACE, "Backspace"),
    K(0x0F, TAB, "Tab"),
    K(0x10, Q, "Q"), K(0x11, W, "W"), K(0x12, E, "E"), K(0x13, R, "R"), K(0x14, T, "T"), K(0x15, Y, "Y"), K(0x16, U, "U"),
    K(0x17, I, "I"), K(0x18, O, "O"), K(0x19, P, "P"), K(0x1A, LEFTBRACKET, "["), K(0x1B, RIGHTBRACKET, "]"),
    K(0x1C, RETURN, "Enter"), K(0x1D, LCTRL, "Ctrl"),
    K(0x1E, A, "A"), K(0x1F, S, "S"), K(0x20, D, "D"), K(0x21, F, "F"), K(0x22, G, "G"), K(0x23, H, "H"), K(0x24, J, "J"),
    K(0x25, K, "K"), K(0x26, L, "L"), K(0x27, SEMICOLON, ";"), K(0x28, APOSTROPHE, "'"), K(0x29, GRAVE, "`"),
    K(0x2A, LSHIFT, "Left Shift"), K(0x2B, BACKSLASH, "\\"),
    K(0x2C, Z, "Z"), K(0x2D, X, "X"), K(0x2E, C, "C"), K(0x2F, V, "V"), K(0x30, B, "B"), K(0x31, N, "N"), K(0x32, M, "M"),
    K(0x33, COMMA, ","), K(0x34, PERIOD, "."), K(0x35, SLASH, "/"), K(0x36, RSHIFT, "Right Shift"),
    K(0x37, KP_MULTIPLY, "PrtSc *"), K(0x38, LALT, "Alt"), K(0x39, SPACE, "Space"), K(0x3A, CAPSLOCK, "Caps Lock"),
    K(0x3B, F1, "F1"), K(0x3C, F2, "F2"), K(0x3D, F3, "F3"), K(0x3E, F4, "F4"), K(0x3F, F5, "F5"), K(0x40, F6, "F6"),
    K(0x41, F7, "F7"), K(0x42, F8, "F8"), K(0x43, F9, "F9"), K(0x44, F10, "F10"),
    K(0x45, NUMLOCKCLEAR, "Num Lock"), K(0x46, SCROLLLOCK, "Scroll Lock"),
    K(0x47, KP_7, "Keypad 7 Home"), K(0x48, KP_8, "Keypad 8 Up"), K(0x49, KP_9, "Keypad 9 PgUp"), K(0x4A, KP_MINUS, "Keypad -"),
    K(0x4B, KP_4, "Keypad 4 Left"), K(0x4C, KP_5, "Keypad 5"), K(0x4D, KP_6, "Keypad 6 Right"), K(0x4E, KP_PLUS, "Keypad +"),
    K(0x4F, KP_1, "Keypad 1 End"), K(0x50, KP_2, "Keypad 2 Down"), K(0x51, KP_3, "Keypad 3 PgDn"),
    K(0x52, KP_0, "Keypad 0 Ins"), K(0x53, KP_PERIOD, "Keypad . Del"),
};

/* Host keys that do the job of an XT key without sitting where it does. */
static const struct { int sdl; uint8_t code; } ALIASES[] = {
    {SDL_SCANCODE_KP_ENTER, 0x1C}, {SDL_SCANCODE_RCTRL, 0x1D}, {SDL_SCANCODE_RALT, 0x38}, {SDL_SCANCODE_KP_DIVIDE, 0x35},
    {SDL_SCANCODE_NONUSBACKSLASH, 0x2B},
    {SDL_SCANCODE_HOME, 0x47}, {SDL_SCANCODE_UP, 0x48}, {SDL_SCANCODE_PAGEUP, 0x49}, {SDL_SCANCODE_LEFT, 0x4B},
    {SDL_SCANCODE_RIGHT, 0x4D}, {SDL_SCANCODE_END, 0x4F}, {SDL_SCANCODE_DOWN, 0x50}, {SDL_SCANCODE_PAGEDOWN, 0x51},
    {SDL_SCANCODE_INSERT, 0x52}, {SDL_SCANCODE_DELETE, 0x53},
};

const mx_key_def *mx_desktop_keys(size_t *count) {
    *count = sizeof KEYS / sizeof KEYS[0];
    return KEYS;
}

int mx_desktop_xt_key(int scancode) {
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; ++i)
        if (KEYS[i].scancode == scancode) return KEYS[i].code;
    for (size_t i = 0; i < sizeof ALIASES / sizeof ALIASES[0]; ++i)
        if (ALIASES[i].sdl == scancode) return ALIASES[i].code;
    return -1;
}

/* The lock keys toggle on each press: a host that reports a toggle with one
 * event (not a press and a release) still gives the guest both. */
int mx_desktop_key_is_lock(int scancode) {
    return scancode == SDL_SCANCODE_CAPSLOCK || scancode == SDL_SCANCODE_NUMLOCKCLEAR || scancode == SDL_SCANCODE_SCROLLLOCK;
}

/* The XT key typing an ASCII character on the US layout (scan code set 1),
 * with whether Shift is held: -1 for a character that has no key. Used by the
 * --type option of the checks. */
int mx_desktop_char_key(int ch, int *shift) {
    static const struct { unsigned first; const char *plain, *shifted; } ROWS[] = {
        {0x02, "1234567890-=", "!@#$%^&*()_+"},
        {0x10, "qwertyuiop[]", "QWERTYUIOP{}"},
        {0x1E, "asdfghjkl;'`", "ASDFGHJKL:\"~"},
        {0x2B, "\\zxcvbnm,./", "|ZXCVBNM<>?"},
    };
    *shift = 0;
    if (ch == ' ') return 0x39;
    for (unsigned r = 0; r < sizeof ROWS / sizeof ROWS[0]; ++r) {
        for (unsigned i = 0; ROWS[r].plain[i]; ++i) {
            if (ROWS[r].plain[i] == ch) return (int)(ROWS[r].first + i);
            if (ROWS[r].shifted[i] == ch) { *shift = 1; return (int)(ROWS[r].first + i); }
        }
    }
    return -1;
}

#include <string.h>
#include <stdio.h>

#include "volume_bar.h"

// The drawing follows FPGA-Companion's menu_volume_bar(), which draws the same bar with u8g2
// for the Apple IIe core, with its strip at y = 0: the same icons, positions and font.

#define W 128
#define H 16

// the bitmap, one byte per 8 pixels across, bit 0 the leftmost
static uint8_t bits[H][W / 8];

static void pixel(int x, int y) {
    if (x >= 0 && x < W && y >= 0 && y < H)
        bits[y][x / 8] |= 1 << (x % 8);
}

static void box(int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            pixel(i, j);
}

static void frame(int x, int y, int w, int h) {
    box(x, y, w, 1);
    box(x, y + h - 1, w, 1);
    box(x, y, 1, h);
    box(x + w - 1, y, 1, h);
}

// an XBM image 4 pixels wide and 8 high: one byte per row, bit 0 the leftmost pixel
static void xbm4x8(int x, int y, const uint8_t *rows) {
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 4; i++)
            if (rows[j] >> i & 1)
                pixel(x + i, y + j);
}

static const uint8_t icn_speaker[] = { 0x08, 0x0c, 0x0f, 0x0f, 0x0f, 0x0f, 0x0c, 0x08 };
static const uint8_t icn_sound[]   = { 0x00, 0x04, 0x09, 0x0a, 0x0a, 0x09, 0x04, 0x00 };
static const uint8_t icn_mute[]    = { 0x00, 0x00, 0x09, 0x06, 0x06, 0x09, 0x00, 0x00 };

// Digits 0-9 of u8g2_font_helvB08_tr (u8g2), from the X11 font
// -Adobe-Helvetica-Bold-R-Normal--11-80-100-100-P-60-ISO10646-1:
//   Copyright (c) 1984, 1987 Adobe Systems Incorporated. All Rights Reserved.
//   Copyright (c) 1988, 1991 Digital Equipment Corporation. All Rights Reserved.
// Each digit advances 6 pixels. rows: the 8 rows above the baseline, as drawn from the pen
// position (bit 0 there); xoff and w: the glyph's x offset and width, for the string width.
#define DIGIT_ADVANCE 6
static const struct {
    uint8_t xoff, w;
    uint8_t rows[8];
} digits[10] = {
    {0, 5, {0x0e, 0x1b, 0x1b, 0x1b, 0x1b, 0x1b, 0x1b, 0x0e}},   // 0
    {1, 3, {0x0c, 0x0e, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c}},   // 1
    {0, 5, {0x0e, 0x1b, 0x18, 0x18, 0x0c, 0x06, 0x03, 0x1f}},   // 2
    {0, 5, {0x0e, 0x1b, 0x18, 0x0c, 0x18, 0x18, 0x1b, 0x0e}},   // 3
    {0, 6, {0x10, 0x18, 0x1c, 0x1a, 0x19, 0x3f, 0x18, 0x18}},   // 4
    {0, 5, {0x1f, 0x03, 0x03, 0x0f, 0x18, 0x19, 0x1b, 0x0e}},   // 5
    {0, 5, {0x0e, 0x1b, 0x03, 0x0f, 0x1b, 0x1b, 0x1b, 0x0e}},   // 6
    {0, 5, {0x1f, 0x18, 0x18, 0x0c, 0x0c, 0x06, 0x06, 0x06}},   // 7
    {0, 5, {0x0e, 0x1b, 0x1b, 0x0e, 0x1b, 0x1b, 0x1b, 0x0e}},   // 8
    {0, 5, {0x0e, 0x1b, 0x1b, 0x1b, 0x1e, 0x18, 0x1b, 0x0e}},   // 9
};

// width of a string of digits, as u8g2_GetStrWidth() gives it: the advance of all but the
// last digit, then the last one's offset and width, plus the first one's offset
static int str_width(const char *s) {
    int n = strlen(s);
    if (n == 0)
        return 0;
    int w = (n - 1) * DIGIT_ADVANCE;
    w += digits[s[n - 1] - '0'].xoff + digits[s[n - 1] - '0'].w;
    w += digits[s[0] - '0'].xoff;
    return w;
}

// a string of digits from the pen position x, baseline y
static void draw_str(int x, int y, const char *s) {
    for (; *s; s++, x += DIGIT_ADVANCE)
        for (int j = 0; j < 8; j++)
            for (int i = 0; i < 8; i++)
                if (digits[*s - '0'].rows[j] >> i & 1)
                    pixel(x + i, y - 8 + j);
}

void volume_bar_font(int pos, int steps, uint8_t font[VOLUME_BAR_CHARS * 8]) {
    // the number sits in a slot as wide as the widest label, ending 4 pixels from the
    // right; with the speaker 6 pixels from the left both sides get about the same
    // padding. The bar fills the space between.
    char label[8];
    int nw = 0;
    for (int i = 0; i <= steps; i++) {
        snprintf(label, sizeof(label), "%d", i);
        int w = str_width(label);
        if (w > nw) nw = w;
    }
    if (steps < 1) steps = 1;
    snprintf(label, sizeof(label), "%d", pos);

    const int y = 0, ix = 6, slot_r = 124, slot_l = slot_r - nw;
    const int bx = ix + 13, bw = slot_l - 4 - bx, by = y + 5, bh = 7;
    memset(bits, 0, sizeof(bits));
    xbm4x8(ix, y + 4, icn_speaker);
    if (pos) xbm4x8(ix + 5, y + 4, icn_sound);
    else     xbm4x8(ix + 6, y + 4, icn_mute);
    frame(bx, by, bw, bh);
    if (pos) box(bx + 2, by + 2, (bw - 4) * pos / steps, bh - 4);
    for (int t = 1; t < steps; t++) pixel(bx + 2 + (bw - 4) * t / steps, by - 2);    // a tick per step
    draw_str(slot_l + (nw - str_width(label) + 1) / 2, y + 12, label);

    for (int c = 0; c < VOLUME_BAR_CHARS; c++)
        for (int j = 0; j < 8; j++)
            font[c * 8 + j] = bits[c / 16 * 8 + j][c % 16];
}

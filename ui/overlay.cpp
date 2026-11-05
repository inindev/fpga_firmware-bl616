#include <string>
#include <stdarg.h>
#include <algorithm>

extern "C" {
#include <FreeRTOS.h>
#include <task.h>
}
#include "utils.h"
#include "overlay.h"

/////////////////////////////////////////////////////////////////////////////////
// Overlay and other core control over UART


int _overlay_on = 1;
int overlay_on() {
    return _overlay_on;
}

// Overlay text is UTF-8. The font has the 256 Latin-1 characters, so each character is sent
// as its code point; one above U+00FF is sent as '?'.

// Copy of the screen (font codes), so that it can be sent again. The FPGA's cursor is
// tracked the same way: it stops at column 32.
static uint8_t screen[28][32];
static int cur_x, cur_y;
static int hl_row = -1;

static bool utf8_cont(uint8_t c) {
    return (c & 0xC0) == 0x80;
}

// byte offset of character k in s (s.size() if s has k characters or fewer)
static size_t utf8_offset(const std::string &s, int k) {
    for (size_t i = 0; i < s.size(); i++)
        if (!utf8_cont(s[i]) && k-- == 0)
            return i;
    return s.size();
}

int utf8_len(const std::string &s) {
    int n = 0;
    for (uint8_t c : s)
        n += !utf8_cont(c);
    return n;
}

// font codes for UTF-8 text: one per character. A continuation byte with no lead byte
// is skipped; a malformed or overlong sequence is one '?'.
static std::string utf8_to_font(const char *s) {
    std::string out;
    for (const uint8_t *p = (const uint8_t *)s; *p; ) {
        uint8_t c = *p++;
        if (c < 0x80) {
            out += c;
            continue;
        }
        if (utf8_cont(c))
            continue;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
        uint32_t cp = c & (0x3F >> more);
        int got = 0;
        for (; got < more && utf8_cont(*p); got++)
            cp = cp << 6 | (*p++ & 0x3F);
        while (utf8_cont(*p))
            p++;
        out += (got == more && cp >= 0x80 && cp <= 0xFF) ? (char)cp : '?';
    }
    return out;
}

void overlay_cursor(int col, int row) {
    // uart1 command: 4 x[7:0] y[7:0]
    taskENTER_CRITICAL();
    fpga_tx_header(0x04, 3);
    fpga_tx_byte(col);
    fpga_tx_byte(row);
    taskEXIT_CRITICAL();
    cur_x = col;
    cur_y = row;
}

// print font codes at the cursor, keeping the copy up to date
static void send_codes(const uint8_t *codes, int len) {
    taskENTER_CRITICAL();
    fpga_tx_header(0x05, len+1);
    for (int i = 0; i < len; i++) {
        if (cur_x < 32 && cur_y >= 0 && cur_y < 28)
            screen[cur_y][cur_x++] = codes[i];
        fpga_tx_byte(codes[i]);
    }
    taskEXIT_CRITICAL();
}

// print UTF-8 text at the cursor
static void send_text(const char *s) {
    std::string codes = utf8_to_font(s);
    send_codes((const uint8_t *)codes.data(), codes.size());
}

static void send_highlight() {
    // uart1 command: 0x0f row[7:0] x0[7:0] x1[7:0] (row 0xFF: none)
    taskENTER_CRITICAL();
    fpga_tx_header(0x0f, 4);
    fpga_tx_byte(hl_row >= 0 && hl_row < 28 ? hl_row : 0xFF);
    fpga_tx_byte(1);
    fpga_tx_byte(30);
    taskEXIT_CRITICAL();
}

// send the whole screen again from the copy, row by row with its final content, and the
// highlight: restores them if the core was reset, and never shows a blank screen as a
// clear would
void overlay_refresh() {
    int x = cur_x, y = cur_y;
    uint8_t line[32];
    for (int row = 0; row < 28; row++) {
        for (int col = 0; col < 32; col++)
            line[col] = screen[row][col] ? screen[row][col] : ' ';
        overlay_cursor(0, row);
        send_codes(line, 32);
    }
    send_highlight();
    overlay_cursor(x, y);
}

// highlight bar on one row (-1: none), over columns 1-30 inside the frame
void overlay_highlight(int row) {
    if (row == hl_row)
        return;
    hl_row = row;
    send_highlight();
}

// print to UART without the core displaying it. liveuart.py catches this.
void dprint(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    taskENTER_CRITICAL();
    int len = strlen(buf);
    fpga_tx_header(0x0d, len+1);
    for(int i = 0; i < len; i++) {
        fpga_tx_byte(buf[i]);
    }
    taskEXIT_CRITICAL();
}

void overlay_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    send_text(buf);
}

void overlay_clear() {
    overlay_highlight(-1);
    for (int i = 0; i < 28; i++) {
        overlay_cursor(0, i);
        //              01234567890123456789012345678901
        overlay_printf("                                ");
    }
}

// print one line centered on `row`, cut to the 32 columns
void overlay_center(int row, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    std::string s = utf8_head(buf, 32);
    overlay_cursor((32 - utf8_len(s)) / 2, row);
    send_text(s.c_str());
}

// the status row also holds the button hints: pad to the end of the row to replace them
void overlay_status(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    std::string s(buf);
    int n = utf8_len(s);
    if (n < 31)
        s.append(31 - n, ' ');

    overlay_cursor(1, OVERLAY_STATUS);
    send_text(s.c_str());
}

// double-line frame around the content area (rows 5-24, columns 0-31), with labels in its
// top edge: `left` after the corner (its start cut off if too long), `right` before the
// other corner
void overlay_frame(const char *left, const char *right) {
    std::string top(BOX2_TL);
    top += std::string(30, BOX2_H[0]) + BOX2_TR;
    // top is one byte per column until the labels go in: right first, then left before it
    int rlen = utf8_len(right);
    int rstart = 31;                // first column of " right "
    if (rlen) {
        rstart = 30 - (rlen + 2);
        top.replace(rstart, rlen + 2, std::string(" ") + right + " ");
    }
    std::string label(left);
    int maxlen = rstart - 5;        // corner, one line each side, two spaces
    int len = utf8_len(label);
    if (len > maxlen) {
        label = GLYPH_ELLIPSIS + utf8_tail(label, maxlen - 1);
        len = maxlen;
    }
    if (len)
        top.replace(2, len + 2, " " + label + " ");

    overlay_cursor(0, FRAME_TOP);
    overlay_printf("%s", top.c_str());
    for (int y = FRAME_TOP + 1; y < FRAME_BOTTOM; y++) {
        overlay_cursor(0, y);
        overlay_printf(BOX2_V);
        overlay_cursor(31, y);
        overlay_printf(BOX2_V);
    }
    std::string bottom(BOX2_BL);
    bottom += std::string(30, BOX2_H[0]) + BOX2_BR;
    overlay_cursor(0, FRAME_BOTTOM);
    overlay_printf("%s", bottom.c_str());
}

// button hints, centered on the status row
void overlay_hints(const char *hints) {
    overlay_cursor(0, OVERLAY_STATUS);
    overlay_printf("%32s", "");
    overlay_center(OVERLAY_STATUS, "%s", hints);
}

// the first n characters of s
std::string utf8_head(const std::string &s, int n) {
    return s.substr(0, utf8_offset(s, n));
}

// the last n characters of s
std::string utf8_tail(const std::string &s, int n) {
    int len = utf8_len(s);
    return n >= len ? s : s.substr(utf8_offset(s, len - n));
}

// s cut to `width` characters, ending in an ellipsis if it was longer
std::string overlay_fit(const std::string &s, int width) {
    if (utf8_len(s) <= width)
        return s;
    return utf8_head(s, width - 1) + GLYPH_ELLIPSIS;
}

// show a pop-up message, press any key to discard (caller needs to redraw screen)
// msg: could be multi-line (separate with \n), max 10 lines
// center: whether to center the text
void overlay_message(const char *msg, int center) {
    overlay_highlight(-1);      // the pop-up may cover the highlighted row
    // split into lines (at most 10), each cut to 26 characters
    std::string text[10];
    int w[10], lines = 0, maxw = 0;
    for (const char *sol = msg; lines < 10; lines++) {
        const char *eol = strchr(sol, '\n');
        text[lines] = utf8_head(eol ? std::string(sol, eol - sol) : std::string(sol), 26);
        w[lines] = utf8_len(text[lines]);
        maxw = std::max(w[lines], maxw);
        if (!eol) {
            lines++;
            break;
        }
        sol = eol + 1;
    }
    // status("");
    // printf("w=%d, lines=%d", maxw, lines);
    // draw a double-line box
    int y0 = 14 - ((lines + 2) >> 1);
    int y1 = y0 + lines + 2;
    int x0 = 16 - ((maxw + 2) >> 1);
    int x1 = x0 + maxw + 2;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            bool top = y == y0, bottom = y == y1-1, left = x == x0, right = x == x1-1;
            overlay_cursor(x, y);
            if (top && left)
                overlay_printf(BOX2_TL);
            else if (top && right)
                overlay_printf(BOX2_TR);
            else if (bottom && left)
                overlay_printf(BOX2_BL);
            else if (bottom && right)
                overlay_printf(BOX2_BR);
            else if (left || right)
                overlay_printf(BOX2_V);
            else if (top || bottom)
                overlay_printf(BOX2_H);
            else
                overlay_printf(" ");
        }
    // print text
    for (int i = 0; i < lines; i++) {
        if (center)
            overlay_cursor(16-(w[i]>>1), y0+i+1);
        else
            overlay_cursor(x0+1, y0+i+1);
        send_text(text[i].c_str());
    }
    overlay_wait_button();
}

// wait for A or B on either controller, or at most timeout_ms (0: no limit)
void overlay_wait_button(int timeout_ms) {
    delay(300);
    for (int waited = 0; timeout_ms == 0 || waited < timeout_ms; waited += 10) {
        uint16_t joy1=0, joy2=0, hid1=0, hid2=0;
        get_joypad_states(&joy1, &joy2, &hid1, &hid2);
        joy1 |= hid1; joy2 |= hid2;
        if ((joy1 & 0x1) || (joy1 & 0x100) || (joy2 & 0x1) || (joy2 & 0x100))
            break;
        delay(10);
    }
    delay(300);
}

// turn overlay on/off
void overlay(int state) {
    taskENTER_CRITICAL();
    _overlay_on = state;
    fpga_tx_header(0x08, 2);
    fpga_tx_byte(state);        
    taskEXIT_CRITICAL();
}


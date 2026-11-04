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

// Copy of the screen, so that a row can be sent again with the highlight bit (bit 7) set
// on columns 1-30. The FPGA's cursor is tracked the same way: it stops at column 32.
static char screen[28][32];
static int cur_x, cur_y;
static int hl_row = -1;

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

// print at the cursor, keeping the copy and the highlight up to date
static void send_text(const char *s) {
    taskENTER_CRITICAL();
    int len = strlen(s);
    fpga_tx_header(0x05, len+1);
    for(int i = 0; i < len; i++) {
        uint8_t c = s[i];
        if (cur_x < 32 && cur_y >= 0 && cur_y < 28) {
            screen[cur_y][cur_x] = c;
            if (cur_y == hl_row && cur_x >= 1 && cur_x <= 30)
                c |= 0x80;
            cur_x++;
        }
        fpga_tx_byte(c);
    }
    taskEXIT_CRITICAL();
}

// send columns 1-30 of a row again, highlighted if it is hl_row
static void redraw_row(int row) {
    char line[31];
    for (int x = 1; x <= 30; x++)
        line[x-1] = screen[row][x] ? screen[row][x] : ' ';
    line[30] = '\0';
    overlay_cursor(1, row);
    send_text(line);
}

// highlight bar on one row (-1: none), over columns 1-30 inside the frame
void overlay_highlight(int row) {
    if (row == hl_row)
        return;
    int x = cur_x, y = cur_y, old = hl_row;
    hl_row = row;
    if (old >= 0 && old < 28)
        redraw_row(old);
    if (row >= 0 && row < 28)
        redraw_row(row);
    overlay_cursor(x, y);
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
    hl_row = -1;
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
    char buf[33];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    overlay_cursor((32 - strlen(buf)) / 2, row);
    overlay_printf("%s", buf);
}

// the status row also holds the button hints: pad to the end of the row to replace them
void overlay_status(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    buf[255] = '\0';
    for (int n = strlen(buf); n < 31; n++) {
        buf[n] = ' ';
        buf[n+1] = '\0';
    }

    overlay_cursor(1, OVERLAY_STATUS);
    send_text(buf);
}

// double-line frame around the content area (rows 5-24, columns 0-31), with labels in its
// top edge: `left` after the corner (its start cut off if too long), `right` before the
// other corner
void overlay_frame(const char *left, const char *right) {
    std::string top(BOX2_TL);
    top += std::string(30, BOX2_H[0]) + BOX2_TR;
    int rlen = strlen(right);
    int rstart = 31;                // first column of " right "
    if (rlen) {
        rstart = 30 - (rlen + 2);
        top.replace(rstart, rlen + 2, std::string(" ") + right + " ");
    }
    std::string label(left);
    int maxlen = rstart - 5;        // corner, one line each side, two spaces
    if ((int)label.size() > maxlen)
        label = GLYPH_ELLIPSIS + label.substr(label.size() - (maxlen - 1));
    if (!label.empty())
        top.replace(2, label.size() + 2, " " + label + " ");

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

// s cut to `width` characters, ending in an ellipsis if it was longer
std::string overlay_fit(const std::string &s, int width) {
    if ((int)s.size() <= width)
        return s;
    return s.substr(0, width - 1) + GLYPH_ELLIPSIS;
}

// show a pop-up message, press any key to discard (caller needs to redraw screen)
// msg: could be multi-line (separate with \n), max 10 lines
// center: whether to center the text
void overlay_message(const char *msg, int center) {
    overlay_highlight(-1);      // the pop-up may cover the highlighted row
    // count number of lines and max width
    int w[10], lines=10, maxw = 0;
    int len = strlen(msg);
    const char *end = msg + len;
    const char *sol = msg;
    for (int i = 0; i < 10; i++) {
        const char *eol = strchr(sol, '\n');
        if (eol) { // found \n
            w[i] = std::min(eol - sol, 26);
            maxw = std::max(w[i], maxw);
            sol = eol+1;
        } else {
            w[i] = std::min(end - sol, 26);
            maxw = std::max(w[i], maxw);
            lines = i+1;
            break;
        }		
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
    const char *s = msg;
    for (int i = 0; i < lines; i++) {
        if (center)
            overlay_cursor(16-(w[i]>>1), y0+i+1);
        else
            overlay_cursor(x0+1, y0+i+1);
        while (*s != '\n' && *s != '\0') {
            overlay_printf("%c", *s);
            s++;
        }
        s++;
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


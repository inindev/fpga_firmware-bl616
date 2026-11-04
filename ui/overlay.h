#pragma once

#include <string>

// Screen layout (32x28 characters): rows 0-4 are the header (the core's logo, drawn by the
// FPGA on pixel lines 22-35), rows 5-24 the frame around the content, then status and
// button hints, a blank row, and the footer (version).
#define FRAME_TOP       5
#define FRAME_BOTTOM    24
#define OVERLAY_TOP     6       // first content row
#define OVERLAY_ROWS    18      // content rows 6-23, columns 1-30
#define OVERLAY_STATUS  25      // status messages and button hints
#define OVERLAY_FOOTER  27

// Extra glyphs in the menu font (see scripts/font_glyphs.py), as strings for
// overlay_printf. 0x09, 0x0A and 0x0D are left blank, so "\t", "\n" and "\r" print nothing.
#define BOX_H           "\x01"  // ─
#define BOX_V           "\x02"  // │
#define BOX_TL          "\x03"  // ┌
#define BOX_TR          "\x04"  // ┐
#define BOX_BL          "\x05"  // └
#define BOX_BR          "\x06"  // ┘
#define BOX_LT          "\x07"  // ├
#define BOX_RT          "\x08"  // ┤
#define BOX2_H          "\x0b"  // ═
#define BOX2_V          "\x0c"  // ║
#define BOX2_TL         "\x0e"  // ╔
#define BOX2_TR         "\x0f"  // ╗
#define BOX2_BL         "\x10"  // ╚
#define BOX2_BR         "\x11"  // ╝
#define BOX2_LT         "\x12"  // ╠
#define BOX2_RT         "\x13"  // ╣
#define GLYPH_UP        "\x14"  // ▲ more above
#define GLYPH_DOWN      "\x15"  // ▼ more below
#define GLYPH_LEFT      "\x16"  // ◄
#define GLYPH_RIGHT     "\x17"  // ►
#define GLYPH_FOLDER    "\x18"
#define GLYPH_FILE      "\x19"
#define GLYPH_A         "\x1a"  // A button
#define GLYPH_B         "\x1b"  // B button
#define GLYPH_ELLIPSIS  "\x1c"  // …

extern int _overlay_on;
extern int overlay_on();
extern void overlay_cursor(int x, int y);
extern void dprint(const char *fmt, ...);
extern void overlay_printf(const char *fmt, ...);
extern void overlay_clear();
extern void overlay_center(int row, const char *fmt, ...);
extern void overlay_status(const char *fmt, ...);
extern void overlay_frame(const char *left = "", const char *right = "");
extern void overlay_hints(const char *hints);
extern void overlay_highlight(int row);     // highlight bar on columns 1-30 (-1: none)
extern std::string overlay_fit(const std::string &s, int width);
extern void overlay_message(const char *msg, int center);
extern void overlay_wait_button(int timeout_ms = 0);   // wait for A or B (0: no limit)
extern void overlay(int state);     // turn overlay on/off

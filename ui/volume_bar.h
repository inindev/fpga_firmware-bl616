#pragma once

#include <stdint.h>

// TV style volume bar, as on the Apple IIe core's OSD strip: a 128x16 pixel bitmap with a
// speaker (sound waves, or an X when muted), a bar with a tick per step, and the number.
// It is shown through the 32 font characters 0x80-0x9F (16 columns by 2 rows, iosys command
// 0x10), so it comes out as their font bytes: character i (0-31) is at column i % 16, row
// i / 16, and has 8 bytes, top row first, bit 0 the leftmost pixel.
#define VOLUME_BAR_CHARS    32
void volume_bar_font(int pos, int steps, uint8_t font[VOLUME_BAR_CHARS * 8]);

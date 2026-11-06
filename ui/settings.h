#pragma once

// Settings saved in tangcore.ini at the root of the drive (SD card or USB drive), an INI
// file with a [TangCore] section:
//
//   [TangCore]
//   ; menu font: serif or sans
//   font=serif
//   ; gamepad button for the menu: select+start or select+right
//   menu_button=select+start
//
// A missing file, an unknown key or an unknown value leaves the default. Keys in other
// sections are ignored, which leaves room for per-core sections.

// Menu font, an index into menu_fonts (menu_fonts.h): 0 is the one in the cores' font ROM
#define FONT_SERIF  0
#define FONT_SANS   1
extern int option_font;

// read the settings from the drive (drv); false if there is no file or it can't be read
bool settings_load(void);
// write them; false if that failed
bool settings_save(void);

// Write the chosen font into the running core's font, if it doesn't have it yet. A core
// starts with the ROM font: call menu_font_reset() whenever a core is loaded.
void menu_font_sync(void);
void menu_font_reset(void);

// the Options screen, from the main menu
void options_menu(void);

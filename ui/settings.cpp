#include <string>
#include <string.h>
#include <strings.h>

#include "settings.h"
#include "menu_fonts.h"
#include "overlay.h"
#include "utils.h"

int option_font = FONT_SERIF;
int option_burgertime_rotate = 1;

// A setting: its key in the file, the comment written above it, its name in Options, the
// variable it sets and its choices (value in the file, label in Options, variable's value).
struct Choice {
    const char *value;
    const char *label;
    int id;
};
struct Setting {
    const char *key;
    const char *comment;
    const char *name;
    int *var;
    const Choice *choices;
    int count;
};

static const Choice font_choices[] = {
    {"serif", "Serif", FONT_SERIF},
    {"sans", "Sans", FONT_SANS},
};
static const Choice menu_button_choices[] = {
    {"select+start", "Select+Start", OPTION_OSD_KEY_SELECT_START},
    {"select+right", "Select+Right", OPTION_OSD_KEY_SELECT_RIGHT},
};
static const Setting settings[] = {
    {"font", "menu font: serif or sans", "Font", &option_font, font_choices, 2},
    {"menu_button", "gamepad button for the menu: select+start or select+right", "Menu button",
     &option_osd_key, menu_button_choices, 2},
};
#define SETTINGS_COUNT (int)(sizeof(settings) / sizeof(settings[0]))

// Core settings, each in the core's section, changed in the core's in-game menu (not in
// Options). A core's settings are next to each other.
struct CoreSetting {
    const char *section;
    Setting setting;
};

static const Choice on_off_choices[] = {
    {"on", "On", 1},
    {"off", "Off", 0},
};
static const CoreSetting core_settings[] = {
    {"burgertime", {"rotate", "rotate the picture: on or off", "Rotate screen",
                    &option_burgertime_rotate, on_off_choices, 2}},
};
#define CORE_SETTINGS_COUNT (int)(sizeof(core_settings) / sizeof(core_settings[0]))

// the file and its text, where the USB host can reach them (as fcore and fbuf in main.cpp)
static USB_NOCACHE_RAM_SECTION FIL fset;
static USB_NOCACHE_RAM_SECTION char __attribute__((aligned(64))) ini_text[1024];

static std::string settings_path(void) {
    return std::string(drv) + "tangcore.ini";
}

// s without leading and trailing white space (cut in place)
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = 0;
    return s;
}

// value from the file in the form the choices use: lower case is matched later, spaces go
// ("Select + Start" is select+start)
static void drop_spaces(char *s) {
    char *d = s;
    for (; *s; s++)
        if (*s != ' ' && *s != '\t')
            *d++ = *s;
    *d = 0;
}

static void set_choice(const Setting &s, const char *key, const char *value) {
    if (strcasecmp(key, s.key) != 0)
        return;
    for (int j = 0; j < s.count; j++)
        if (strcasecmp(value, s.choices[j].value) == 0)
            *s.var = s.choices[j].id;
}

static void set_value(const char *section, const char *key, char *value) {
    drop_spaces(value);
    if (strcasecmp(section, "TangCore") == 0)
        for (int i = 0; i < SETTINGS_COUNT; i++)
            set_choice(settings[i], key, value);
    for (int i = 0; i < CORE_SETTINGS_COUNT; i++)
        if (strcasecmp(section, core_settings[i].section) == 0)
            set_choice(core_settings[i].setting, key, value);
}

bool settings_load(void) {
    std::string path = settings_path();
    if (f_open(&fset, path.c_str(), FA_READ) != FR_OK)
        return false;
    UINT br = 0;
    FRESULT r = f_read(&fset, ini_text, sizeof(ini_text) - 1, &br);
    f_close(&fset);
    if (r != FR_OK)
        return false;
    ini_text[br] = 0;

    char *line = ini_text;
    if (strncmp(line, "\xEF\xBB\xBF", 3) == 0)      // UTF-8 byte order mark, from some editors
        line += 3;
    const char *section = "TangCore";       // before any section: [TangCore]
    while (line) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        char *s = trim(line);
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end)
                *end = 0;
            section = trim(s + 1);
        } else if (*s && *s != ';' && *s != '#') {
            char *eq = strchr(s, '=');
            if (eq) {
                *eq = 0;
                char *value = eq + 1;
                char *comment = strchr(value, ';');
                if (comment)
                    *comment = 0;
                set_value(section, trim(s), trim(value));
            }
        }
        line = next;
    }
    DEBUG("settings: font=%d menu_button=%d burgertime rotate=%d\n", option_font, option_osd_key,
          option_burgertime_rotate);
    return true;
}

// a setting's lines in the file: its comment, then key=value
static std::string setting_text(const Setting &s) {
    const char *value = s.choices[0].value;
    for (int j = 0; j < s.count; j++)
        if (*s.var == s.choices[j].id)
            value = s.choices[j].value;
    return std::string("; ") + s.comment + "\n" + s.key + "=" + value + "\n";
}

// The whole file is written again, with every setting.
bool settings_save(void) {
    std::string text = "; TangCore settings, written by the menus\n[TangCore]\n";
    for (int i = 0; i < SETTINGS_COUNT; i++)
        text += setting_text(settings[i]);
    for (int i = 0; i < CORE_SETTINGS_COUNT; i++) {
        const CoreSetting &c = core_settings[i];
        if (i == 0 || strcmp(c.section, core_settings[i - 1].section) != 0)
            text += std::string("[") + c.section + "]\n";
        text += setting_text(c.setting);
    }
    if (text.size() > sizeof(ini_text))
        return false;
    memcpy(ini_text, text.data(), text.size());

    std::string path = settings_path();
    if (f_open(&fset, path.c_str(), FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return false;
    UINT bw = 0;
    bool ok = f_write(&fset, ini_text, text.size(), &bw) == FR_OK && bw == text.size();
    if (f_close(&fset) != FR_OK)        // the data only reaches the drive here
        ok = false;
    if (ok)
        f_chmod(path.c_str(), AM_HID, AM_HID);      // out of the way, as snestang.ini
    return ok;
}

// The font the running core has: a core starts with the ROM font, menu_fonts[0].
static int core_font = 0;

void menu_font_reset(void) {
    core_font = 0;
}

void menu_font_sync(void) {
    if (option_font < 0 || option_font >= MENU_FONT_COUNT || option_font == core_font)
        return;
    // codes 0x20-0x7F, then 0xA0-0xFF, 8 characters per frame as the volume bar
    // (0x80-0x9F are the volume bar's)
    const uint8_t *font = menu_fonts[option_font];
    for (int i = 0; i < 192; i += 8) {
        int code = i < 96 ? 0x20 + i : 0xA0 + (i - 96);
        taskENTER_CRITICAL();
        fpga_tx_header(0x10, 2 + 64);
        fpga_tx_byte(code);
        for (int j = 0; j < 64; j++)
            fpga_tx_byte(font[i * 8 + j]);
        taskEXIT_CRITICAL();
    }
    core_font = option_font;
}

// Options screen: one row per setting, Left/Right change the value (round), the font at
// once, A or B go back. Saves when leaving, if anything changed.
#define VALUE_X     15      // left arrow; the value is centered in the 13 columns after it
#define VALUE_W     13

static void draw_row(int y, const Setting &s) {
    const char *label = s.choices[0].label;
    for (int j = 0; j < s.count; j++)
        if (*s.var == s.choices[j].id)
            label = s.choices[j].label;
    char value[VALUE_W + 1];
    int len = strlen(label);
    int pad = (VALUE_W - len) / 2;
    snprintf(value, sizeof(value), "%*s%s%*s", pad, "", label, VALUE_W - len - pad, "");
    overlay_cursor(2, y);
    overlay_printf("%-12s", s.name);
    overlay_cursor(VALUE_X, y);
    overlay_printf(GLYPH_LEFT "%s" GLYPH_RIGHT, value);
}

static void step(const Setting &s, int dir) {
    int j = 0;
    for (int k = 0; k < s.count; k++)
        if (*s.var == s.choices[k].id)
            j = k;
    j = (j + dir + s.count) % s.count;
    *s.var = s.choices[j].id;
}

void options_menu(void) {
    int before[SETTINGS_COUNT];
    for (int i = 0; i < SETTINGS_COUNT; i++)
        before[i] = *settings[i].var;

    int line_start = OVERLAY_TOP + (OVERLAY_ROWS - SETTINGS_COUNT) / 2 - 1;
    int active = 0;
    bool redraw = true;
    for (;;) {
        if (redraw) {
            overlay_clear();
            overlay_frame("Options");
            overlay_hints(GLYPH_LEFT GLYPH_RIGHT " Change  " GLYPH_A GLYPH_B " Back");
            for (int i = 0; i < SETTINGS_COUNT; i++)
                draw_row(line_start + i, settings[i]);
            redraw = false;
        }
        int r = joy_choice(line_start, SETTINGS_COUNT, &active, OSD_KEY_CODE);
        if (r == 1 || r == 4 || r == 5)     // A, B, or the menu was closed and opened again
            break;
        if (r == 2 || r == 3) {
            step(settings[active], r == 2 ? 1 : -1);
            draw_row(line_start + active, settings[active]);
            menu_font_sync();
        }
        delay(20);
    }
    wait_joy_release(0x101);                // the main menu should not see the A/B press

    bool changed = false;
    for (int i = 0; i < SETTINGS_COUNT; i++)
        changed |= before[i] != *settings[i].var;
    if (changed && !settings_save()) {
        overlay_message("Settings not saved:\ncan't write tangcore.ini", 1);
    }
}

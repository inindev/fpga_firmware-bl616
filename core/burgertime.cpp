// BurgerTime arcade core: games are MRA files (core/arcade.cpp). The core takes the ROM
// image as the MRA builds it, and its settings as core_config:
//   [7:0] DIP1, [15:8] DIP2   MRA switch bytes 0 and 1 (the core inverts them as MiSTer)
//   [16] rotate               1: picture rotated (the Rotate screen setting)
// The in-game menu changes Rotate screen; it is sent at once (the core takes it at its
// next frame) and saved in tangcore.ini ([burgertime] rotate=).
#include <map>
#include <string>
#include <vector>

#include "cores.h"
#include "settings.h"
#include "utils.h"

#define CONFIG_ROTATE   (1u << 16)

// switch bytes of the game loaded last, sent again with each change of the rotate bit
static uint32_t switches;

static uint32_t burgertime_core_config(void) {
    return switches | (option_burgertime_rotate ? CONFIG_ROTATE : 0);
}

static bool burgertime_config(const mra_info &mra, const std::map<int, std::vector<uint8_t>> &roms,
                              uint32_t &config, std::string &err) {
    switches = (uint32_t)mra_switches(mra) & 0xffff;
    config = burgertime_core_config();
    return true;
}

// return 0 if successful
int loadburgertime(const char *fname) {
    return load_mra(fname, burgertime_config);
}

// In-game menu
#define ROW_ROTATE  12
#define ROW_MAIN    14

struct BurgertimeMenu: Menu {
    void render_rotate() {
        overlay_cursor(2, ROW_ROTATE);
        overlay_printf("Rotate screen: %-3s", option_burgertime_rotate ? "On" : "Off");
    }

    void render() override {
        overlay_clear();
        overlay_frame("Burger Time");
        render_rotate();
        overlay_cursor(2, ROW_MAIN);
        overlay_printf("<< Main Menu");
    }

    std::vector<int> get_options() override {
        return {ROW_ROTATE, ROW_MAIN};
    }

    bool on_choose(int idx) override {
        if (idx == 0) {
            option_burgertime_rotate = !option_burgertime_rotate;
            render_rotate();
            set_core_config(burgertime_core_config());
            if (!settings_save()) {
                overlay_message("Settings not saved:\ncan't write tangcore.ini", 1);
                do_redraw();
            }
            return false;       // stay in the menu
        }
        return true;            // << Main Menu
    }
};

Menu *create_burgertime_menu(const char *imgdir) {
    dprint("Creating BurgerTime menu\n");
    return new BurgertimeMenu();
}

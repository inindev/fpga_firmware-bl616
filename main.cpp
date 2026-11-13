/*
 * TangCore firmware for BL616 MCU
 *
 * (c) 2025, nand2mario <nand2mario@outlook.com>
 *
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree. 
 *
 */

#include <stdarg.h>
#include <string.h>
#include <vector>

extern "C" {
#include "board.h"
#include "bl616_glb.h"
#include "bflb_gpio.h"
#include "bflb_uart.h"
#include "bflb_clock.h"
#include "bl616_clock.h"

#include "usbh_core.h"
#include "usbh_msc.h"
#include "ff.h"
#include "fatfs_diskio_register.h"

// fatfs_diskio_register.h declares this only when the SD card driver is off
void fatfs_usbh_driver_register(struct usbh_msc *msc_class);
}

#include "file_chooser.h"
#include "programmer.h"
#include "gamepad.h"
#include "utils.h"
#include "cores.h"
#include "overlay.h"
#include "volume_bar.h"
#include "init.h"
#include "menu_manager.h"
#include "fw_version.h"
#include "settings.h"
#include "mra_image.h"

// Uncomment this to enable UART console (use with caution. it may interfere with MCU-FPGA communication)
#define UART_CONSOLE

/////////////////////////////////////////////////////////////////////////////////
// Global state

int option_osd_key = OPTION_OSD_KEY_SELECT_START;
int16_t active_core = -1;           // firmware detected this core as active
bool core_running;                  // a rom is loaded and running on the core
struct core_info *core;

// UART
struct bflb_device_s *gpio_dev;
struct bflb_device_s *uart0_dev;
struct bflb_device_s *uart1_dev;

// C++ registers global destructors against this; the SDK doesn't define it.
// The firmware never exits, so they never run (same as the SDK's C++ example).
void *__dso_handle = NULL;

// USB and fatfs
struct usbh_msc *msc;
const char *drv = "sd:";

// Tasks and shared state
TaskHandle_t main_task_handle;
TaskHandle_t uart1_rx_task_handle;

#ifdef TANG_CONSOLE60K
const char *BOARD_NAME = "console60k";
#elif defined(TANG_CONSOLE138K)
const char *BOARD_NAME = "console138k";
#elif defined(TANG_PRIMER25K)
const char *BOARD_NAME = "primer25k";
#elif defined(TANG_NANO20K)
const char *BOARD_NAME = "nano20k";
#else
const char *BOARD_NAME = "unknown";
#endif

// Override system printf() to send to FPGA
int __attribute__((weak)) putchar(int ch) {
    fpga_tx_header(0x05, 2);
    fpga_tx_byte(ch);
    return ch;
}


/////////////////////////////////////////////////////////////////////////////////
// Core loading and other file system operations

// nand2mario: these USB data structures cannot be CACHED as they are written to by hardware
USB_NOCACHE_RAM_SECTION FATFS fs;
USB_NOCACHE_RAM_SECTION FIL fcore;
USB_NOCACHE_RAM_SECTION BYTE __attribute__((aligned(64))) fbuf[BLOCK_SIZE];

FRESULT res_sd;
FileChooser file_chooser;

// #define PAGESIZE 22
// #define TOPLINE 2
// #define PWD_SIZE 1024
// char pwd[PWD_SIZE];
// one page of file names to display
// char file_names[PAGESIZE][256];
// int file_dir[PAGESIZE];         // this file is a directory
// int file_sizes[PAGESIZE];       
// int file_len;		            // number of files on this page

/////////////////////////////////////////////////////////////////////////////////
// Menu display and user interaction

// Menus for "NES", "SNES" ... entries
// dir: initial dir including the drive name (e.g. "sd:nes", "usb:cores")
// return 0: user chose a ROM (*choice), 1: no choice made, -1: error
// file chosen: pwd / file_name[*choice]
static int menu_loadrom(const char *dir) {
    string fname;
    file_chooser.rootdir = dir;
    file_chooser.curdir = dir;
    file_chooser.msg_return = "<< Return to main menu";
    bool r = file_chooser.choose_file(fname);
    if (!r) {
        overlay_status("No file chosen");
        return 1;
    }

    // now proceed to load the core and ROM
    joy1_state = 0; joy2_state = 0; // clear joypad states

    // load core if in cores/ dir
    if (fname.find(string(drv) + "cores") == 0) {
        overlay_status("Core: %s", fname.c_str());
        fpga_program(fname.c_str());
        menu_font_reset();              // the new core has the ROM font
        _overlay_on = 1;                // turn on overlay after core is loaded
        core_running = false;           // no rom on the new core yet
        return 0;       // return to main menu
    } 

    // find core info entry: an MRA file names its core (<rbf>), other files go by folder
    core_info *core = NULL;
    string path = fname.substr(fname.find(":")+1);
    if (path.size() > 4 && strcasecmp(path.c_str() + path.size() - 4, ".mra") == 0) {
        mra_info mra;
        string err;
        if (!mra_load(&fcore, fname.c_str(), mra, err)) {
            overlay_message(err.c_str(), 1);
            return -1;
        }
        core = find_core_by_rbf(mra.rbf.c_str());
        if (core == NULL) {
            overlay_message(("No core for this game:\n<rbf>" + mra.rbf + "</rbf>").c_str(), 1);
            return -1;
        }
        overlay_status("MRA for: %s", core->display_name);
    } else {
        for (int i = 0; i < core_info_list.size(); i++) {
            core_info *c = &core_info_list[i];
            if (!c->rbf && path.find(c->rom_dir) == 0) {
                overlay_status("ROM for: %s", c->display_name);
                core = c;
                break;
            }
        }
    }
    if (core == NULL) {
        if (prefix(ARCADE_DIR "/", path.c_str()))
            overlay_message("Arcade games are\n.mra files", 1);
        overlay_status("Core not found: %s", path.c_str());
        return -1;
    }

    // user chose a ROM file
    active_core = get_core_id();

    // load core if needed
    if (core != NULL) {
        if (active_core != core->id) {      // active core is not what we need
            string fname_core;
            if (find_core_for_board(fname_core, core->core_file)) {
                // load core
                fpga_program(fname_core.c_str());
                menu_font_reset();
                _overlay_on = 1;

                // allow 2 seconds for core to start
                uint64_t start = bflb_mtimer_get_time_ms();
                while (bflb_mtimer_get_time_ms() - start < 2000) {
                    send_blank_packet();
                    active_core = get_core_id();
                    if (active_core == core->id)
                        break;
                }
            } 
        }

        // Attemp to load ROM
        if (active_core == core->id) {
            overlay_status("Loading ROM: %s\n", fname.c_str());
            core->load_rom(fname.c_str());
            return 1;
        } else {
            overlay_status("Core failed to load\n");
            delay(1000);
            return -1;
        }
    }
    return -1;
}

// Master volume, in the HDMI encoder: 0 (mute) to 10 (0 dB), 3 dB per step. A core starts
// at 7 when it is loaded, so the step is sent again whenever control returns to the game.
#define VOLUME_MAX 10
static int volume_step = 7;
volatile int volume_request;        // steps asked for by the keyboard (USB task), applied here

static void set_volume(int step) {
    if (step < 0) step = 0;
    if (step > VOLUME_MAX) step = VOLUME_MAX;
    volume_step = step;
    taskENTER_CRITICAL();           // the USB task also sends (PS/2 scancodes)
    fpga_tx_header(0x0e, 2);
    fpga_tx_byte(step);
    taskEXIT_CRITICAL();
}

// TV style volume bar, on the overlay's strip over the running game (volume_bar.h).
// Hidden VOLUME_BAR_MS after the last change.
#define VOLUME_BAR_MS 1500

static void volume_bar(void) {
    uint8_t font[VOLUME_BAR_CHARS * 8];
    volume_bar_font(volume_step, VOLUME_MAX, font);
    overlay_strip(font);
}

#define OSD_HOLD_MS 400             // OSD key must be held this long to open the OSD in game

// Volume hotkeys on a pad: Select + L down, Select + R up (other buttons may be held too)
#define PAD_SELECT  0x004
#define PAD_L       0x400
#define PAD_R       0x800
static int pad_volume_dir(uint16_t p) {
    if ((p & (PAD_SELECT | PAD_L | PAD_R)) == (PAD_SELECT | PAD_L)) return -1;
    if ((p & (PAD_SELECT | PAD_L | PAD_R)) == (PAD_SELECT | PAD_R)) return 1;
    return 0;
}

static void send_hid(uint16_t hid1, uint16_t hid2) {
    fpga_tx_header(0x09, 5);
    fpga_tx_byte(hid1 >> 8);
    fpga_tx_byte(hid1 & 0xff);
    fpga_tx_byte(hid2 >> 8);
    fpga_tx_byte(hid2 & 0xff);
}

// keep sending HID state to core until the OSD key is held for OSD_HOLD_MS,
// or the keyboard turns the OSD on. Volume hotkeys act here, while the game runs.
static void send_hid_to_core(uint16_t osd_key) {
    uint16_t hid1_old = 0, hid2_old = 0;
    bool first = true;
    bool held = false;
    uint64_t held_since = 0;
    int pad_dir_old = 0;
    bool bar = false;           // volume bar shown
    uint64_t bar_since = 0;     // time of the last press that showed it
    dprint("Start sending HID to core...");
    set_volume(volume_step);    // a core loaded since the last game starts at its default
    taskENTER_CRITICAL();
    volume_request = 0;
    taskEXIT_CRITICAL();
    while (1) {
        uint16_t joy1=0, joy2=0, hid1=0, hid2=0;
        get_joypad_states(&joy1, &joy2, &hid1, &hid2);
        if (first || hid1 != hid1_old || hid2 != hid2_old) {    // send HID if changed
            send_hid(hid1, hid2);
            hid1_old = hid1;
            hid2_old = hid2;
            first = false;
        }

        // one volume step per press of Select + L / Select + R, on any pad
        int pad_dir = 0;
        for (uint16_t p : {joy1, joy2, hid1, hid2})
            if (pad_volume_dir(p)) pad_dir = pad_volume_dir(p);
        bool pressed = false;
        if (pad_dir && pad_dir != pad_dir_old) {
            set_volume(volume_step + pad_dir);
            pressed = true;
        }
        pad_dir_old = pad_dir;

        // Ctrl + - / Ctrl + = from the keyboard
        taskENTER_CRITICAL();
        int request = volume_request;
        volume_request = 0;
        taskEXIT_CRITICAL();
        if (request) {
            set_volume(volume_step + request);
            pressed = true;
        }

        // the bar, until VOLUME_BAR_MS after the last press
        uint64_t now_ms = bflb_mtimer_get_time_ms();
        if (pressed) {
            volume_bar();
            bar = true;
            bar_since = now_ms;
        } else if (bar && now_ms - bar_since >= VOLUME_BAR_MS) {
            overlay_strip_off();
            bar = false;
        }

        if (joy1 == osd_key || joy2 == osd_key || hid1 == osd_key || hid2 == osd_key) {
            uint64_t now = bflb_mtimer_get_time_ms();
            if (!held) {
                held = true;
                held_since = now;
            } else if (now - held_since >= OSD_HOLD_MS) {
                break;
            }
        } else {
            held = false;
        }
        if (overlay_on())      // turned on by keyboard
            break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    send_hid(0, 0);            // the core should not see buttons held while the OSD is up
    overlay_strip_off();        // the menu wins: no bar, and its row back as it was
    dprint("Stopped sending HID to core.");
}

// // (R L X A RT LT DN UP START SELECT Y B)
// Return: 1 button B pressed, 4: button A pressed, 2: next page, 3: previous page,
//         5: OSD was turned back on (caller should redraw)
// active is the entry chosen, shown with the highlight bar
int joy_choice(int start_line, int len, int *active, int overlay_key_code) {
    if (*active < 0 || *active >= len)
        *active = 0;

    if (!overlay_on()) {           // keep sending HID state to core when OSD is off
        send_hid_to_core(overlay_key_code);
        if (!overlay_on())         // not already turned on by keyboard
            overlay(1);
        dprint("OSD: ON");
        wait_joy_release(overlay_key_code);     // the held key should not act in the menu
        return 5;
    }

    uint16_t joy1=0, joy2=0, hid1=0, hid2=0;

    get_joypad_states(&joy1, &joy2, &hid1, &hid2);
    joy1 |= hid1;
    joy2 |= hid2;
    uint16_t press = joy_press(joy1 | joy2);

    if ((joy1 == overlay_key_code) || (joy2 == overlay_key_code)) {
        // no hold needed to close, but the core gets control only after release
        wait_joy_release(overlay_key_code);
        overlay(0);
        dprint("OSD: OFF");
        return 0;
    }

    // one action per press, as in FPGA-Companion: buttons win over left/right,
    // and those over up/down. A diagonal used to move the cursor and then
    // return before the old marker was erased.
    if (press & 0x1)
        return 1;      // button B pressed
    if (press & 0x100)
        return 4;      // button A pressed
    if (press & 0x80)
        return 2;      // next page
    if (press & 0x40)
        return 3;      // previous page
    if ((press & 0x30) == 0x10) {
        if (*active > 0) (*active)--;
    }
    if ((press & 0x30) == 0x20) {
        if (*active < len-1) (*active)++;
    }

    overlay_highlight(start_line + (*active));     // only sends anything when it moves

    // overlay_cursor(0, 27);
    // overlay_printf(" j1=%04x j2=%04x h1=%04x h2=%04x", joy1, joy2, hid1, hid2);
    return 0;
}

#define MAIN_TASK_STACK_SIZE  2048
#define MAIN_TASK_PRIORITY    3
#define UART1_RX_TASK_STACK_SIZE  512
#define UART1_RX_TASK_PRIORITY    3

// Receive joypad updates and other UART responses from the FPGA
static void uart1_rx_task(void *pvParameters)
{
    uint8_t buffer[5];
    uint8_t pos = 0;
    uint8_t type = 0;
    uint16_t len = 0;
    
    while (1) {
        if (bflb_uart_rxavailable(uart1_dev)) {
            uint8_t ch = bflb_uart_getchar(uart1_dev);
            
            if (pos == 0) {          // expecting 0xAA
                if (ch == 0xAA) 
                    pos++;
            } else if (pos == 1) {   // len msb
                len = (uint16_t)ch << 8;
                pos++;
            } else if (pos == 2) {   // len lsb
                len += ch;
                pos++;
            } else if (pos == 3) {   // command type
                type = ch;
                pos++;

            ////// pos >= 4 //////
            } else if (type == 1) {     // response to command 1 (get core ID)
                if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
                    core_id = ch;
                    xSemaphoreGive(state_mutex);
                }
                pos = 0;
            } else if (type == 2) {                 // config string
                // skip for now
                if (pos == len+2)
                    pos = 0;
                else
                    pos++;
            } else if (type == 3) {                 // periodic joypad state
                buffer[pos-4] = ch;
                // Complete packet received
                if (pos == 7) {
                    // Combine bytes into 16-bit values
                    uint16_t joy1 = (buffer[0] << 8) | buffer[1];
                    uint16_t joy2 = (buffer[2] << 8) | buffer[3];
                    
                    // Update global state with mutex protection
                    if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
                        joy1_state = joy1;
                        joy2_state = joy2;
                        xSemaphoreGive(state_mutex);
                    }
                    pos = 0; // Reset for next packet
                } else
                    pos++;
            } else if (type == 4) {              // floppy write
                if (pos < 6)
                    buffer[pos-4] = ch;
                else
                    fbuf[pos-6] = ch;
                if (pos == 6+511) {
                    uint16_t drive = buffer[0] >> 7;
                    uint16_t sector = (buffer[0] & 0x7f) << 8 | buffer[1];
                    if (floppy[drive]) {
                        UINT br;
                        f_lseek(&f_floppy[drive], sector * 512);
                        if (f_write(&f_floppy[drive], fbuf, 512, &br) != FR_OK) {
                            overlay_status("Failed to write floppy");
                        }
                    }
                    pos = 0;   // reset for next packet
                } else
                    pos++;
            } else if (type == 5) {              // floppy read
                buffer[pos-4] = ch;
                if (pos == 5) {
                    uint16_t drive = buffer[0] >> 7;
                    uint16_t sector = (buffer[0] & 0x7f) << 8 | buffer[1];
                    if (floppy[drive]) {
                        UINT br;
                        f_lseek(&f_floppy[drive], sector * 512);
                        if (f_read(&f_floppy[drive], fbuf, 512, &br) == FR_OK) {
                            fpga_tx_header(0x0a, br+1);
                            for (UINT i = 0; i < br; i++) {
                                fpga_tx_byte(fbuf[i]);
                            }
                        } else {
                            overlay_status("Failed to read floppy");
                        }
                    }
                    pos = 0;   // reset for next packet
                } else
                    pos++;

            } else {
                pos = 0; // Reset if we get out of sync
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Device Mode at power-up: without a USB device on the port (the board is plugged into a
// PC, which never pulls up D+/D-), restart as the USB debugger, so that the PC can program
// the FPGA. DEVICE_MODE: boards without an SD card, checked first. DEVICE_MODE_SD: boards
// with an SD card, checked only when no SD card mounts. Primer 25K: untested (FPGA-Companion
// restarts into the Partner firmware the same way on it).
#if defined(TANG_NANO20K)
#define DEVICE_MODE
#elif defined(TANG_CONSOLE60K) || defined(TANG_CONSOLE138K) || defined(TANG_PRIMER25K)
#define DEVICE_MODE_SD
#endif

#if defined(DEVICE_MODE) || defined(DEVICE_MODE_SD)
#define DEVICE_MODE_ANY
#define USB_CONNECT_WAIT_MS 4000    // max wait for a USB device to connect (a cold hub took 2.5 s)
#define USB_MOUNT_WAIT_MS   5000    // max wait for the USB drive to mount
static bool no_storage;             // no USB drive mounted: the menu says so
#ifdef DEVICE_MODE_SD
static uint64_t usb_host_start_ms;  // the USB connect window starts with the USB host
#endif

// ask the core for its ID without waiting: core_id becomes >= 0 when it answers
static void core_id_request(void) {
    if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
        core_id = -1;
        xSemaphoreGive(state_mutex);
    }
    fpga_tx_header(0x01, 1);
}

static bool core_answered(void) {
    bool r = false;
    if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
        r = core_id >= 0;
        xSemaphoreGive(state_mutex);
    }
    return r;
}

// a message in the empty menu frame: a title on `row`, then lines from two rows below it
static void frame_message(int row, const char *title, std::initializer_list<const char *> lines) {
    overlay(1);
    overlay_clear();
    overlay_frame();
    overlay_hints("");
    overlay_center(row, "%s", title);
    int y = row + 2;
    for (const char *line : lines)
        overlay_center(y++, "%s", line);
}

// Wait for a USB device until `start` + USB_CONNECT_WAIT_MS. Without one, show the Device
// Mode text and restart as the USB debugger; the monitor core keeps showing the text.
static void device_mode_check(uint64_t start, std::initializer_list<const char *> text) {
    uint64_t asked = 0;
    bool shown = false, connected = false;
    for (;;) {
        uint64_t now = bflb_mtimer_get_time_ms();
        if (usb_port_connected()) {
            connected = true;
            break;
        }
        if (now - start >= USB_CONNECT_WAIT_MS)
            break;
        // the core configures itself from flash at power-on: show the message once it answers
        if (!shown) {
            if (core_answered()) {
                frame_message(OVERLAY_TOP + 7, "Detecting USB...", {});
                shown = true;
            } else if (now - asked >= 100) {
                send_blank_packet();
                core_id_request();
                asked = now;
            }
        }
        delay(10);
    }
    if (connected) {
        if (shown)
            overlay_clear();
        return;
    }
    overlay_status("No USB device: device mode");

    start = bflb_mtimer_get_time_ms();
    while (!shown && !core_answered() && bflb_mtimer_get_time_ms() - start < 3000) {
        send_blank_packet();
        core_id_request();
        delay(100);
    }
    frame_message(OVERLAY_TOP + 6, "Device Mode", text);
    delay(100);
    restart_as_usb_debugger();
}
#endif

// Display main menu and call other menu functions
static void main_task(void *pvParameters)
{
    uint32_t last_redraw_time = 0;
    // volatile uint32_t *reg_gpio0 = (volatile uint32_t *)0x200008c4;     // bl616 reference 4.8.5
    // volatile uint32_t *reg_gpio1 = (volatile uint32_t *)0x200008c8;
    // volatile uint32_t *reg_gpio2 = (volatile uint32_t *)0x200008cc;
    // volatile uint32_t *reg_gpio3 = (volatile uint32_t *)0x200008d0;

#ifdef DEVICE_MODE
    device_mode_check(bflb_mtimer_get_time_ms(), {"Connect USB storage and", "power cycle for Game Mode"});
#endif

    // wait for drive to be ready
    uint64_t start = bflb_mtimer_get_time_ms();
    FRESULT res = FR_NOT_READY;
#ifndef TANG_NANO20K
    // Nano 20K: no SD card on the BL616, go straight to USB
    overlay_status("Mounting sd card...", drv);
    while ((res = f_mount(&fs, "sd:", 1)) != FR_OK && bflb_mtimer_get_time_ms() - start < 500)
        delay(100);
#endif

    if (res == FR_OK) {
        overlay_status("SD card mounted in %d ms", bflb_mtimer_get_time_ms() - start);
    } else  {
#ifdef DEVICE_MODE_SD
        device_mode_check(usb_host_start_ms, {"Insert SD card or connect", "USB storage and power",
                                              "cycle for Game Mode"});
#endif
        overlay_status("SD not found. Mounting USB...");
        drv = "usb:";
        start = bflb_mtimer_get_time_ms();
#ifdef DEVICE_MODE_ANY
        while ((res = f_mount(&fs, "usb:", 1)) != FR_OK && bflb_mtimer_get_time_ms() - start < USB_MOUNT_WAIT_MS)
            delay(100);
        no_storage = res != FR_OK;
#else
        while ((res = f_mount(&fs, "usb:", 1)) != FR_OK && bflb_mtimer_get_time_ms() - start < 2000)
            delay(100);
#endif
        if (res != FR_OK) {
            overlay_status("Failed to mount USB drive");
        } else {
            overlay_status("USB drive mounted in %d ms", bflb_mtimer_get_time_ms() - start);
        }
    }
    if (res == FR_OK)
        settings_load();                // tangcore.ini; without it, the defaults

    int menu_cnt = main_menu_config.size();

    while (1) {
        bool redraw = true;
        int choice = 0;
        int resume = core_running ? 1 : 0;      // "Resume game" row above the menu items
        int rows = resume + menu_cnt;
        int line_start = OVERLAY_TOP + (OVERLAY_ROWS - rows) / 2 - 1;  // one row above center

        // menu item names; the list is centered on its widest item
        std::vector<const char *> items;
        if (resume)
            items.push_back("Resume game");
        for (int i = 0; i < menu_cnt; i++) {
            const char *name = "";
            if (main_menu_config[i] > 0) {
                for (int j = 0; core_info_list[j].id != 0; j++) {
                    if (core_info_list[j].id == main_menu_config[i]) {
                        name = core_info_list[j].display_name;
                        break;
                    }
                }
            } else if (main_menu_config[i] == -1) {
                name = "Cores";
            } else if (main_menu_config[i] == -2) {
                name = "Options";
            }
            items.push_back(name);
        }
        int width = 0;
        for (const char *name : items)
            if ((int)strlen(name) > width) width = strlen(name);
        int col = (32 - width) / 2;
        if (col < 2) col = 2;

        for (;;) {
            uint32_t now = bflb_mtimer_get_time_ms();
            if (active_core == -1) {
                // send_blank_packet();
                active_core = get_core_id();            // 200ms timeout
                overlay_status("core_id=%d", active_core);
                if (active_core >= 0) redraw = true;    // redraw immediately if core is detected
            }
            // if (core < 0) continue;         // do not draw or process input if core is not ready
            if (now - last_redraw_time > 5000) {
                // a core loaded over JTAG gets a full redraw; otherwise send the screen again
                // as it is, which restores it without the blink of a clear
                if (get_core_id() != active_core) {
                    redraw = true;
                } else {
                    overlay(overlay_on());              // set correct overlay state
                    overlay_refresh();
                    last_redraw_time = now;
                }
            }
            if (redraw) {
                int id = get_core_id();                 // allow jtag to change core underneath us
                if (id != active_core)
                    menu_font_reset();                  // a core loaded over JTAG has the ROM font
                active_core = id;
                if (active_core >= 0)
                    menu_font_sync();                   // the font chosen in Options
                overlay(overlay_on());                  // set correct overlay state
                overlay_clear();

                // the header (rows 0-4) is the core's logo, drawn by the FPGA
                overlay_frame();
#ifdef DEVICE_MODE_ANY
                overlay_hints(no_storage ? "No USB storage found" : GLYPH_B " Select");
#else
                overlay_hints(GLYPH_B " Select");
#endif
                for (int i = 0; i < rows; i++) {
                    overlay_cursor(col, line_start + i);
                    overlay_printf("%s", items[i]);
                }
                overlay_center(OVERLAY_FOOTER, "%s", FW_VERSION);
                last_redraw_time = now;
                redraw = false;

                // print some debug stats to UART
                // uint16_t joy1=0, joy2=0;
                // get_joypad_states(&joy1, &joy2);
                // overlay_status("core=%d, j1=%04x, j2=%04x", active_core, joy1, joy2);
                // overlay_status("Mtimer frequency: %d MHz", bflb_mtimer_get_freq() / 1000000);
                // overlay_status("CPU frequency: %d MHz", bflb_clk_get_system_clock(BL_SYSTEM_CLOCK_MCU_CLK) / 1000000);
                // overlay_status("GPIO0-3 status: %08x %08x %08x %08x", *reg_gpio0, *reg_gpio1, *reg_gpio2, *reg_gpio3);
            }

            int r = joy_choice(line_start, rows, &choice, OSD_KEY_CODE);
            if (r == 1) break;

            if (r == 5) {
                // overlay is turned back on: show the main menu, cursor on "Resume game"
                choice = 0;
                redraw = true;
                // cores with their own pop-up menu: PC/XT (floppy images), BurgerTime and Galaga (rotation)
                core_info *core = find_core_by_id(active_core);
                if (core != NULL && core->create_menu != create_default_menu) {
                    DEBUG("Displaying pop-up menu\n");
                    menu_clear();
                    Menu *menu = core->create_menu(std::string(drv).append(core->rom_dir).c_str());
                    std::unique_ptr<Menu> menu_ptr(menu);
                    push_menu(std::move(menu_ptr));
                    menu->do_redraw();
                    menu_input_loop();
                }
            }

            delay(20);
        }

        if (resume && choice == 0) {
            wait_joy_release(0x1);      // the game should not see the B press
            overlay(0);
            continue;
        }

        int item = main_menu_config[choice - resume];
        if (item > 0) {
            // Load rom or core from USB drive
            struct core_info *core = NULL;
            for (int i = 0; core_info_list[i].id != 0; i++) {
                if (core_info_list[i].id == item) {
                    core = &core_info_list[i];
                    break;
                }
            }
            if (core) {
                std::string dir = std::string(drv).append(core->rom_dir);
                menu_loadrom(dir.c_str());
            }
        } else if (item == -1) {
            // load cores manually
            std::string dir = std::string(drv).append("cores");
            menu_loadrom(dir.c_str());
        } else if (item == -2) {
            // Options
            options_menu();
        } 

        delay(300);
    }
}

static void print_system_info(void) {
    // this is viewable with scripts/liveuart.py
    overlay_status("TangCore %s", FW_VERSION);
    overlay_status("TangBoard: %s", BOARD_NAME);
    overlay_status("System clock: %u MHz", bflb_clk_get_system_clock(BL_SYSTEM_CLOCK_MCU_CLK) / 1000000);
    // UART registers
    // Clock comes from XCLK/160M/BCLK and goes through a divider and becomes UART_CLK
    overlay_status("GLB_UART_CFG0: %08x", BL_RD_WORD(0x20000150));
    overlay_status("GLB_UART_CFG1: %08x", BL_RD_WORD(0x20000154));
    overlay_status("GLB_UART_CFG2: %08x", BL_RD_WORD(0x20000158));
    overlay_status("UART0 clock: %u", Clock_Peripheral_Clock_Get(BL_PERIPHERAL_CLOCK_UART0));
    overlay_status("UART1 clock: %u", Clock_Peripheral_Clock_Get(BL_PERIPHERAL_CLOCK_UART1));

    // 10.3.5: baudrate = UART_clk / (uart_prd + 1)
    // This causes memory exception.
    // overlay_status("UART_BIT_PRD: %08x", BL_RD_WORD(0x40010008));
    //              01234567890123456789012345678901
    // overlay_status("                                ");
}

// USB drive: CherryUSB calls usbh_msc_run() when one is connected. SCSI init
// blocks, so it runs in a short-lived task, then the drive is registered as "usb:".
static TaskHandle_t usbh_msc_task;

static void usbh_msc_thread(void *arg)
{
    struct usbh_msc *msc_class = (struct usbh_msc *)arg;
    if (usbh_msc_scsi_init(msc_class) < 0) {
        overlay_status("USB drive init failed");
    } else {
        fatfs_usbh_driver_register(msc_class);
        msc = msc_class;
    }
    taskENTER_CRITICAL();
    usbh_msc_task = NULL;
    taskEXIT_CRITICAL();
    vTaskDelete(NULL);
}

extern "C" void usbh_msc_run(struct usbh_msc *msc_class)
{
    xTaskCreate(usbh_msc_thread, "usbh_msc", 1024, msc_class, CONFIG_USBHOST_PSC_PRIO - 1, &usbh_msc_task);
}

// Suspend the tasks that can use the USB host: the menu (USB drive reads) and USB drive
// setup. The calling task keeps running.
void usb_host_tasks_stop(void)
{
    taskENTER_CRITICAL();
    if (main_task_handle && main_task_handle != xTaskGetCurrentTaskHandle())
        vTaskSuspend(main_task_handle);
    if (usbh_msc_task)
        vTaskSuspend(usbh_msc_task);
    taskEXIT_CRITICAL();
    usb_gamepad_stop();
}

extern "C" void usbh_msc_stop(struct usbh_msc *msc_class)
{
    if (msc == msc_class)
        msc = NULL;
}

// Initialize things, then start main_task and uart1_rx_task to do actual work
int main(void)
{
    /* Board init */
    board_init();
    init_core_list();

    // Initialize GPIO and UART
    init_gpio_and_uart();

    print_system_info();

    // Create mutex for joypad states
    state_mutex = xSemaphoreCreateMutex();

#ifndef TANG_NANO20K
    overlay_status("Initializing SDH...");
    fatfs_sdh_driver_register();        // calls SDH_Init()
#endif
    // f_mount(&fs_sd, "sd:", 0);          // registers SDMMC drive 

    // Initializing USB host...
    overlay_status("Initializing USB host...");
    // USB drives are registered in usbh_msc_run() when they connect
    struct bflb_device_s *usb_dev = bflb_device_get_by_name(BFLB_NAME_USB_V2);
    usbh_initialize(0, usb_dev->reg_base, NULL);
#ifdef DEVICE_MODE_SD
    usb_host_start_ms = bflb_mtimer_get_time_ms();
#endif
    usb_gamepad_init();

    overlay_status("Creating tasks...");
    // Create the tasks
    xTaskCreate(main_task, "main_task", MAIN_TASK_STACK_SIZE, NULL, MAIN_TASK_PRIORITY, &main_task_handle);
    xTaskCreate(uart1_rx_task, "uart1_rx_task", UART1_RX_TASK_STACK_SIZE, NULL, UART1_RX_TASK_PRIORITY, &uart1_rx_task_handle);
    
    vTaskStartScheduler();

    while (1) {
    }
}

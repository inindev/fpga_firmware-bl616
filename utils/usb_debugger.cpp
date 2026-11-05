// USB port connect state, and the restart that makes Sipeed's Partner firmware come up as
// the USB debugger. The restart follows FPGA-Companion's mcu_hw_reset() (src/bl616/mcu_hw.c).

#include "utils.h"

extern "C" {
#include "bflb_mtimer.h"
#include "bflb_wdg.h"
#include "bflb_irq.h"
#include "bl616_hbn.h"
#include "usbh_core.h"
#include "usbh_hub.h"

void hid_keyboard_init(uint8_t busid, uintptr_t reg_base);     // usb/hid_keyboard_template.c
}

// Port status of the root hub's port 1, the same request CherryUSB's hub thread makes;
// the EHCI driver answers it from the port status register.
bool usb_port_connected(void) {
    struct usb_setup_packet setup;
    uint32_t status = 0;
    setup.bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_OTHER;
    setup.bRequest = HUB_REQUEST_GET_STATUS;
    setup.wValue = 0;
    setup.wIndex = 1;
    setup.wLength = 4;
    if (usbh_roothub_control(&g_usbhost_bus[0], &setup, (uint8_t *)&status) < 0)
        return false;
    return status & HUB_PORT_STATUS_CONNECTION;
}

void restart_as_usb_debugger(void) {
    // watchdog reset in about 1.5 s (32 kHz clock divided by 32, 1500 counts)
    struct bflb_device_s *wdg = bflb_device_get_by_name("watchdog0");
    struct bflb_wdg_config_s wdg_cfg;
    wdg_cfg.clock_source = WDG_CLKSRC_32K;
    wdg_cfg.clock_div = 31;
    wdg_cfg.comp_val = 1500;
    wdg_cfg.mode = WDG_MODE_RESET;
    bflb_wdg_stop(wdg);
    bflb_wdg_init(wdg, &wdg_cfg);
    bflb_wdg_start(wdg);

    usb_host_tasks_stop();
    bflb_mtimer_delay_ms(500);

    bflb_irq_disable(gpio_dev->irq_num);
    bflb_gpio_deinit(gpio_dev, GPIO_PIN_2);

    // stop the USB host, then start a USB device (a HID keyboard) on the same port
    usbh_deinitialize(0);
    bflb_mtimer_delay_ms(20);
    struct bflb_device_s *usb_dev = bflb_device_get_by_name(BFLB_NAME_USB_V2);
    hid_keyboard_init(0, usb_dev->reg_base);

    HBN_Set_User_Boot_Config(0);    // the next boot follows the boot pin

#ifdef TANG_PRIMER25K
    // release the BL616's pins to the dock before the reset, as FPGA-Companion does
    bflb_mtimer_delay_ms(250);
    static const uint8_t pins[] = {0, 1, 2, 3, 10, 11, 12, 13, 14, 15, 16, 17, 20, 21, 22, 27,
                                   28, 29, 30};
    for (uint8_t pin : pins)
        bflb_gpio_deinit(gpio_dev, pin);
#endif

    while (1) {
        // wait for the watchdog reset
    }
}

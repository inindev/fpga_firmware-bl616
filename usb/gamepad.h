#pragma once

#include "FreeRTOS.h"
#include "semphr.h"
#include "usbh_core.h"
#include "usbh_hid.h"

// Start USB gamepad tasks
void usb_gamepad_init(void);
// Suspend the USB gamepad tasks, so that they no longer use the USB host
void usb_gamepad_stop(void);

// state will be written here by the USB gamepad task
extern volatile uint16_t hid1_state;               // SNES-format gamepad state
extern volatile uint16_t hid2_state;               // SNES-format gamepad state
extern volatile uint16_t kbd_joy_state;            // SNES-format state from keyboards, ORed into hid1
extern SemaphoreHandle_t state_mutex;              // for all global state access
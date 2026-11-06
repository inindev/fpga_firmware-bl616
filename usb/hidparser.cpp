// HID report descriptor and report parser
// Mostly based on FPGA-Companion by Till Harbaum
//
// http://www.frank-zhao.com/cache/hid_tutorial_1.php

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hidparser.h"
#include "usb2ps2.h"
#include "utils.h"
#include "overlay.h"

#define hidp_debugf(fmt, ...) do {} while(0)
// #define DEBUG(fmt, ...) printf(fmt, ##__VA_ARGS__)

#if 0
#define hidp_extreme_debugf(...) hidp_debugf(__VA_ARGS__)
#else
#define hidp_extreme_debugf(...)
#endif

typedef struct {
  uint8_t bSize: 2;
  uint8_t bType: 2;
  uint8_t bTag: 4;
} __attribute__((packed)) item_t;

// flags for joystick components required
#define JOY_MOUSE_REQ_AXIS_X  0x01
#define JOY_MOUSE_REQ_AXIS_Y  0x02
#define JOY_MOUSE_REQ_BTN_0   0x04
#define JOY_MOUSE_REQ_BTN_1   0x08
#define JOYSTICK_COMPLETE     (JOY_MOUSE_REQ_AXIS_X | JOY_MOUSE_REQ_AXIS_Y | JOY_MOUSE_REQ_BTN_0)
#define MOUSE_COMPLETE        (JOY_MOUSE_REQ_AXIS_X | JOY_MOUSE_REQ_AXIS_Y | JOY_MOUSE_REQ_BTN_0 | JOY_MOUSE_REQ_BTN_1)

#define USAGE_PAGE_GENERIC_DESKTOP  1
#define USAGE_PAGE_SIMULATION       2
#define USAGE_PAGE_VR               3
#define USAGE_PAGE_SPORT            4
#define USAGE_PAGE_GAMING           5
#define USAGE_PAGE_GENERIC_DEVICE   6
#define USAGE_PAGE_KEYBOARD         7
#define USAGE_PAGE_LEDS             8
#define USAGE_PAGE_BUTTON           9
#define USAGE_PAGE_ORDINAL         10
#define USAGE_PAGE_TELEPHONY       11
#define USAGE_PAGE_CONSUMER        12


#define USAGE_POINTER   1
#define USAGE_MOUSE     2
#define USAGE_JOYSTICK  4
#define USAGE_GAMEPAD   5
#define USAGE_KEYBOARD  6
#define USAGE_KEYPAD    7
#define USAGE_MULTIAXIS 8

#define USAGE_X       48
#define USAGE_Y       49
#define USAGE_Z       50
#define USAGE_RX      51
#define USAGE_RY      52
#define USAGE_RZ      53
#define USAGE_WHEEL   56
#define USAGE_HAT     57

// check if the current report 
bool report_is_usable(uint16_t bit_count, uint8_t report_complete, hid_report_t *conf) {
	hidp_debugf("  - total bit count: %d (%d bytes, %d bits)", 
	      bit_count, bit_count/8, bit_count%8);

	conf->report_size = bit_count/8;

	// check if something useful was detected
	if( ((conf->type == REPORT_TYPE_JOYSTICK) && ((report_complete & JOYSTICK_COMPLETE) == JOYSTICK_COMPLETE)) ||
	    ((conf->type == REPORT_TYPE_MOUSE)    && ((report_complete & MOUSE_COMPLETE) == MOUSE_COMPLETE)) ||
	    ((conf->type == REPORT_TYPE_KEYBOARD))) {
		hidp_debugf("  - report %d is usable", conf->report_id);
		return true;
	}

	hidp_debugf("  - unusable report %d", conf->report_id);
	return false;
}

// remember where a keyboard field sits in the report
static void add_kbd_field(hid_report_t *conf, uint16_t offset, uint8_t size, uint8_t count,
			  bool variable, uint16_t usage_min, uint16_t usage_max, uint16_t logical_min) {
	if(conf->keyboard.num_fields >= MAX_KBD_FIELDS) {
		hidp_debugf("KEYBOARD field @ %u dropped, more than %d fields", offset, MAX_KBD_FIELDS);
		return;
	}
	hid_kbd_field_t *f = &conf->keyboard.field[conf->keyboard.num_fields++];
	f->offset = offset;
	f->size = size;
	f->count = count;
	f->variable = variable;
	f->usage_min = usage_min;
	f->usage_max = usage_max;
	f->logical_min = logical_min;
	hidp_debugf("KEYBOARD %s @ %u, size %u, count %u, usage %u-%u",
		variable ? "bits" : "array", offset, size, count, usage_min, usage_max);
}

bool parse_report_descriptor(const uint8_t *rep, uint16_t rep_size, hid_report_t *conf, uint16_t *rbytes) {
	int8_t app_collection = 0;
	int8_t phys_log_collection = 0;
	uint8_t skip_collection = 0;
	int8_t generic_desktop = -1;   // depth at which first gen_desk was found
	uint8_t collection_depth = 0;

	uint8_t i;

	//
	uint8_t buttons = 0;
	uint8_t report_size = 0, report_count = 0;
	uint16_t bit_count = 0, usage_count = 0;
	uint16_t logical_minimum=0, logical_maximum=0;
	uint16_t physical_minimum=0, physical_maximum=0;
	uint16_t usage_page = 0;
	// local usages, used to locate keyboard fields
	uint16_t usage_min = 0, usage_max = 0;
	uint16_t usage_list[MAX_KBD_USAGES];
	uint8_t usage_listed = 0;      // number of entries in usage_list
	bool usage_range = false;
	memset(conf, 0, sizeof(hid_report_t));

	// mask used to check of all required components have been found, so
	// that e.g. both axes and the button of a joystick are ready to be used
	uint8_t report_complete = 0;

	// joystick/mouse components
	int8_t axis[MAX_AXES];
	uint8_t btns = 0;
	int8_t hat = -1;

	for (i=0; i<MAX_AXES; i++) axis[i] = -1;

	conf->type = REPORT_TYPE_NONE;

	while(rep_size) {
		// extract short item
		uint8_t tag = ((item_t*)rep)->bTag;
		uint8_t type = ((item_t*)rep)->bType;
		uint8_t size = ((item_t*)rep)->bSize;

		rep++;
		rep_size--;   // one byte consumed
		if(rbytes) (*rbytes)++;
		
		uint32_t value = 0;
		if(size) {      // size 1/2/3
			value = *rep++;
			rep_size--;
			if(rbytes) (*rbytes)++;
		}

		if(size > 1) {  // size 2/3
			value = (value & 0xff) + ((uint32_t)(*rep++)<<8);
			rep_size--;
			if(rbytes) (*rbytes)++;
		}

		if(size > 2) {  // size 3
			value &= 0xffff;
			value |= ((uint32_t)(*rep++)<<16);
			value |= ((uint32_t)(*rep++)<<24);
			rep_size-=2;
			if(rbytes) (*rbytes) += 2;
		}

		//    hidp_extreme_debugf("Value = %d (%u)\n", value, value);

		// we are currently skipping an unknown/unsupported collection) 
		if(skip_collection) {
			if(!type) {  // main item
				// any new collection increases the depth of collections to skip
				if(tag == 10) {
					skip_collection++;
					collection_depth++;
				}

				// any end collection decreases it
				if(tag == 12) {
					skip_collection--;
					collection_depth--;

					// leaving the depth the generic desktop was valid for
					if(generic_desktop > collection_depth)
						generic_desktop = -1;
				}
			}

		} else {
			//      hidp_extreme_debugf("-> Item tag=%d type=%d size=%d", tag, type, size);

			switch(type) {
			case 0:
			// main item

				switch(tag) {
				case 8:
					// handle found buttons 
					if(btns) {
						if((conf->type == REPORT_TYPE_JOYSTICK) ||
						   (conf->type == REPORT_TYPE_MOUSE)) {
						// scan for up to four buttons
							char b;
							for(b=0;b<12;b++) {
								if(report_count > buttons) {
								uint16_t this_bit = bit_count+b;

									hidp_debugf("BUTTON%d @ %d (byte %d, mask %d)", buttons, 
										this_bit, this_bit/8, 1 << (this_bit%8));

									conf->joystick_mouse.button[buttons].byte_offset = this_bit/8;
									conf->joystick_mouse.button[buttons].bitmask = 1 << (this_bit%8);
									buttons++;
								}
							}

							// we found at least one button which is all we want to accept this as a valid 
							// joystick
							report_complete |= JOY_MOUSE_REQ_BTN_0;
							if(report_count > 1) report_complete |= JOY_MOUSE_REQ_BTN_1;
						}
					}

					// handle found axes
					for(int c=0;c<MAX_AXES;c++) {
						if(axis[c] >= 0) {
							uint16_t cnt = bit_count + report_size * axis[c];
							hidp_debugf("  (%c-AXIS @ %d (byte %d, bit %d))", 'X'+c,
							  cnt, cnt/8, cnt&7);

							if((conf->type == REPORT_TYPE_JOYSTICK) || (conf->type == REPORT_TYPE_MOUSE)) {
								// save in joystick report
								conf->joystick_mouse.axis[c].offset = cnt;
								conf->joystick_mouse.axis[c].size = report_size;
								conf->joystick_mouse.axis[c].logical.min = logical_minimum;
								conf->joystick_mouse.axis[c].logical.max = logical_maximum;
								if(c==0) report_complete |= JOY_MOUSE_REQ_AXIS_X;
								if(c==1) report_complete |= JOY_MOUSE_REQ_AXIS_Y;
							}
						}
					}

					// handle found hat
					if(hat >= 0) {
						uint16_t cnt = bit_count + report_size * hat;
						hidp_debugf("  (HAT @ %d (byte %d, bit %d), size %d)",
						  cnt, cnt/8, cnt&7, report_size);
						if(conf->type == REPORT_TYPE_JOYSTICK) {
							conf->joystick_mouse.hat.offset = cnt;
							conf->joystick_mouse.hat.size = report_size;
							conf->joystick_mouse.hat.logical.min = logical_minimum;
							conf->joystick_mouse.hat.logical.max = logical_maximum;
							conf->joystick_mouse.hat.physical.min = physical_minimum;
							conf->joystick_mouse.hat.physical.max = physical_maximum;
						}
					}

					hidp_extreme_debugf("INPUT(%lu)", value);

					// keyboard: remember where the key data sits in the report
					// (skip constant fields such as the boot report's reserved byte)
					if(conf->type == REPORT_TYPE_KEYBOARD && usage_page == USAGE_PAGE_KEYBOARD &&
					   !(value & 0x01) && (usage_range || usage_listed)) {
						bool variable = (value & 0x02) != 0;
						if(usage_range) {
							add_kbd_field(conf, bit_count, report_size, report_count, variable,
								      usage_min, usage_max, logical_minimum);
						} else if(variable) {
							// one field per run of consecutive usages; elements past
							// the list use its last usage, so the last run takes them
							uint8_t start = 0;
							for(uint8_t j = 1; j <= usage_listed && start < report_count; j++) {
								if(j < usage_listed && usage_list[j] == usage_list[j-1] + 1)
									continue;
								uint8_t count = (j < usage_listed) ? j - start : report_count - start;
								if(start + count > report_count) count = report_count - start;
								add_kbd_field(conf, bit_count + start * report_size, report_size, count,
									      true, usage_list[start], usage_list[j-1], logical_minimum);
								start = j;
							}
						} else {
							// array: values index the usage list, which must be consecutive
							bool consecutive = true;
							for(uint8_t j = 1; j < usage_listed; j++)
								if(usage_list[j] != usage_list[0] + j) consecutive = false;
							if(consecutive)
								add_kbd_field(conf, bit_count, report_size, report_count, false,
									      usage_list[0], usage_list[usage_listed-1], logical_minimum);
							else
								hidp_debugf("KEYBOARD array with scattered usages ignored");
						}
					}

					// reset for next inputs
					bit_count += report_count * report_size;
					usage_count = 0;
					btns = 0;
					for (i=0; i<MAX_AXES; i++) axis[i] = -1;
					hat = -1;
					break;

				case 9:
					hidp_extreme_debugf("OUTPUT(%lu)", value);
					break;

				case 11:
					hidp_extreme_debugf("FEATURE(%lu)", value);
					break;

				case 10:
					hidp_extreme_debugf("COLLECTION(%lu)", value);
					collection_depth++;
					usage_count = 0;

					if(value == 1) {   // app collection
						hidp_extreme_debugf("  -> application");
						app_collection++;
					} else if(value == 0) {  // physical collection
						hidp_extreme_debugf("  -> physical");
						phys_log_collection++;
					} else if(value == 2) {  // logical collection
						hidp_extreme_debugf("  -> logical");
						phys_log_collection++;
					} else {
						hidp_extreme_debugf("skipping unsupported collection");
						skip_collection++;
					}
					break;

				case 12:
					hidp_extreme_debugf("END_COLLECTION(%lu)", value);
					collection_depth--;

					// leaving the depth the generic desktop was valid for
					if(generic_desktop > collection_depth)
						generic_desktop = -1;

					if(phys_log_collection) {
						hidp_extreme_debugf("  -> phys/log end");
						phys_log_collection--;
					} else if(app_collection) {
						hidp_extreme_debugf("  -> app end");
						app_collection--;

						// check if report is usable and stop parsing if it is
						if(report_is_usable(bit_count, report_complete, conf)) {
						        return true;
						} else {
							// retry with next report
							bit_count = 0;
							report_complete = 0;
							conf->keyboard.num_fields = 0;
						}

					} else {
						hidp_debugf(" -> unexpected");
						return false;
					}
					break;

				default:
					hidp_debugf("unexpected main item %d", tag);
					return false;
					break;
				}
				// local items only apply to the next main item
				usage_range = false;
				usage_listed = 0;
				break;

			case 1:
				// global item
				switch(tag) {
				case 0:
					hidp_extreme_debugf("USAGE_PAGE(%lu/0x%lx)", value, value);
					usage_page = value;

					if(value == USAGE_PAGE_KEYBOARD) {
						hidp_extreme_debugf(" -> Keyboard");
					} else if(value == USAGE_PAGE_GAMING) {
						hidp_extreme_debugf(" -> Game device");
					} else if(value == USAGE_PAGE_LEDS) {
						hidp_extreme_debugf(" -> LEDs");
					} else if(value == USAGE_PAGE_CONSUMER) {
						hidp_extreme_debugf(" -> Consumer");
					} else if(value == USAGE_PAGE_BUTTON) {
						hidp_extreme_debugf(" -> Buttons");
						btns = 1;
					} else if(value == USAGE_PAGE_GENERIC_DESKTOP) {
						hidp_extreme_debugf(" -> Generic Desktop");

						if(generic_desktop < 0)
							generic_desktop = collection_depth;
					} else
						hidp_extreme_debugf(" -> UNSUPPORTED USAGE_PAGE");

					break;

				case 1:
					hidp_extreme_debugf("LOGICAL_MINIMUM(%lu/%d)", value, (int16_t)value);
					logical_minimum = value;
					break;

				case 2:
					hidp_extreme_debugf("LOGICAL_MAXIMUM(%lu)", value);
					logical_maximum = value;
					break;

				case 3:
					hidp_extreme_debugf("PHYSICAL_MINIMUM(%lu/%d)", value, (int16_t)value);
					physical_minimum = value;
					break;

				case 4:
					hidp_extreme_debugf("PHYSICAL_MAXIMUM(%lu)", value);
					physical_maximum = value;
					break;

				case 5:
					hidp_extreme_debugf("UNIT_EXPONENT(%lu)", value);
					break;

				case 6:
					hidp_extreme_debugf("UNIT(%lu)", value);
					break;

				case 7:
					hidp_extreme_debugf("REPORT_SIZE(%lu)", value);
					report_size = value;
					break;

				case 8:
					hidp_extreme_debugf("REPORT_ID(%lu)", value);
					conf->report_id_present = 1;
					conf->report_id = value;
					break;

				case 9:
					hidp_extreme_debugf("REPORT_COUNT(%lu)", value);
					report_count = value;
					break;

				default:
					hidp_debugf("unexpected global item %d", tag);
					return false;
					break;
				}
				break;

			case 2:
				// local item
				switch(tag) {
				case 0:
					// we only support mice, keyboards and joysticks
					hidp_extreme_debugf("USAGE(%lu/0x%lx)", value, value);

					if( !collection_depth && (value == USAGE_KEYBOARD)) {
						// usage(keyboard) is always allowed
						hidp_debugf(" -> Keyboard");
						conf->type = REPORT_TYPE_KEYBOARD;
					} else if(!collection_depth && (value == USAGE_MOUSE)) {
						// usage(mouse) is always allowed
						hidp_debugf(" -> Mouse");
						conf->type = REPORT_TYPE_MOUSE;
					} else if(!collection_depth && 
						((value == USAGE_GAMEPAD) || (value == USAGE_JOYSTICK))) {
							hidp_extreme_debugf(" -> Gamepad/Joystick");
							hidp_debugf("Gamepad/Joystick usage found");
							conf->type = REPORT_TYPE_JOYSTICK;
					} else if(value == USAGE_POINTER && app_collection) {
						// usage(pointer) is allowed within the application collection

						hidp_debugf(" -> Pointer");

					} else if(((value >= USAGE_X && value <= USAGE_RZ) || value == USAGE_WHEEL) && app_collection) {
						// usage(x) and usage(y) are allowed within the app collection
						hidp_extreme_debugf(" -> axis usage");

						// we support x and y axis on mice and joysticks (+wheel on mice)
						if((conf->type == REPORT_TYPE_JOYSTICK) || (conf->type == REPORT_TYPE_MOUSE)) {
							if(value == USAGE_X) {
								hidp_extreme_debugf("JOYSTICK/MOUSE: found x axis @ %d", usage_count);
								axis[0] = usage_count;
							}
							if(value == USAGE_Y) {
								hidp_extreme_debugf("JOYSTICK/MOUSE: found y axis @ %d", usage_count);
								axis[1] = usage_count;
							}
							if(value == USAGE_Z) {
								hidp_extreme_debugf("JOYSTICK/MOUSE: found z axis @ %d", usage_count);
								if (axis[2] == -1) axis[2] = usage_count; // don't override wheel
							}
							if(value == USAGE_RX || value == USAGE_RY || value == USAGE_RZ) {
							        hidp_extreme_debugf("JOYSTICK/MOUSE: found R%c axis @ %d", 'X'+(char)(value-USAGE_RX), usage_count);
								if (axis[3] == -1) axis[3] = usage_count;
							}
							if(value == USAGE_WHEEL) {
								hidp_extreme_debugf("MOUSE: found wheel @ %d", usage_count);
								axis[2] = usage_count;
							}
						}
					} else if((value == USAGE_HAT) && app_collection) {
						// usage(hat) is allowed within the app collection
						hidp_extreme_debugf(" -> hat usage");

						// we support hat on joysticks only
						if(conf->type == REPORT_TYPE_JOYSTICK) {
							hidp_extreme_debugf("JOYSTICK: found hat @ %d", usage_count);
							hat = usage_count;
						}
					} else {
						hidp_extreme_debugf(" -> UNSUPPORTED USAGE");
						//    return false;
					}

					if(usage_listed < MAX_KBD_USAGES)
						usage_list[usage_listed++] = value;
					usage_count++;
					break;

				case 1:
					hidp_extreme_debugf("USAGE_MINIMUM(%lu)", value);
					usage_count -= (value-1);
					usage_min = value;
					usage_range = true;
					break;

				case 2:
					hidp_extreme_debugf("USAGE_MAXIMUM(%lu)", value);
					usage_count += value;
					usage_max = value;
					usage_range = true;
					break;

				default:
					hidp_extreme_debugf("unexpected local item %d", tag);
					//  return false;
					break;
				}
				break;

			default:
				// reserved
				hidp_extreme_debugf("unexpected reserved item %d", tag);
				// return false;
				break;
			}
		}
	}

	// if we get here then no usable setup was found
	return false;
}

void kbd_tx(const ps2_scancode_t scancode) {
	taskENTER_CRITICAL();
	fpga_tx_header(0x0c, scancode.len+1);
	for (int i = 0; i < scancode.len; i++) {
		fpga_tx_byte(scancode.code[i]);
	}
	taskEXIT_CRITICAL();
}

uint8_t usb_to_ascii(uint8_t code, uint8_t modifier) {
	bool ctrl = modifier & 0x01 || modifier & 0x10;
	bool shift = modifier & 0x02 || modifier & 0x20;
	bool alt = modifier & 0x04 || modifier & 0x40;
	bool gui = modifier & 0x08 || modifier & 0x80;
	bool sh = shift && !ctrl && !alt && !gui;

    // Handle special keys first
    if (code == 0x28) return '\n';  // Enter
    if (code == 0x2C) return ' ';   // Space
	if (code == 0x2A) return '\b';  // Backspace
	if (code == 0x2B) return '\t';  // Tab
	if (code == 0x29) return '\e';  // Esc
    if (code == 0x2D) return sh ? '_' : '-';   // Minus
    if (code == 0x2E) return sh ? '+' : '=';   // Equals
    if (code == 0x2F) return sh ? '{' : '[';   // Left bracket
    if (code == 0x30) return sh ? '}' : ']';   // Right bracket
    if (code == 0x31) return sh ? '|' : '\\';  // Backslash
    if (code == 0x33) return sh ? ':' : ';';   // Semicolon
    if (code == 0x34) return sh ? '"' : '\'';  // Quote
    if (code == 0x35) return sh ? '~' : '`';   // Backtick
    if (code == 0x36) return sh ? '<' : ',';   // Comma
    if (code == 0x37) return sh ? '>' : '.';   // Period
    if (code == 0x38) return sh ? '?' : '/';   // Forward slash

    // Handle number keys (0-9)
    if (code >= 0x1E && code <= 0x27) {
        if (sh) {
            return ")!@#$%^&*(" [code - 0x1E];
        }
        return "1234567890" [code - 0x1E];
    }

    // Handle letter keys (a-z)
    if (code >= 0x04 && code <= 0x1D) {
        if (sh) {
            return 'A' + (code - 0x04);
        }
        return 'a' + (code - 0x04);
    }

    return 0;  // Return 0 for unhandled keys
}

static void kbd_set_usage(uint8_t *modifiers, uint8_t keys[32], uint32_t usage) {
	if (usage >= 0xE0 && usage <= 0xE7)
		*modifiers |= (uint8_t)(1U << (usage - 0xE0));
	else if (usage > 3 && usage <= 0xFF)
		keys[usage / 8] |= (uint8_t)(1U << (usage % 8));
}

// Decode a keyboard report into modifier bits and a bitmap of held keys.
// Uses the fields from the report descriptor, so both 8-byte boot reports
// and one-bit-per-key reports (e.g. the Logi Bolt receiver, 046d:c548) work.
// Returns false if the report should be ignored.
static bool kbd_decode(const hid_report_t *report, const unsigned char *buffer, int nbytes,
		       uint8_t *modifiers, uint8_t keys[32]) {
	*modifiers = 0;
	memset(keys, 0, 32);

	if (!report->keyboard.num_fields) {
		// no keyboard fields in the descriptor: assume boot report layout
		if (nbytes != 8) return false;
		*modifiers = buffer[0];
		for (int i = 2; i < 8; i++) {
			if (buffer[i] >= 1 && buffer[i] <= 3) return false;   // rollover/error
			kbd_set_usage(modifiers, keys, buffer[i]);
		}
		return true;
	}

	for (int i = 0; i < report->keyboard.num_fields; i++) {
		const hid_kbd_field_t *f = &report->keyboard.field[i];

		if (f->size == 0 || f->size > 32 ||
		    f->offset + (uint32_t)f->size * f->count > (uint32_t)nbytes * 8)
			return false;

		for (uint32_t element = 0; element < f->count; element++) {
			uint32_t first = f->offset + element * f->size;
			uint32_t value = 0;
			for (uint32_t bit = 0; bit < f->size; bit++)
				value |= (uint32_t)((buffer[(first + bit) / 8] >> ((first + bit) % 8)) & 1) << bit;

			if (f->variable) {
				if (value == 0 || f->usage_min > f->usage_max) continue;
				uint32_t usage_count = (uint32_t)f->usage_max - f->usage_min + 1;
				kbd_set_usage(modifiers, keys,
					      f->usage_min + (element < usage_count ? element : usage_count - 1));
			} else {
				if (value < f->logical_min) continue;
				uint32_t usage = f->usage_min + (value - f->logical_min);
				if (usage == 0) continue;
				if (usage <= 3) return false;   // rollover/error: ignore the whole report
				kbd_set_usage(modifiers, keys, usage);
			}
		}
	}
	return true;
}

static inline bool key_held(const uint8_t keys[32], int usage) {
	return keys[usage / 8] & (1 << (usage % 8));
}

extern volatile int volume_request;     // main.cpp: volume steps asked for
static uint8_t volume_keys_held;        // bit 0: '-', bit 1: '=', pressed with Ctrl for the volume

// Keyboard report: convert to PS/2 scancodes and send them to the core.
// F12 toggles the OSD state. In OSD, arrow keys and space/enter are used for navigation.
void kbd_parse(const hid_report_t *report, struct hid_kbd_state_S *state,
	       const unsigned char *buffer, int nbytes) {
	bool send = !overlay_on();
	uint8_t modifiers, keys[32];

	if (!kbd_decode(report, buffer, nbytes, &modifiers, keys)) return;

	// check if modifier have changed
	for(int i=0;i<8;i++) {            // scancodes are 0xE0 ~ 0xE7
		uint8_t mask = 1 << i;
		if ((modifiers & mask) != (state->modifiers & mask)) {
			bool is_break = (modifiers & mask) == 0;
			if (send) kbd_tx(usb_to_ps2(0xE0 + i, is_break));
		}
	}

	// send scancodes for keys that changed
	bool ctrl = modifiers & 0x11;
	for (int i = 4; i < 256; i++) {
		bool now = key_held(keys, i);
		bool last = key_held(state->keys, i);
		if (now == last) continue;

		if (i == 0x45) {     // F12 toggles the OSD, do not send to the core
			if (now) overlay(!overlay_on());
			continue;
		}

		// Ctrl + - / Ctrl + = in game: master volume down / up (main.cpp applies it).
		// The key is kept from the core, its release too, even if Ctrl is let go first.
		if (i == 0x2D || i == 0x2E) {
			uint8_t bit = 1 << (i - 0x2D);
			if (now && send && ctrl) {
				taskENTER_CRITICAL();
				volume_request += i == 0x2E ? 1 : -1;
				taskEXIT_CRITICAL();
				volume_keys_held |= bit;
				continue;
			}
			if (!now && (volume_keys_held & bit)) {
				volume_keys_held &= ~bit;
				continue;
			}
		}
		if (now) {
			// key pressed
			if (send)
				kbd_tx(usb_to_ps2(i, false));
			else {
				// put into key_buf
				uint8_t ascii = usb_to_ascii(i, modifiers);
				if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
					int j = key_buf[0] == 0 ? 0 :
					        key_buf[1] == 0 ? 1 :
					        key_buf[2] == 0 ? 2 :
					        key_buf[3] == 0 ? 3 : -1;
					if (j != -1) key_buf[j] = ascii;
					xSemaphoreGive(state_mutex);
				}
			}
		} else {
			// key released
			if (send) kbd_tx(usb_to_ps2(i, true));
		}
	}

	// save current state
	state->modifiers = modifiers;
	memcpy(state->keys, keys, sizeof(state->keys));
}

// collect bits from byte stream and assemble them into a signed word
static uint16_t collect_bits(const uint8_t *p, uint16_t offset, uint8_t size, bool is_signed) {
  // mask unused bits of first byte
  uint8_t mask = 0xff << (offset&7);
  uint8_t byte = offset/8;
  uint8_t bits = size;
  uint8_t shift = offset&7;
  
  //  iusb_debugf("0 m:%x by:%d bi=%d sh=%d ->", mask, byte, bits, shift);
  uint16_t rval = (p[byte++] & mask) >> shift;
  mask = 0xff;
  shift = 8-shift;
  bits -= shift;
  
  // first byte already contained more bits than we need
  if(shift > size) {
    // mask unused bits
    rval &= (1<<size)-1;
  } else {
    // further bytes if required
    while(bits) {
      mask = (bits<8)?(0xff>>(8-bits)):0xff;
      rval += (p[byte++] & mask) << shift;
      shift += 8;
      bits -= (bits>8)?8:bits;
    }
  }
  
  if(is_signed) {
    // do sign expansion
    uint16_t sign_bit = 1<<(size-1);
    if(rval & sign_bit) {
      while(sign_bit) {
	rval |= sign_bit;
	sign_bit <<= 1;
      }
    }
  }
  
  return rval;
}

void joystick_parse(const hid_report_t *report, struct hid_joystick_state_S *state,
		    const unsigned char *buffer, __attribute__((unused)) int nbytes) {
  //  usb_debugf("joystick: %d %02x %02x %02x %02x", nbytes,
  //  	 buffer[0]&0xff, buffer[1]&0xff, buffer[2]&0xff, buffer[3]&0xff);

  // collect info about the two axes
  int a[2];
  for(int i=0;i<2;i++) {  
    bool is_signed = report->joystick_mouse.axis[i].logical.min > 
      report->joystick_mouse.axis[i].logical.max;
    
    a[i] = collect_bits(buffer, report->joystick_mouse.axis[i].offset, 
			report->joystick_mouse.axis[i].size, is_signed);
  }

  // ... and four buttons
  unsigned char joy = 0;
  for(int i=0;i<4;i++)
    if(buffer[report->joystick_mouse.button[i].byte_offset] & 
       report->joystick_mouse.button[i].bitmask)
      joy |= (0x10<<i);

  // ... and the eight extra buttons
  unsigned char btn_extra = 0;
  for(int i=4;i<12;i++)
    if(buffer[report->joystick_mouse.button[i].byte_offset] & 
      report->joystick_mouse.button[i].bitmask) 
      btn_extra |= (1<<(i-4));

  // map directions to digital
  if(a[0] > 0xc0) joy |= 0x01;	// right
  if(a[0] < 0x40) joy |= 0x02;	// left
  if(a[1] > 0xc0) joy |= 0x04;	// down
  if(a[1] < 0x40) joy |= 0x08;  // up

  int ax = 0;
  int ay = 0;
  ax = a[0];
  ay = a[1];

  if((joy != state->last_state) || 
     (ax != state->last_state_x) || 
     (ay != state->last_state_y) || 
     (btn_extra != state->last_state_btn_extra))  {
    state->last_state = joy;
    state->last_state_x = ax;
    state->last_state_y = ay;
    state->last_state_btn_extra = btn_extra;
    DEBUG("JOY%d: D %02x X %02x Y %02x EB %02x\n", state->js_index, joy, ax, ay, btn_extra);

  }
}

void hid_parse(const hid_report_t *report, hid_state_t *state, uint8_t const* data, uint16_t len) {
  //  usb_debugf("hid parse %d, expect %d", len, report->report_size);
  if(!len) return;
  
  // hexdump((void*)data, len);

  // check and skip report id if present
  if(report->report_id_present && (len-1 == report->report_size)) {
    if(data[0] != report->report_id) {
      DEBUG("FAIL %d != %d", data[0], report->report_id);
      return;
    }
        
    // skip report id
    data++; len--;
  }
  
  if(len == report->report_size) {
    if(report->type == REPORT_TYPE_KEYBOARD)
      kbd_parse(report, &state->kbd, data, len);
    
    if(report->type == REPORT_TYPE_MOUSE)
      DEBUG("MOUSE");
    
    if(report->type == REPORT_TYPE_JOYSTICK)
      joystick_parse(report, &state->joystick, data, len);
  }
}


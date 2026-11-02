#ifndef HIDPARSER_H
#define HIDPARSER_H

#include <stdint.h>
#include <stdbool.h>

#define REPORT_TYPE_NONE     0
#define REPORT_TYPE_MOUSE    1
#define REPORT_TYPE_KEYBOARD 2
#define REPORT_TYPE_JOYSTICK 3

#define MAX_AXES 4
#define MAX_KBD_FIELDS 16
#define MAX_KBD_USAGES 32     // USAGE items remembered per main item

// a keyboard input field (usage page 7) from the report descriptor
typedef struct {
	uint16_t offset;      // bit offset inside report (without report id)
	uint16_t usage_min;   // usage of the first element (variable fields)
	uint16_t usage_max;
	uint16_t logical_min; // value of usage_min (array fields)
	uint8_t size;         // size of one element in bits
	uint8_t count;        // number of elements
	uint8_t variable: 1;  // 1: one bit per usage, 0: array of usage codes
} hid_kbd_field_t;

// currently only joysticks are supported
typedef struct {
	uint8_t type: 2;               // REPORT_TYPE_...
	uint8_t report_id_present: 1;  // REPORT_TYPE_...
	uint8_t report_id;
	uint8_t report_size;

	union {
		struct {
			struct {
				uint16_t offset;
				uint8_t size;
				struct {
					uint16_t min;
					uint16_t max;
				} logical;
			} axis[MAX_AXES];               // x and y axis + wheel or right hat
				
			struct {
				uint8_t byte_offset;
				uint8_t bitmask;
			} button[12];             // 12 buttons max
				
			struct {
				uint16_t offset;
				uint8_t size;
				struct {
					uint16_t min;
					uint16_t max;
				} logical;
				struct {
					uint16_t min;
					uint16_t max;
				} physical;
			} hat;                   // 1 hat (joystick only)
		} joystick_mouse;
	};

	// keyboard layout, so boot style and one-bit-per-key reports both decode
	struct {
		uint8_t num_fields;
		hid_kbd_field_t field[MAX_KBD_FIELDS];
	} keyboard;
} hid_report_t;

// return true if the report descriptor is a joystick
// parse raw report descriptor `rep` into hid_report_t `conf`
bool parse_report_descriptor(const uint8_t *rep, uint16_t rep_size, hid_report_t *conf, uint16_t *rbytes);


// HID report processing 

// keys currently held: modifier bits (usages 0xE0-0xE7) and a bitmap of usages
struct hid_kbd_state_S {
  uint8_t modifiers;
  uint8_t keys[32];
};

struct hid_mouse_state_S {
};
struct hid_joystick_state_S {
  unsigned char last_state;
  unsigned char js_index;
  unsigned char last_state_x;
  unsigned char last_state_y;
  unsigned char last_state_btn_extra;
};

// a struct, not a union: keyboard reports must not overwrite js_index
typedef struct {
  struct hid_kbd_state_S kbd;
  struct hid_mouse_state_S mouse;
  struct hid_joystick_state_S joystick;  
} hid_state_t;

// Parse received report `data` into `state`, given report descriptor `report`
void hid_parse(const hid_report_t *report, hid_state_t *state, uint8_t const* data, uint16_t len);

// Parse report `buffer` into `state`
void kbd_parse(const hid_report_t *report, struct hid_kbd_state_S *state, const unsigned char *buffer, int nbytes);
void mouse_parse(const hid_report_t *report, struct hid_mouse_state_S *state, const unsigned char *buffer, int nbytes);
void joystick_parse(const hid_report_t *report, struct hid_joystick_state_S *state, const unsigned char *buffer, int nbytes);

#endif // HIDPARSER_H

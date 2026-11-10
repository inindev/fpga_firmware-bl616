#pragma once

// ROM images from MRA files: reads the MRA, finds each <part> in the MAME zips and sends the
// image in order, as MiSTer builds it (Main_MiSTer support/arcade/mra_loader.cpp):
//
// - A part's zips are its own zip= list, else the <rom>'s, tried in order. A name starting
//   with '/' is under the arcade folder, any other is in its mame/ folder.
// - In each zip the file is looked up by the part's CRC first, then by its name (the full
//   path in the zip, any case). The first zip that has it is used, even if it was found by
//   name and its CRC differs from the part's.
// - From the file, the bytes from `offset`, at most `length` of them, `repeat` times.
//   A part without a name sends its hex bytes `repeat` times.
//
// Every file is extracted once (which checks its data against the zip's own CRC) before
// anything is sent, so a bad zip is reported before the core is reset. Uses FatFS and the
// zip reader only, so it can be tested off the board.

#include <stdint.h>
#include <string>
#include <vector>
#include "ff.h"
#include "zip.h"
#include "mra.h"

// Read the MRA file at `path` (through `fp`) and parse it. On failure, `err` is set.
bool mra_load(FIL *fp, const char *path, mra_info &mra, std::string &err);

struct mra_zip {
    std::string path;
    FRESULT open;               // FR_OK: entries are valid
    const char *list_err;       // error from zip_list, or NULL
    std::vector<zip_entry> entries;
};

// Where each part of a <rom> comes from
struct mra_plan {
    const mra_rom *rom;
    std::vector<mra_zip> zips;
    std::vector<int> zip;       // per part: index in zips, -1 for data
    std::vector<int> entry;     // per part: index in zips[zip].entries
    uint32_t size;              // image size
    uint32_t max_entry;         // largest file used
};

// Find and check every part of `rom`. `arcade_dir` is the arcade folder, e.g. "usb:arcade".
// Return true if all parts are there and good; otherwise false with `err` set to a message
// for the overlay (at most 26 characters per line).
bool mra_prepare(FIL *fp, const std::string &arcade_dir, const mra_rom &rom, mra_plan &plan,
                 std::string &err);

// Called with consecutive pieces of the image, MRA_CHUNK bytes each (the last may be shorter)
#define MRA_CHUNK 1024
typedef void (*mra_sink)(const uint8_t *data, uint32_t len, void *ctx);

// Extract the parts again and pass the whole image to `sink`.
bool mra_stream(FIL *fp, const mra_plan &plan, mra_sink sink, void *ctx, std::string &err);

// Build a whole <rom> into memory (for the small core specific ones, index 1 and up).
bool mra_build(FIL *fp, const std::string &arcade_dir, const mra_rom &rom,
               std::vector<uint8_t> &out, std::string &err);

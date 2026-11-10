#pragma once

// Minimal zip reader over FatFS: lists a zip's central directory and extracts one entry
// (stored or deflate) to memory. No zip64, encryption or multi-disk archives; MAME ROM
// sets don't use them.

#include <stdint.h>
#include <string>
#include <vector>
#include "ff.h"

struct zip_entry {
    std::string name;           // full path inside the zip, '/' separated
    uint32_t crc;
    uint32_t comp_size;
    uint32_t size;              // uncompressed size
    uint32_t local_offset;      // offset of the local file header
    uint16_t method;            // 0: stored, 8: deflate
};

// Read the central directory of the open zip file `fp` into `entries`.
// Return NULL on success, or an error message.
const char *zip_list(FIL *fp, std::vector<zip_entry> &entries);

// Extract `e` into `out` (e.size bytes) and check its CRC32.
// Return NULL on success, or an error message.
const char *zip_extract(FIL *fp, const zip_entry &e, uint8_t *out);

uint32_t zip_crc32(const uint8_t *data, uint32_t len);

#pragma once

// MRA files (MiSTer ROM descriptions): a small parser for the subset of the format the
// firmware loads. It follows MiSTer's loader (Main_MiSTer support/arcade/mra_loader.cpp):
//
//   <rbf>                       the core, matched to core_info::rbf
//   <switches default="..">     DIP switch defaults, hex bytes: byte i is bits 8i-8i+7
//   <rom index="N" zip="a.zip|b.zip" md5="none">
//     <part name=".." crc=".." [zip=".."] [offset=".."] [length=".."] [repeat=".."]/>
//                               a file from the zip(s), found by CRC, else by name
//     <part [repeat=".."]> hex bytes </part>
//                               the bytes in the text, `repeat` times
//
// Not supported, reported as errors: <interleave>, <patch>, <part map=..>, <rom address=..>,
// an md5 other than "none", and more than one <rom> with the same index. Everything else
// (buttons, dip lists, nvram, cheats, ...) is ignored, as it does not change the ROM image.
// No file system code here, so it can be tested off the board.

#include <stdint.h>
#include <string>
#include <vector>

struct mra_part {
    std::string name;           // file in the zip; empty: the hex bytes in `data`
    std::string zip;            // zip list for this part ('|' separated); empty: the rom's
    uint32_t crc;               // 0: none (find by name only)
    uint32_t offset;            // first byte of the file to use
    uint32_t length;            // bytes of the file to use; 0: to its end
    uint32_t repeat;            // times the part is sent (default 1)
    std::vector<uint8_t> data;  // hex bytes from the text, when there is no name
};

struct mra_rom {
    int index;                  // <rom index>: 0 is the ROM image, others are core specific
    std::string zip;            // zip list ('|' separated)
    std::vector<mra_part> parts;
};

struct mra_info {
    std::string name;           // <name> (or <n>)
    std::string setname;
    std::string rbf;
    std::vector<uint8_t> switches;      // <switches default>, empty if none
    std::vector<mra_rom> roms;
};

// Parse an MRA file's text. Return true on success; otherwise false with `err` set to a
// message for the overlay (at most 26 characters per line).
bool mra_parse(const char *text, size_t len, mra_info &mra, std::string &err);

// The <rom> with this index, or NULL
const mra_rom *mra_find_rom(const mra_info &mra, int index);

// DIP switch defaults as one value, as MiSTer keeps them (byte i in bits 8i-8i+7)
uint64_t mra_switches(const mra_info &mra);

// MiSTer's hex data format: pairs of hex digits, with spaces, tabs, newlines or commas before
// any pair ("00 7F", "007F", "00,7F"); a single digit at the very end is a byte of its own.
// Return false if the text has anything else.
bool mra_hex(const std::string &text, std::vector<uint8_t> &out);

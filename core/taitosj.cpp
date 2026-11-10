// Taito System SJ arcade core: games are MRA files (core/arcade.cpp). The core takes the
// ROM image as the MRA builds it, and its settings as core_config:
//   [7:0] DIP1, [15:8] DIP2, [23:16] DIP3   MRA switch bytes 1, 2, 3 (byte 0 is unused)
//   [31:24] pcb                             the byte in <rom index="1"> (board variant)
#include <map>
#include <string>
#include <vector>

#include "cores.h"

static bool taitosj_config(const mra_info &mra, const std::map<int, std::vector<uint8_t>> &roms,
                           uint32_t &config, std::string &err) {
    auto pcb = roms.find(1);
    if (pcb == roms.end() || pcb->second.size() != 1) {
        err = "MRA needs the PCB byte\nin <rom index=\"1\">";
        return false;
    }
    config = (uint32_t)(mra_switches(mra) >> 8) & 0xffffff;
    config |= (uint32_t)pcb->second[0] << 24;
    return true;
}

// return 0 if successful
int loadtaitosj(const char *fname) {
    return load_mra(fname, taitosj_config);
}

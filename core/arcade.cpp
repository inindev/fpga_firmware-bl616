// Arcade games from MRA files: the MRA in arcade/ gives the ROM image, built from the MAME
// zips in arcade/mame/ (see mra_image.h), and the core's settings, which each core maps to its
// core_config (an mra_config_fn, e.g. taitosj_config in taitosj.cpp).
#include <string.h>
#include <map>
#include <string>
#include <vector>

#include "utils.h"
#include "cores.h"
#include "overlay.h"
#include "mra_image.h"

struct send_state {
    uint32_t total, size;
};

// send a piece of the image to the core
static void send_piece(const uint8_t *data, uint32_t len, void *ctx) {
    send_state *s = (send_state *)ctx;
    memcpy(fbuf, data, len);
    send_fbuf_data(len);
    taskYIELD();                // allow gamepad polling to run
    s->total += len;
    if ((s->total & 0x3fff) == 0)
        //              01234567890123456789012345678901
        overlay_status("%d/%dK                          ", s->total >> 10, s->size >> 10);
}

static int fail(const std::string &err) {
    overlay_message(err.c_str(), 1);
    return 1;
}

// Load the game of an MRA file
// return 0 if successful
int load_mra(const char *fname, mra_config_fn config_fn) {
    DEBUG("load_mra start\n");
    std::string err, dir = std::string(drv) + ARCADE_DIR;
    mra_info mra;
    if (!mra_load(&fcore, fname, mra, err))
        return fail(err);
    const mra_rom *rom0 = mra_find_rom(mra, 0);
    if (rom0 == NULL)
        return fail("MRA has no <rom index=\"0\">");

    // the other <rom>s are small and core specific: built in memory for config_fn
    std::map<int, std::vector<uint8_t>> roms;
    for (auto &r : mra.roms)
        if (r.index != 0 && !mra_build(&fcore, dir, r, roms[r.index], err))
            return fail(err);
    uint32_t config;
    if (!config_fn(mra, roms, config, err))
        return fail(err);

    overlay_status("Checking %s", mra.setname.c_str());
    mra_plan plan;
    if (!mra_prepare(&fcore, dir, *rom0, plan, err))
        return fail(err);

    // load the ROM image
    set_loading_state(1);       // enable game loading, this resets the core
    core_running = false;
    send_state s = {0, plan.size};
    bool ok = mra_stream(&fcore, plan, send_piece, &s, err);

    // settings in place when the core leaves reset
    set_core_config(config);
    set_loading_state(0);       // turn off game loading, this starts the core

    if (!ok || s.total != plan.size)
        return fail(err.empty() ? "ROM image incomplete" : err);

    DEBUG("load_mra: %s, %d bytes, config %08x\n", mra.setname.c_str(), s.total, config);
    overlay_status("Success");
    core_running = true;
    overlay(0);                 // turn off OSD
    return 0;
}

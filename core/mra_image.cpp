// ROM images from MRA files, see mra_image.h

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "utils.h"
#include "mra_image.h"

#define MRA_MAX_FILE    (64*1024)       // MRA file size limit
#define MRA_MAX_IMAGE   (16*1024*1024)  // ROM image size limit

bool mra_load(FIL *fp, const char *path, mra_info &mra, std::string &err) {
    if (f_open(fp, path, FA_READ) != FR_OK) {
        err = "Cannot open MRA file";
        return false;
    }
    uint32_t size = f_size(fp);
    if (size > MRA_MAX_FILE) {
        f_close(fp);
        err = "MRA file too large";
        return false;
    }
    // read through fbuf, which the USB/SD drivers can DMA into
    std::string text;
    text.reserve(size);
    while (text.size() < size) {
        UINT n = size - text.size() > BLOCK_SIZE ? BLOCK_SIZE : size - text.size(), br;
        if (f_read(fp, fbuf, n, &br) != FR_OK || br != n) {
            f_close(fp);
            err = "MRA file read error";
            return false;
        }
        text.append((const char *)fbuf, n);
    }
    f_close(fp);
    return mra_parse(text.data(), text.size(), mra, err);
}

// the names in a '|' separated list, empty ones included (as strsep gives them)
static std::vector<std::string> split_zips(const std::string &list) {
    std::vector<std::string> v;
    size_t s = 0;
    for (;;) {
        size_t e = list.find('|', s);
        v.push_back(list.substr(s, e == std::string::npos ? std::string::npos : e - s));
        if (e == std::string::npos)
            return v;
        s = e + 1;
    }
}

static std::string zip_path(const std::string &arcade_dir, const std::string &name) {
    return name[0] == '/' ? arcade_dir + name : arcade_dir + "/mame/" + name;
}

// index of the zip at `path` in plan.zips, opened and listed the first time
static int get_zip(FIL *fp, mra_plan &plan, const std::string &path) {
    for (size_t i = 0; i < plan.zips.size(); i++)
        if (plan.zips[i].path == path)
            return i;
    mra_zip z;
    z.path = path;
    z.list_err = NULL;
    z.open = f_open(fp, path.c_str(), FA_READ);
    if (z.open == FR_OK) {
        z.list_err = zip_list(fp, z.entries);
        f_close(fp);
    }
    plan.zips.push_back(std::move(z));
    return plan.zips.size() - 1;
}

// the zip entry for a part: by CRC, then by its full name in any case; -1 if none
static int find_entry(const mra_zip &z, const mra_part &p) {
    if (p.crc)
        for (size_t j = 0; j < z.entries.size(); j++)
            if (z.entries[j].crc == p.crc)
                return j;
    for (size_t j = 0; j < z.entries.size(); j++)
        if (strcasecmp(z.entries[j].name.c_str(), p.name.c_str()) == 0)
            return j;
    return -1;
}

static std::string hex32(uint32_t v) {
    char s[12];
    snprintf(s, sizeof(s), "%08x", (unsigned)v);
    return s;
}

// bytes a file part sends each time (`size`: the file's size), or false if offset is past it
static bool part_bytes(const mra_part &p, uint32_t size, uint32_t &n) {
    if (p.offset > size)
        return false;
    n = size - p.offset;
    if (p.length > 0 && p.length < n)
        n = p.length;
    return true;
}

bool mra_prepare(FIL *fp, const std::string &arcade_dir, const mra_rom &rom, mra_plan &plan,
                 std::string &err) {
    plan.rom = &rom;
    plan.zips.clear();
    plan.zip.assign(rom.parts.size(), -1);
    plan.entry.assign(rom.parts.size(), -1);
    plan.max_entry = 0;
    uint64_t total = 0;

    for (size_t i = 0; i < rom.parts.size(); i++) {
        const mra_part &p = rom.parts[i];
        if (p.name.empty()) {
            total += (uint64_t)p.data.size() * p.repeat;
            if (total > MRA_MAX_IMAGE)
                break;
            continue;
        }
        const std::string &list = p.zip.empty() ? rom.zip : p.zip;
        if (list.empty()) {
            err = "No zip for ROM part:\n" + p.name;
            return false;
        }

        // the first zip that has the file
        std::vector<std::string> names = split_zips(list);
        int zi = -1, ei = -1;
        int bad = -1;                   // a zip that is there but can't be read
        bool any = false;               // any zip of the list is there
        for (auto &n : names) {
            if (n.empty())
                continue;
            int k = get_zip(fp, plan, zip_path(arcade_dir, n));
            const mra_zip &z = plan.zips[k];
            if (z.open != FR_OK)
                continue;
            any = true;
            if (z.list_err) {
                if (bad < 0) bad = k;
                continue;
            }
            if ((ei = find_entry(z, p)) >= 0) {
                zi = k;
                break;
            }
        }
        if (zi < 0) {
            if (bad >= 0) {
                const mra_zip &z = plan.zips[bad];
                err = std::string(z.list_err) + ":\n" + z.path.substr(z.path.find(':') + 1);
            } else if (!any) {
                err = "Zip not found in\n" + (arcade_dir + "/mame/").substr(arcade_dir.find(':') + 1) + ":";
                for (auto &n : names)
                    err += "\n" + n;
            } else {
                err = "Missing ROM part:\n" + p.name + (p.crc ? "\nCRC " + hex32(p.crc) : "");
            }
            return false;
        }

        // found by name with another CRC: used, as MiSTer does
        const zip_entry &e = plan.zips[zi].entries[ei];
        uint32_t n;
        if (!part_bytes(p, e.size, n)) {
            err = "Part offset past its end:\n" + p.name;
            return false;
        }
        plan.zip[i] = zi;
        plan.entry[i] = ei;
        if (e.size > plan.max_entry)
            plan.max_entry = e.size;
        total += (uint64_t)n * p.repeat;
        if (total > MRA_MAX_IMAGE)
            break;
    }
    if (total > MRA_MAX_IMAGE) {
        err = "ROM image too large";
        return false;
    }
    plan.size = total;

    // extract each file once, which checks its data against the zip's CRC
    uint8_t *buf = (uint8_t *)malloc(plan.max_entry ? plan.max_entry : 1);
    if (!buf) {
        err = "Out of memory";
        return false;
    }
    bool ok = true;
    for (size_t z = 0; z < plan.zips.size() && ok; z++) {
        bool open = false;
        std::vector<bool> done(plan.zips[z].entries.size(), false);
        for (size_t i = 0; i < rom.parts.size() && ok; i++) {
            if (plan.zip[i] != (int)z || done[plan.entry[i]])
                continue;
            done[plan.entry[i]] = true;
            if (!open && f_open(fp, plan.zips[z].path.c_str(), FA_READ) != FR_OK) {
                err = "Cannot open zip:\n" + plan.zips[z].path.substr(plan.zips[z].path.find(':') + 1);
                ok = false;
                break;
            }
            open = true;
            const char *e = zip_extract(fp, plan.zips[z].entries[plan.entry[i]], buf);
            if (e) {
                err = std::string(e) + ":\n" + rom.parts[i].name;
                ok = false;
            }
        }
        if (open)
            f_close(fp);
    }
    free(buf);
    return ok;
}

// collects the image into MRA_CHUNK pieces for the sink
struct emitter {
    mra_sink sink;
    void *ctx;
    uint32_t n;
    uint8_t buf[MRA_CHUNK];

    void put(const uint8_t *d, uint32_t len) {
        while (len) {
            uint32_t k = MRA_CHUNK - n < len ? MRA_CHUNK - n : len;
            memcpy(buf + n, d, k);
            n += k;
            d += k;
            len -= k;
            if (n == MRA_CHUNK)
                flush();
        }
    }
    void flush() {
        if (n)
            sink(buf, n, ctx);
        n = 0;
    }
};

bool mra_stream(FIL *fp, const mra_plan &plan, mra_sink sink, void *ctx, std::string &err) {
    const mra_rom &rom = *plan.rom;
    emitter *out = (emitter *)malloc(sizeof(emitter));
    uint8_t *buf = (uint8_t *)malloc(plan.max_entry ? plan.max_entry : 1);
    if (!out || !buf) {
        free(out);
        free(buf);
        err = "Out of memory";
        return false;
    }
    out->sink = sink;
    out->ctx = ctx;
    out->n = 0;

    bool ok = true;
    int cur = -1;                       // zip open in fp
    for (size_t i = 0; i < rom.parts.size() && ok; i++) {
        const mra_part &p = rom.parts[i];
        if (p.name.empty()) {
            for (uint32_t r = 0; r < p.repeat; r++)
                out->put(p.data.data(), p.data.size());
            continue;
        }
        const mra_zip &z = plan.zips[plan.zip[i]];
        if (plan.zip[i] != cur) {
            if (cur >= 0)
                f_close(fp);
            cur = -1;
            if (f_open(fp, z.path.c_str(), FA_READ) != FR_OK) {
                err = "Cannot open zip:\n" + z.path.substr(z.path.find(':') + 1);
                ok = false;
                break;
            }
            cur = plan.zip[i];
        }
        const zip_entry &e = z.entries[plan.entry[i]];
        const char *msg = zip_extract(fp, e, buf);
        if (msg) {
            err = std::string(msg) + ":\n" + p.name;
            ok = false;
            break;
        }
        uint32_t n = 0;
        part_bytes(p, e.size, n);       // checked in mra_prepare
        for (uint32_t r = 0; r < p.repeat; r++)
            out->put(buf + p.offset, n);
    }
    if (cur >= 0)
        f_close(fp);
    if (ok)
        out->flush();
    free(out);
    free(buf);
    return ok;
}

static void append(const uint8_t *data, uint32_t len, void *ctx) {
    std::vector<uint8_t> *v = (std::vector<uint8_t> *)ctx;
    v->insert(v->end(), data, data + len);
}

bool mra_build(FIL *fp, const std::string &arcade_dir, const mra_rom &rom,
               std::vector<uint8_t> &out, std::string &err) {
    mra_plan plan;
    out.clear();
    return mra_prepare(fp, arcade_dir, rom, plan, err) && mra_stream(fp, plan, append, &out, err);
}

// Minimal zip reader over FatFS, see zip.h

#include <stdlib.h>
#include <string.h>

#include "utils.h"
#include "zip.h"
#include "tinf.h"

#define ZIP_MAX_CD      (64*1024)       // central directory size limit
#define ZIP_MAX_ENTRY   (1024*1024)     // entry size limit (arcade ROM parts are small)

static uint32_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Read len bytes at off into dst. Disk reads go through fbuf, which is in the
// non-cacheable section the USB/SD drivers can DMA into.
static bool read_at(FIL *fp, uint32_t off, uint8_t *dst, uint32_t len) {
    if (f_lseek(fp, off) != FR_OK)
        return false;
    while (len) {
        UINT n = len > BLOCK_SIZE ? BLOCK_SIZE : len, br;
        if (f_read(fp, fbuf, n, &br) != FR_OK || br != n)
            return false;
        memcpy(dst, fbuf, n);
        dst += n;
        len -= n;
    }
    return true;
}

uint32_t zip_crc32(const uint8_t *data, uint32_t len) {
    return tinf_crc32(data, len);
}

const char *zip_list(FIL *fp, std::vector<zip_entry> &entries) {
    entries.clear();
    uint32_t size = f_size(fp);
    if (size < 22)
        return "Not a zip file";

    // end of central directory record: the last 22 bytes, plus a comment of any length;
    // look for it in the last BLOCK_SIZE bytes
    uint32_t tail = size < BLOCK_SIZE ? size : BLOCK_SIZE;
    UINT br;
    if (f_lseek(fp, size - tail) != FR_OK || f_read(fp, fbuf, tail, &br) != FR_OK || br != tail)
        return "Zip read error";
    int eocd = -1;
    for (int i = tail - 22; i >= 0; i--) {
        if (le32(fbuf + i) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0)
        return "Not a zip file";
    const uint8_t *e = fbuf + eocd;
    uint32_t count = le16(e + 10);
    uint32_t cd_size = le32(e + 12);
    uint32_t cd_off = le32(e + 16);
    if (le16(e + 4) != 0 || le16(e + 6) != 0 || count == 0xffff || cd_off == 0xffffffff)
        return "Unsupported zip (multi-disk/zip64)";
    if (cd_size > ZIP_MAX_CD || cd_off + cd_size > size)
        return "Bad zip directory";

    uint8_t *cd = (uint8_t *)malloc(cd_size);
    if (!cd)
        return "Out of memory";
    const char *err = NULL;
    if (!read_at(fp, cd_off, cd, cd_size)) {
        err = "Zip read error";
    } else {
        uint32_t p = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (p + 46 > cd_size || le32(cd + p) != 0x02014b50) {
                err = "Bad zip directory";
                break;
            }
            const uint8_t *h = cd + p;
            uint32_t nlen = le16(h + 28), xlen = le16(h + 30), clen = le16(h + 32);
            if (p + 46 + nlen > cd_size) {
                err = "Bad zip directory";
                break;
            }
            zip_entry z;
            z.method = le16(h + 10);
            z.crc = le32(h + 16);
            z.comp_size = le32(h + 20);
            z.size = le32(h + 24);
            z.local_offset = le32(h + 42);
            z.name.assign((const char *)h + 46, nlen);
            if (!(le16(h + 8) & 1))                 // skip encrypted entries
                entries.push_back(z);
            p += 46 + nlen + xlen + clen;
        }
    }
    free(cd);
    return err;
}

const char *zip_extract(FIL *fp, const zip_entry &e, uint8_t *out) {
    uint8_t h[30];
    if (e.size > ZIP_MAX_ENTRY || e.comp_size > ZIP_MAX_ENTRY)
        return "Zip entry too large";
    if (!read_at(fp, e.local_offset, h, 30) || le32(h) != 0x04034b50)
        return "Bad zip entry header";
    uint32_t data = e.local_offset + 30 + le16(h + 26) + le16(h + 28);

    if (e.method == 0) {
        if (e.comp_size != e.size || !read_at(fp, data, out, e.size))
            return "Zip read error";
    } else if (e.method == 8) {
        uint8_t *comp = (uint8_t *)malloc(e.comp_size ? e.comp_size : 1);
        if (!comp)
            return "Out of memory";
        unsigned int len = e.size;
        int r = TINF_DATA_ERROR;
        if (read_at(fp, data, comp, e.comp_size))
            r = tinf_uncompress(out, &len, comp, e.comp_size);
        free(comp);
        if (r != TINF_OK || len != e.size)
            return "Zip inflate error";
    } else {
        return "Unsupported zip method";
    }

    if (zip_crc32(out, e.size) != e.crc)
        return "Zip CRC error";
    return NULL;
}

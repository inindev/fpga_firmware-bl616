// MRA parser, see mra.h
//
// The XML reader handles what MRA files contain: elements with attributes ('..' or ".."
// quoted), text, comments, CDATA, the <?xml ..?> declaration, the five predefined entities
// and character references. A DOCTYPE or a malformed document is an error, with its line.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "mra.h"

namespace {

struct xml_attr {
    std::string name, value;
};

struct xml_node {
    std::string tag;
    std::vector<xml_attr> attrs;
    std::string text;           // the element's own text, children's text not included
    std::vector<xml_node> children;
    int line;

    const char *attr(const char *n) const {
        for (auto &a : attrs)
            if (strcasecmp(a.name.c_str(), n) == 0)
                return a.value.c_str();
        return NULL;
    }
};

struct xml_reader {
    const char *p, *end;
    int line = 1;
    std::string err;

    bool fail(const char *msg) {
        if (err.empty()) {
            char s[48];
            snprintf(s, sizeof(s), "MRA error, line %d:\n", line);
            err = std::string(s) + msg;
        }
        return false;
    }
    bool at(const char *s) const {
        size_t n = strlen(s);
        return (size_t)(end - p) >= n && memcmp(p, s, n) == 0;
    }
    void adv(size_t n) {
        for (; n && p < end; n--, p++)
            if (*p == '\n') line++;
    }
    static bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
    void skip_space() {
        while (p < end && space(*p)) adv(1);
    }
    // skip to just past `term`
    bool skip_past(const char *term, const char *what) {
        while (p < end && !at(term)) adv(1);
        if (p >= end) return fail(what);
        adv(strlen(term));
        return true;
    }
    static bool name_start(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || (c & 0x80);
    }
    static bool name_char(char c) {
        return name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
    }
    bool read_name(std::string &n) {
        if (p >= end || !name_start(*p)) return fail("bad name");
        const char *s = p;
        while (p < end && name_char(*p)) p++;
        n.assign(s, p - s);
        return true;
    }

    // append UTF-8 for code point c
    static void put_utf8(std::string &s, unsigned long c) {
        if (c < 0x80) {
            s += (char)c;
        } else if (c < 0x800) {
            s += (char)(0xc0 | (c >> 6));
            s += (char)(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            s += (char)(0xe0 | (c >> 12));
            s += (char)(0x80 | ((c >> 6) & 0x3f));
            s += (char)(0x80 | (c & 0x3f));
        } else {
            s += (char)(0xf0 | (c >> 18));
            s += (char)(0x80 | ((c >> 12) & 0x3f));
            s += (char)(0x80 | ((c >> 6) & 0x3f));
            s += (char)(0x80 | (c & 0x3f));
        }
    }
    // an entity or character reference at p ('&'), appended to s
    bool read_ref(std::string &s) {
        const char *semi = (const char *)memchr(p, ';', end - p);
        if (!semi || semi - p > 12) return fail("bad '&' reference");
        std::string r(p + 1, semi - p - 1);
        if (r == "amp") s += '&';
        else if (r == "lt") s += '<';
        else if (r == "gt") s += '>';
        else if (r == "quot") s += '"';
        else if (r == "apos") s += '\'';
        else if (r.size() > 1 && r[0] == '#') {
            bool hex = r[1] == 'x';
            const char *d = r.c_str() + (hex ? 2 : 1);
            char *e;
            unsigned long c = strtoul(d, &e, hex ? 16 : 10);
            if (!*d || *e || c == 0 || c > 0x10ffff) return fail("bad character reference");
            put_utf8(s, c);
        } else {
            return fail(("unknown entity &" + r + ";").c_str());
        }
        adv(semi + 1 - p);
        return true;
    }

    bool read_attrs(xml_node &n, bool &empty) {
        for (;;) {
            bool sp = p < end && space(*p);
            skip_space();
            if (p >= end) return fail("unterminated tag");
            if (*p == '>') {
                adv(1);
                empty = false;
                return true;
            }
            if (at("/>")) {
                adv(2);
                empty = true;
                return true;
            }
            if (!sp) return fail("bad tag");
            xml_attr a;
            if (!read_name(a.name)) return false;
            skip_space();
            if (p >= end || *p != '=') return fail("attribute without value");
            adv(1);
            skip_space();
            if (p >= end || (*p != '"' && *p != '\'')) return fail("attribute value not quoted");
            char q = *p;
            adv(1);
            while (p < end && *p != q) {
                if (*p == '<') return fail("'<' in attribute value");
                if (*p == '&') {
                    if (!read_ref(a.value)) return false;
                } else {
                    a.value += *p;
                    adv(1);
                }
            }
            if (p >= end) return fail("unterminated attribute value");
            adv(1);
            if (n.attr(a.name.c_str())) return fail(("duplicate attribute " + a.name).c_str());
            n.attrs.push_back(a);
        }
    }

    // content of element n, up to and including its end tag
    bool read_content(xml_node &n, int depth) {
        if (depth > 16) return fail("elements nested too deep");
        for (;;) {
            if (p >= end) return fail(("missing </" + n.tag + ">").c_str());
            if (at("<!--")) {
                if (!skip_past("-->", "unterminated comment")) return false;
            } else if (at("<![CDATA[")) {
                adv(9);
                const char *s = p;
                if (!skip_past("]]>", "unterminated CDATA")) return false;
                n.text.append(s, p - 3 - s);
            } else if (at("<?")) {
                if (!skip_past("?>", "unterminated <?")) return false;
            } else if (at("</")) {
                adv(2);
                std::string t;
                if (!read_name(t)) return false;
                skip_space();
                if (p >= end || *p != '>') return fail("bad end tag");
                adv(1);
                if (t != n.tag) return fail(("</" + t + "> closes <" + n.tag + ">").c_str());
                return true;
            } else if (at("<!")) {
                return fail("unsupported <! markup");
            } else if (*p == '<') {
                xml_node c;
                if (!read_element(c, depth + 1)) return false;
                n.children.push_back(std::move(c));
            } else if (*p == '&') {
                if (!read_ref(n.text)) return false;
            } else {
                n.text += *p;
                adv(1);
            }
        }
    }

    bool read_element(xml_node &n, int depth) {
        n.line = line;
        adv(1);                         // '<'
        bool empty = false;
        if (!read_name(n.tag) || !read_attrs(n, empty)) return false;
        return empty || read_content(n, depth);
    }

    // comments, declarations and space before or after the root element
    bool skip_misc() {
        for (;;) {
            skip_space();
            if (at("<!--")) {
                if (!skip_past("-->", "unterminated comment")) return false;
            } else if (at("<?")) {
                if (!skip_past("?>", "unterminated <?")) return false;
            } else if (at("<!")) {
                return fail("unsupported <! markup");
            } else {
                return true;
            }
        }
    }

    bool read_document(xml_node &root) {
        if (at("\xef\xbb\xbf")) p += 3;         // UTF-8 byte order mark
        if (!skip_misc()) return false;
        if (p >= end || *p != '<') return fail("not an XML file");
        if (!read_element(root, 0) || !skip_misc()) return false;
        if (p < end) return fail("text after the root element");
        return true;
    }
};

std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool error_at(std::string &err, const xml_node &n, const std::string &msg) {
    char s[48];
    snprintf(s, sizeof(s), "MRA error, line %d:\n", n.line);
    err = s + msg;
    return false;
}

// a number as MiSTer reads it: strtoul(v, NULL, base), but all of v must be the number
bool number(const char *v, int base, uint32_t &out) {
    char *e;
    while (*v == ' ' || *v == '\t') v++;
    if (!*v || *v == '-' || *v == '+') return false;
    unsigned long n = strtoul(v, &e, base);
    while (*e == ' ' || *e == '\t') e++;
    if (*e || n > 0xffffffffUL) return false;
    out = (uint32_t)n;
    return true;
}

bool parse_part(const xml_node &x, mra_part &part, std::string &err) {
    part.crc = 0;
    part.offset = 0;
    part.length = 0;
    part.repeat = 1;
    if (!x.children.empty())
        return error_at(err, x, "<" + x.children[0].tag + "> in <part>");
    for (auto &a : x.attrs) {
        const char *n = a.name.c_str(), *v = a.value.c_str();
        if (strcasecmp(n, "name") == 0) {
            part.name = a.value;
        } else if (strcasecmp(n, "zip") == 0) {
            part.zip = a.value;
        } else if (strcasecmp(n, "crc") == 0) {
            if (!number(v, 16, part.crc)) return error_at(err, x, "bad crc=\"" + a.value + "\"");
        } else if (strcasecmp(n, "offset") == 0) {
            if (!number(v, 0, part.offset)) return error_at(err, x, "bad offset=\"" + a.value + "\"");
        } else if (strcasecmp(n, "length") == 0) {
            if (!number(v, 0, part.length)) return error_at(err, x, "bad length=\"" + a.value + "\"");
        } else if (strcasecmp(n, "repeat") == 0) {
            if (!number(v, 0, part.repeat)) return error_at(err, x, "bad repeat=\"" + a.value + "\"");
        } else if (strcasecmp(n, "map") == 0) {
            return error_at(err, x, "<part map> unsupported");
        }
    }
    // a part without a name is data, as in MiSTer (its crc, zip, offset and length unused)
    if (part.name.empty() && !mra_hex(x.text, part.data))
        return error_at(err, x, "bad hex data in <part>");
    return true;
}

bool parse_rom(const xml_node &x, mra_rom &rom, std::string &err) {
    rom.index = 0;
    for (auto &a : x.attrs) {
        const char *n = a.name.c_str(), *v = a.value.c_str();
        if (strcasecmp(n, "index") == 0) {
            uint32_t i;
            if (!number(v, 10, i) || i > 255) return error_at(err, x, "bad index=\"" + a.value + "\"");
            rom.index = i;
        } else if (strcasecmp(n, "zip") == 0) {
            rom.zip = a.value;
        } else if (strcasecmp(n, "address") == 0) {
            return error_at(err, x, "<rom address> unsupported");
        }
    }
    for (auto &c : x.children) {
        if (strcasecmp(c.tag.c_str(), "part") != 0)
            return error_at(err, c, "<" + c.tag + "> in <rom>\nunsupported");
        mra_part part;
        if (!parse_part(c, part, err)) return false;
        rom.parts.push_back(std::move(part));
    }
    return true;
}

}   // namespace

bool mra_hex(const std::string &text, std::vector<uint8_t> &out) {
    out.clear();
    auto sep = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ','; };
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    size_t i = 0, n = text.size();
    for (;;) {
        while (i < n && sep(text[i])) i++;
        if (i >= n) return true;
        int hi = digit(text[i++]);
        if (hi < 0) return false;
        if (i >= n) {                   // a single digit at the end
            out.push_back(hi);
            return true;
        }
        int lo = digit(text[i++]);
        if (lo < 0) return false;
        out.push_back(hi << 4 | lo);
    }
}

bool mra_parse(const char *text, size_t len, mra_info &mra, std::string &err) {
    mra = mra_info();
    xml_reader r;
    r.p = text;
    r.end = text + len;
    xml_node root;
    if (!r.read_document(root)) {
        err = r.err;
        return false;
    }
    if (strcasecmp(root.tag.c_str(), "misterromdescription") != 0) {
        err = "Not an MRA file:\nno <misterromdescription>";
        return false;
    }

    for (auto &c : root.children) {
        const char *t = c.tag.c_str();
        if (strcasecmp(t, "name") == 0 || strcasecmp(t, "n") == 0) {
            mra.name = trim(c.text);
        } else if (strcasecmp(t, "setname") == 0) {
            mra.setname = trim(c.text);
        } else if (strcasecmp(t, "rbf") == 0) {
            mra.rbf = trim(c.text);
        } else if (strcasecmp(t, "switches") == 0) {
            const char *d = c.attr("default");
            if (d && !mra_hex(d, mra.switches))
                return error_at(err, c, "bad <switches default>");
            if (mra.switches.size() > 8)
                return error_at(err, c, "<switches default>\nlonger than 8 bytes");
        } else if (strcasecmp(t, "rom") == 0) {
            mra_rom rom;
            if (!parse_rom(c, rom, err)) return false;
            if (mra_find_rom(mra, rom.index)) {
                char s[40];
                snprintf(s, sizeof(s), "more than one <rom index=%d>", rom.index);
                return error_at(err, c, s);
            }
            mra.roms.push_back(std::move(rom));
        }
    }
    if (mra.rbf.empty()) {
        err = "MRA has no <rbf>";
        return false;
    }
    return true;
}

const mra_rom *mra_find_rom(const mra_info &mra, int index) {
    for (auto &r : mra.roms)
        if (r.index == index)
            return &r;
    return NULL;
}

uint64_t mra_switches(const mra_info &mra) {
    uint64_t v = 0;
    for (size_t i = 0; i < mra.switches.size() && i < 8; i++)
        v |= (uint64_t)mra.switches[i] << (i * 8);
    return v;
}

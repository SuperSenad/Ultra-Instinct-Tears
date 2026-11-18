// Ultra Instinct Tears setup: zip reading and inflate (see unzip.h). [UIT 0433]
#include "unzip.h"

#include <SDL3/SDL.h>

namespace ut {

// ---------------------------------------------------------------- CRC-32 (the zip polynomial, 0xEDB88320)
uint32_t crc32_update(uint32_t crc, const void *data, size_t n) {
    static uint32_t table[8][256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; i++)
            for (int t = 1; t < 8; t++) table[t][i] = (table[t - 1][i] >> 8) ^ table[0][table[t - 1][i] & 0xFF];
        ready = true;
    }
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (n >= 8) {   // eight bytes a step (slicing-by-8)
        uint32_t a = crc ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
        crc = table[7][a & 0xFF] ^ table[6][(a >> 8) & 0xFF] ^ table[5][(a >> 16) & 0xFF] ^ table[4][a >> 24] ^
              table[3][p[4]] ^ table[2][p[5]] ^ table[1][p[6]] ^ table[0][p[7]];
        p += 8, n -= 8;
    }
    while (n--) crc = table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// ---------------------------------------------------------------- inflate
namespace {
constexpr int kFast = 10;   // codes up to this length decode with one table look-up; longer ones bit by bit

struct Huff {
    uint16_t count[16];     // codes of each length
    uint16_t sym[320];      // symbols in canonical order
    uint16_t fast[1 << kFast];   // (length << 9) | symbol for the next kFast input bits; 0 = longer code (slow path)
};

struct In {
    const uint8_t *p;
    size_t n, pos = 0;
    uint64_t buf = 0;
    int cnt = 0;
    bool bad = false;
    void fill() {
        while (cnt <= 56) {
            if (pos < n) buf |= (uint64_t)p[pos++] << cnt;
            else if (pos < n + 32) pos++;   // past the end: zero bits (the last code may be read ahead of)
            else { bad = true; return; }   // far past it: damaged data
            cnt += 8;
        }
    }
    uint32_t peek(int k) { if (cnt < k) fill(); return (uint32_t)(buf & ((1ull << k) - 1)); }
    void drop(int k) { buf >>= k; cnt -= k; }
    uint32_t bits(int k) { if (!k) return 0; uint32_t v = peek(k); drop(k); return v; }
};

int build(Huff &h, const uint8_t *len, int n) {   // >= 0 ok (incomplete codes are allowed for a single code), -1 bad
    SDL_memset(h.count, 0, sizeof h.count);
    for (int s = 0; s < n; s++) h.count[len[s]]++;
    if (h.count[0] == n) { SDL_memset(h.fast, 0, sizeof h.fast); return 0; }
    int left = 1;
    for (int l = 1; l < 16; l++) { left <<= 1; left -= h.count[l]; if (left < 0) return -1; }
    uint16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h.count[l];
    for (int s = 0; s < n; s++) if (len[s]) h.sym[offs[len[s]]++] = (uint16_t)s;
    // the look-up table: canonical codes (RFC 1951 3.2.2), bit-reversed because the stream is read from the low bit
    SDL_memset(h.fast, 0, sizeof h.fast);
    uint32_t code = 0;
    int k = 0;
    for (int l = 1; l <= 15; l++) {
        for (int c = 0; c < h.count[l]; c++, k++, code++) {
            if (l > kFast) continue;
            uint32_t r = 0;
            for (int b = 0; b < l; b++) r |= ((code >> b) & 1) << (l - 1 - b);
            for (uint32_t f = r; f < (1u << kFast); f += 1u << l) h.fast[f] = (uint16_t)(l << 9 | h.sym[k]);
        }
        code <<= 1;
    }
    return left;
}

int decode(In &in, const Huff &h) {
    uint16_t f = h.fast[in.peek(kFast)];
    if (f) { in.drop(f >> 9); return f & 511; }
    int code = 0, first = 0, index = 0;   // the slow path: one bit at a time, MSB-first per code
    for (int l = 1; l < 16; l++) {
        code |= (int)in.bits(1);
        int c = h.count[l];
        if (code - c < first) return h.sym[index + (code - first)];
        index += c, first += c;
        first <<= 1, code <<= 1;
        if (in.bad) return -1;
    }
    return -1;
}

const uint16_t kLBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLExt[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                             4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDExt[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(In &in, uint8_t *out, size_t on, size_t &op, const Huff &lc, const Huff &dc) {
    for (;;) {
        int s = decode(in, lc);
        if (s < 0 || in.bad) return false;
        if (s < 256) {
            if (op >= on) return false;
            out[op++] = (uint8_t)s;
        } else if (s == 256) return true;
        else {
            s -= 257;
            if (s >= 29) return false;
            size_t len = kLBase[s] + in.bits(kLExt[s]);
            int ds = decode(in, dc);
            if (ds < 0 || ds >= 30) return false;
            size_t dist = kDBase[ds] + in.bits(kDExt[ds]);
            if (dist > op || len > on - op) return false;
            uint8_t *q = out + op, *from = q - dist;
            if (dist >= len) SDL_memcpy(q, from, len);
            else for (size_t k = 0; k < len; k++) q[k] = from[k];
            op += len;
        }
    }
}
}  // namespace

bool inflate_raw(const uint8_t *src, size_t n, uint8_t *out, size_t out_n) {
    In in{src, n};
    size_t op = 0;
    static Huff *fixl = nullptr, *fixd = nullptr;   // the fixed codes, built once
    if (!fixl) {
        Huff *l = (Huff *)SDL_malloc(sizeof(Huff)), *dd = (Huff *)SDL_malloc(sizeof(Huff));
        uint8_t len[288];
        int s = 0;
        for (; s < 144; s++) len[s] = 8;
        for (; s < 256; s++) len[s] = 9;
        for (; s < 280; s++) len[s] = 7;
        for (; s < 288; s++) len[s] = 8;
        build(*l, len, 288);
        for (s = 0; s < 30; s++) len[s] = 5;
        build(*dd, len, 30);
        fixl = l, fixd = dd;
    }
    Huff *lc = (Huff *)SDL_malloc(sizeof(Huff)), *dc = (Huff *)SDL_malloc(sizeof(Huff));
    bool ok = true, last = false;
    while (ok && !last) {
        last = in.bits(1);
        int type = (int)in.bits(2);
        if (type == 0) {   // stored: the rest of this byte is dropped, then LEN, NLEN and the bytes
            in.drop(in.cnt & 7);
            uint32_t len = in.bits(16), nlen = in.bits(16);
            if ((len ^ 0xFFFF) != nlen || len > out_n - op) { ok = false; break; }
            for (uint32_t k = 0; k < len; k++) out[op++] = (uint8_t)in.bits(8);
        } else if (type == 1) {
            ok = codes(in, out, out_n, op, *fixl, *fixd);
        } else if (type == 2) {
            int nlen = (int)in.bits(5) + 257, ndist = (int)in.bits(5) + 1, ncode = (int)in.bits(4) + 4;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t lens[320] = {0};
            if (nlen > 286 || ndist > 30) { ok = false; break; }
            for (int k = 0; k < ncode; k++) lens[order[k]] = (uint8_t)in.bits(3);
            if (build(*lc, lens, 19) != 0) { ok = false; break; }   // the code-length code must be complete
            int k = 0;
            while (k < nlen + ndist) {
                int s = decode(in, *lc);
                if (s < 0 || in.bad) { ok = false; break; }
                if (s < 16) { lens[k++] = (uint8_t)s; continue; }
                uint8_t v = 0;
                int rep;
                if (s == 16) { if (k == 0) { ok = false; break; } v = lens[k - 1]; rep = 3 + (int)in.bits(2); }
                else if (s == 17) rep = 3 + (int)in.bits(3);
                else rep = 11 + (int)in.bits(7);
                if (k + rep > nlen + ndist) { ok = false; break; }
                while (rep--) lens[k++] = v;
            }
            if (!ok) break;
            if (lens[256] == 0) { ok = false; break; }
            int el = build(*lc, lens, nlen), ed = build(*dc, lens + nlen, ndist);
            if (el < 0 || (el > 0 && nlen - lc->count[0] != 1) || ed < 0 || (ed > 0 && ndist - dc->count[0] != 1)) { ok = false; break; }
            ok = codes(in, out, out_n, op, *lc, *dc);
        } else ok = false;
        if (in.bad) ok = false;
    }
    SDL_free(lc), SDL_free(dc);
    return ok && op == out_n;
}

// ---------------------------------------------------------------- zip
namespace {
uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
bool read_at(SDL_IOStream *io, int64_t at, void *dst, size_t n) {
    return SDL_SeekIO(io, at, SDL_IO_SEEK_SET) == at && SDL_ReadIO(io, dst, n) == n;
}
}  // namespace

void zip_close(Zip *z) {
    if (z->io) SDL_CloseIO(z->io);
    z->io = nullptr;
    z->entries.clear();
}

bool zip_open(Zip *z, const mini::string &path, int64_t start, int64_t end, mini::string *err) {
    zip_close(z);
    z->io = SDL_IOFromFile(path.c_str(), "rb");
    if (!z->io) { *err = "cannot open " + path + ": " + SDL_GetError(); return false; }
    int64_t size = SDL_GetIOSize(z->io);
    if (end < 0 || end > size) end = size;
    // the end record: in the last 64 KiB + 22 bytes (it may end in a comment)
    int64_t from = mini::max(start, end - (int64_t)(65535 + 22));
    mini::string tail;
    tail.resize((size_t)(end - from));
    if (end - from < 22 || !read_at(z->io, from, &tail[0], tail.size())) { *err = "not a zip file (too short)"; zip_close(z); return false; }
    const uint8_t *t = (const uint8_t *)tail.data();
    int64_t eocd = -1;
    for (int64_t k = (int64_t)tail.size() - 22; k >= 0; k--)
        if (le32(t + k) == 0x06054b50) { eocd = k; break; }
    if (eocd < 0) { *err = "not a zip file (no end record)"; zip_close(z); return false; }
    uint64_t count = le16(t + eocd + 10), cd_size = le32(t + eocd + 12), cd_off = le32(t + eocd + 16);
    int64_t eocd_at = from + eocd, cd_end = eocd_at;
    if (eocd >= 20 && le32(t + eocd - 20) == 0x07064b50) {   // zip64: the locator before the end record
        uint8_t r[56];
        int64_t rec = eocd_at - 20 - 56;
        if (rec < start || !read_at(z->io, rec, r, 56) || le32(r) != 0x06064b50) { *err = "damaged zip64 end record"; zip_close(z); return false; }
        count = le64(r + 32), cd_size = le64(r + 40), cd_off = le64(r + 48);
        cd_end = rec;
    }
    z->base = cd_end - (int64_t)cd_size - (int64_t)cd_off;   // where the zip really starts in this file
    if (z->base < start || cd_size > (uint64_t)(cd_end - start)) { *err = "damaged zip (central directory out of range)"; zip_close(z); return false; }
    mini::string cd;
    cd.resize((size_t)cd_size);
    if (cd_size && !read_at(z->io, z->base + (int64_t)cd_off, &cd[0], cd.size())) { *err = "cannot read the zip's directory"; zip_close(z); return false; }
    const uint8_t *p = (const uint8_t *)cd.data(), *e = p + cd.size();
    for (uint64_t i = 0; i < count; i++) {
        if (e - p < 46 || le32(p) != 0x02014b50) { *err = "damaged zip directory"; zip_close(z); return false; }
        ZipEntry x;
        x.method = le16(p + 10);
        x.crc = le32(p + 16);
        x.csize = le32(p + 20), x.usize = le32(p + 24);
        uint16_t nl = le16(p + 28), xl = le16(p + 30), cl = le16(p + 32);
        x.local = le32(p + 42);
        if (e - p < 46 + nl + xl + cl) { *err = "damaged zip directory"; zip_close(z); return false; }
        x.name = mini::string((const char *)p + 46, nl);
        const uint8_t *xp = p + 46 + nl, *xe = xp + xl;
        while (xe - xp >= 4) {   // zip64 sizes and offset, in this order, only for the fields that are 0xFFFFFFFF
            uint16_t id = le16(xp), sz = le16(xp + 2);
            const uint8_t *v = xp + 4;
            if (id == 1) {
                if (x.usize == 0xFFFFFFFFu && sz >= 8) x.usize = le64(v), v += 8, sz -= 8;
                if (x.csize == 0xFFFFFFFFu && sz >= 8) x.csize = le64(v), v += 8, sz -= 8;
                if (x.local == 0xFFFFFFFFu && sz >= 8) x.local = le64(v);
                break;
            }
            xp += 4 + sz;
        }
        x.dir = !x.name.empty() && (x.name.back() == '/' || x.name.back() == '\\');
        z->entries.push_back(x);
        p += 46 + nl + xl + cl;
    }
    return true;
}

bool zip_read(Zip *z, const ZipEntry &e, mini::string *out, mini::string *err) {
    uint8_t h[30];
    if (!read_at(z->io, z->base + (int64_t)e.local, h, 30) || le32(h) != 0x04034b50) { *err = e.name + ": damaged entry"; return false; }
    int64_t data = z->base + (int64_t)e.local + 30 + le16(h + 26) + le16(h + 28);
    if (e.usize > ((uint64_t)1 << 32) * 4) { *err = e.name + ": too large"; return false; }
    out->resize((size_t)e.usize);
    if (e.method == 0) {
        if (e.csize != e.usize || (e.usize && !read_at(z->io, data, &(*out)[0], (size_t)e.usize))) { *err = e.name + ": cannot read"; return false; }
    } else if (e.method == 8) {
        mini::string c;
        c.resize((size_t)e.csize);
        if (e.csize && !read_at(z->io, data, &c[0], c.size())) { *err = e.name + ": cannot read"; return false; }
        if (!inflate_raw((const uint8_t *)c.data(), c.size(), e.usize ? (uint8_t *)&(*out)[0] : nullptr, (size_t)e.usize)) {
            *err = e.name + ": damaged (cannot unpack)"; return false;
        }
    } else { *err = e.name + ": packed in a way this installer cannot read (method " + mini::to_string((int)e.method) + ")"; return false; }
    if (crc32_update(0, out->data(), out->size()) != e.crc) { *err = e.name + ": damaged (checksum)"; return false; }
    return true;
}

mini::string zip_safe_name(const mini::string &name) {
    mini::string n;
    for (char c : name) n += c == '\\' ? '/' : c;
    if (n.empty() || n[0] == '/' || (n.size() > 1 && n[1] == ':')) return {};
    mini::string out, part;
    for (size_t k = 0; k <= n.size(); k++) {
        if (k == n.size() || n[k] == '/') {
            if (part == "..") return {};
            if (!part.empty() && part != ".") out += (out.empty() ? "" : "/") + part;
            part.clear();
        } else {
            unsigned char c = (unsigned char)n[k];
            if (c < 32 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return {};
            part += n[k];
        }
    }
    return out;
}

}  // namespace ut

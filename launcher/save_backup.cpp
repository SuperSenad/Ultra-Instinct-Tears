// [UIT 0347] Ultra Instinct Tears launcher: a zip of the save folder before each Play. See save_backup.h.
//
// The zip is "stored" (no compression): the launcher has no deflate, the save files are a few MB, and any unzip tool,
// Windows Explorer included, opens it. The archive comment carries a fingerprint of the files it holds
// ("uit-saves <16 hex digits>"), so the next Play can tell nothing changed without reading the saves again.
#include "save_backup.h"

#include <SDL3/SDL.h>

#include "platform.h"

namespace ut {

namespace {

struct Entry {
    mini::string rel;      // '/'-separated, relative to the save folder
    mini::string full;
    Uint64 size = 0;
    SDL_Time mtime = 0;
};

const Uint64 kMaxBytes = 2000ull * 1000 * 1000;   // a zip without the 64-bit extensions stops at 4 GB
const int kMaxFiles = 20000;

bool same_ci(const char *a, const char *b) { return SDL_strcasecmp(a, b) == 0; }

struct Walk {
    mini::vector<Entry> files;
    mini::string dest;     // the backups folder, never walked into (a save folder set to the game folder)
    Uint64 bytes = 0;
    bool too_big = false;
};

struct Lister {
    mini::vector<mini::string> names;
};

SDL_EnumerationResult SDLCALL list_cb(void *u, const char *, const char *name) {
    static_cast<Lister *>(u)->names.push_back(name);
    return SDL_ENUM_CONTINUE;
}

void walk_dir(Walk &w, const mini::string &dir, const mini::string &rel, int depth) {
    if (w.too_big || depth > 32) return;
    Lister l;
    if (!SDL_EnumerateDirectory(dir.c_str(), list_cb, &l)) return;
    for (const mini::string &name : l.names) {
        if (depth == 0 && (same_ci(name.c_str(), "temp") || same_ci(name.c_str(), "cache") || same_ci(name.c_str(), "romfix-cache")))
            continue;
        mini::string full = join(dir, name), r = rel.empty() ? name : rel + "/" + name;
        if (same_ci(full.c_str(), w.dest.c_str())) continue;
        SDL_PathInfo pi;
        if (!SDL_GetPathInfo(full.c_str(), &pi)) continue;
        if (pi.type == SDL_PATHTYPE_DIRECTORY) {
            walk_dir(w, full, r, depth + 1);
        } else if (pi.type == SDL_PATHTYPE_FILE) {
            if ((int)w.files.size() >= kMaxFiles || (w.bytes += pi.size) > kMaxBytes) { w.too_big = true; return; }
            Entry e;
            e.rel = r, e.full = full, e.size = pi.size, e.mtime = pi.modify_time;
            w.files.push_back(e);
        }
    }
}

int SDLCALL by_rel(void *u, const void *a, const void *b) {
    const mini::vector<Entry> &f = *static_cast<const mini::vector<Entry> *>(u);
    return SDL_strcmp(f[*(const int *)a].rel.c_str(), f[*(const int *)b].rel.c_str());
}

Uint64 fnv(Uint64 h, const void *p, size_t n) {
    const unsigned char *c = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) h = (h ^ c[i]) * 0x100000001b3ull;
    return h;
}

Uint32 crc_table[256];
void crc_init() {
    if (crc_table[1]) return;
    for (Uint32 i = 0; i < 256; i++) {
        Uint32 c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}
Uint32 crc_add(Uint32 crc, const unsigned char *p, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put16(mini::string &o, unsigned v) { o += (char)(v & 0xFF), o += (char)(v >> 8 & 0xFF); }
void put32(mini::string &o, Uint32 v) { put16(o, v & 0xFFFF), put16(o, v >> 16); }

void dos_time(SDL_Time t, unsigned *time, unsigned *date) {
    SDL_DateTime dt;
    if (!SDL_TimeToDateTime(t, &dt, true) || dt.year < 1980) { *time = 0, *date = (1 << 5) | 1; return; }   // 1980-01-01
    *time = (unsigned)(dt.hour << 11 | dt.minute << 5 | dt.second / 2);
    *date = (unsigned)((dt.year - 1980) << 9 | dt.month << 5 | dt.day);
}

// The fingerprint in a backup's comment ("" when it has none).
mini::string comment_of(const mini::string &zip) {
    SDL_IOStream *f = SDL_IOFromFile(zip.c_str(), "rb");
    if (!f) return "";
    Sint64 size = SDL_GetIOSize(f);
    char buf[64] = {0};
    size_t got = 0;
    if (size >= 22 + 26 && SDL_SeekIO(f, size - 26, SDL_IO_SEEK_SET) >= 0) got = SDL_ReadIO(f, buf, 26);
    SDL_CloseIO(f);
    if (got != 26 || SDL_strncmp(buf, "uit-saves ", 10)) return "";
    return mini::string(buf + 10, 16);
}

// Backups in dest, oldest first (only names the launcher makes: saves-YYYYMMDD-HHMMSS[-N].zip).
mini::vector<mini::string> backups_in(const mini::string &dest) {
    Lister l;
    mini::vector<mini::string> out;
    if (!SDL_EnumerateDirectory(dest.c_str(), list_cb, &l)) return out;
    for (const mini::string &n : l.names) {
        const char *s = n.c_str();
        size_t len = n.size();
        if (len < 25 || SDL_strncmp(s, "saves-", 6) || SDL_strcasecmp(s + len - 4, ".zip")) continue;
        bool ok = s[14] == '-';
        for (int i = 6; i < 21 && ok; i++) ok = i == 14 || (s[i] >= '0' && s[i] <= '9');
        for (size_t i = 21; i < len - 4 && ok; i++) ok = i == 21 ? s[i] == '-' : (s[i] >= '0' && s[i] <= '9');
        if (ok) out.push_back(n.substr(0, len - 4));   // compared without ".zip": saves-X sorts before saves-X-2
    }
    for (size_t i = 1; i < out.size(); i++)   // a handful of names
        for (size_t j = i; j > 0 && SDL_strcmp(out[j - 1].c_str(), out[j].c_str()) > 0; j--) mini::swap(out[j - 1], out[j]);
    for (auto &n : out) n += ".zip";
    return out;
}

}  // namespace

SaveBackup save_backup(const mini::string &saves, const mini::string &dest, int keep, double budget_s) {
    SaveBackup r;
    if (keep <= 0) return r;
    Uint64 t0 = SDL_GetTicksNS();
    auto late = [&] { return (double)(SDL_GetTicksNS() - t0) / 1e9 > budget_s; };
    auto skip = [&](const mini::string &why) { r.result = SaveBackup::Skipped, r.why = why; return r; };
    SDL_PathInfo pi;
    if (saves.empty() || !SDL_GetPathInfo(saves.c_str(), &pi) || pi.type != SDL_PATHTYPE_DIRECTORY) {
        r.result = SaveBackup::NoSaves;
        return r;
    }
    Walk w;
    w.dest = dest;
    walk_dir(w, saves, "", 0);
    if (w.too_big) return skip("the save folder is over 2 GB or 20000 files");
    if (w.files.empty()) { r.result = SaveBackup::NoSaves; return r; }
    mini::vector<int> order;
    for (int i = 0; i < (int)w.files.size(); i++) order.push_back(i);
    SDL_qsort_r(&order[0], order.size(), sizeof(int), by_rel, &w.files);
    Uint64 h = 0xcbf29ce484222325ull;
    for (int i : order) {
        const Entry &e = w.files[i];
        h = fnv(h, e.rel.c_str(), e.rel.size() + 1);
        h = fnv(h, &e.size, sizeof e.size);
        h = fnv(h, &e.mtime, sizeof e.mtime);
    }
    char fp[17];
    SDL_snprintf(fp, sizeof fp, "%016llx", (unsigned long long)h);

    SDL_CreateDirectory(dest.c_str());
    mini::vector<mini::string> old = backups_in(dest);
    auto prune = [&] {   // the oldest beyond the newest `keep` (also when `keep` was lowered and nothing changed)
        mini::vector<mini::string> all = backups_in(dest);
        for (size_t i = 0; i + (size_t)keep < all.size(); i++)
            if (SDL_RemovePath(join(dest, all[i]).c_str())) r.removed++;
    };
    if (!old.empty() && comment_of(join(dest, old.back())) == fp) {
        r.result = SaveBackup::Unchanged, r.file = join(dest, old.back());
        prune();
        return r;
    }

    mini::string base = join(dest, "saves-" + timestamp()), zip = base + ".zip";
    for (int k = 2; file_exists(zip) && k < 100; k++) zip = base + "-" + mini::to_string(k) + ".zip";
    mini::string part = zip + ".part";
    SDL_IOStream *out = SDL_IOFromFile(part.c_str(), "wb");
    if (!out) return skip(mini::string("cannot write in the backups folder (") + SDL_GetError() + ")");
    crc_init();
    const size_t CHUNK = 1 << 20;
    unsigned char *buf = (unsigned char *)SDL_malloc(CHUNK);
    mini::string central;
    mini::string err;
    Uint64 offset = 0;
    for (int i : order) {
        if (!buf) { err = "out of memory"; break; }
        if (late()) { err = "it took longer than " + mini::to_string((int)budget_s) + " s"; break; }
        const Entry &e = w.files[i];
        SDL_IOStream *in = SDL_IOFromFile(e.full.c_str(), "rb");
        if (!in) { err = "cannot read " + e.rel + " (" + SDL_GetError() + ")"; break; }
        unsigned tm, dt;
        dos_time(e.mtime, &tm, &dt);
        mini::string hdr;
        put32(hdr, 0x04034b50), put16(hdr, 20), put16(hdr, 0x0800), put16(hdr, 0), put16(hdr, tm), put16(hdr, dt);
        put32(hdr, 0), put32(hdr, 0), put32(hdr, 0);   // crc and sizes: filled in once the file is read
        put16(hdr, (unsigned)e.rel.size()), put16(hdr, 0);
        hdr += e.rel;
        Uint64 here = offset;
        bool ok = SDL_WriteIO(out, hdr.data(), hdr.size()) == hdr.size();
        Uint32 crc = 0;
        Uint64 n = 0;
        while (ok) {
            size_t got = SDL_ReadIO(in, buf, CHUNK);
            if (!got) { ok = SDL_GetIOStatus(in) == SDL_IO_STATUS_EOF; break; }
            crc = crc_add(crc, buf, got);
            n += got;
            ok = n <= 0xFFFFFFFFull && SDL_WriteIO(out, buf, got) == got;
        }
        SDL_CloseIO(in);
        if (!ok) { err = "cannot copy " + e.rel; break; }
        offset += hdr.size() + n;
        mini::string fix;
        put32(fix, crc), put32(fix, (Uint32)n), put32(fix, (Uint32)n);
        if (SDL_SeekIO(out, (Sint64)here + 14, SDL_IO_SEEK_SET) < 0 || SDL_WriteIO(out, fix.data(), 12) != 12 ||
            SDL_SeekIO(out, (Sint64)offset, SDL_IO_SEEK_SET) < 0) { err = "cannot write the zip"; break; }
        if (offset > 0xFFFFFFFFull) { err = "the zip would pass 4 GB"; break; }
        put32(central, 0x02014b50), put16(central, 20), put16(central, 20), put16(central, 0x0800), put16(central, 0);
        put16(central, tm), put16(central, dt), put32(central, crc), put32(central, (Uint32)n), put32(central, (Uint32)n);
        put16(central, (unsigned)e.rel.size()), put16(central, 0), put16(central, 0), put16(central, 0), put16(central, 0);
        put32(central, 0), put32(central, (Uint32)here);
        central += e.rel;
    }
    SDL_free(buf);
    if (err.empty()) {
        mini::string end;
        mini::string note = mini::string("uit-saves ") + fp;
        put32(end, 0x06054b50), put16(end, 0), put16(end, 0), put16(end, (unsigned)order.size()), put16(end, (unsigned)order.size());
        put32(end, (Uint32)central.size()), put32(end, (Uint32)offset), put16(end, (unsigned)note.size());
        end += note;
        if (order.size() > 0xFFFF || SDL_WriteIO(out, central.data(), central.size()) != central.size() ||
            SDL_WriteIO(out, end.data(), end.size()) != end.size())
            err = "cannot write the zip";
    }
    if (!SDL_CloseIO(out) && err.empty()) err = mini::string("cannot finish the zip (") + SDL_GetError() + ")";
    if (err.empty() && !SDL_RenamePath(part.c_str(), zip.c_str())) err = mini::string("cannot name the zip (") + SDL_GetError() + ")";
    if (!err.empty()) {
        SDL_RemovePath(part.c_str());   // only the unfinished zip: the saves are never touched
        return skip(err);
    }
    r.result = SaveBackup::Made, r.file = zip;
    prune();
    return r;
}

}  // namespace ut

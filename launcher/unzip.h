// Ultra Instinct Tears setup: reading zip files (stored and deflate entries, zip64) with a small inflate of its own,
// so the installer needs no other program to unpack the game's source package and the build tools.
// [UIT 0433] Written for the installer (launcher/installer.inc); no third-party code.
#pragma once
#include <stdint.h>
#include "mini.h"

struct SDL_IOStream;

namespace ut {

struct ZipEntry {
    mini::string name;          // as stored, '/' separated
    uint64_t csize = 0, usize = 0, local = 0;   // local: the local header's offset from the zip's start
    uint32_t crc = 0;
    uint16_t method = 0;
    bool dir = false;
};

struct Zip {
    SDL_IOStream *io = nullptr;
    int64_t base = 0;           // where the zip starts in the file (a payload appended to an exe starts after it)
    mini::vector<ZipEntry> entries;
};

// Opens the zip that ends at file offset end (-1: the end of the file) and starts at or after `start`. The zip's own
// offsets are relative to its first byte, which is found from the end record (as self-extracting archives do).
bool zip_open(Zip *z, const mini::string &path, int64_t start, int64_t end, mini::string *err);
void zip_close(Zip *z);
// The whole entry, unpacked and checked against its CRC-32.
bool zip_read(Zip *z, const ZipEntry &e, mini::string *out, mini::string *err);
// A name that stays inside the folder it is unpacked into ('\\' counts as '/'; no "..", no drive or root): "" if not.
mini::string zip_safe_name(const mini::string &name);

// Raw DEFLATE (RFC 1951) into out, which must already have the unpacked size. False on damaged data.
bool inflate_raw(const uint8_t *in, size_t n, uint8_t *out, size_t out_n);
uint32_t crc32_update(uint32_t crc, const void *data, size_t n);   // start with 0

}  // namespace ut

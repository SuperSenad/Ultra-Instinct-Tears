// Ultra Instinct Tears setup: SHA-256 (FIPS 180-4), to check the player's game files are version 1.4.3 before
// anything is built. [UIT 0433]
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "mini.h"

namespace ut {

struct Sha256 {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len = 0;
    size_t fill = 0;
    Sha256();
    void update(const void *data, size_t n);
    mini::string hex();   // finishes: 64 lowercase hex digits
};

// The SHA-256 of a whole file as hex ("" if it cannot be read). *stop (optional) set to true ends it early ("").
mini::string sha256_file(const mini::string &path, volatile int *stop = nullptr);

}  // namespace ut

// [UIT 0433] The installer's zip reader, inflate and SHA-256 (launcher/unzip.cpp, launcher/sha256.cpp) against
// Python's zipfile / zlib / hashlib (tools/tests/test_installer.py builds and drives this):
//   test_unzip list ZIP [START]      every entry: name, size, CRC-32 and SHA-256 of the unpacked bytes (tab-separated)
//   test_unzip sha FILE              the file's SHA-256
//   test_unzip safe NAME             zip_safe_name(NAME) ("-" when refused)
#include <SDL3/SDL.h>
#include <stdio.h>
#include "../../launcher/sha256.h"
#include "../../launcher/unzip.h"

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    mini::string cmd = argv[1];
    if (cmd == "sha") { printf("%s\n", ut::sha256_file(argv[2]).c_str()); return 0; }
    if (cmd == "safe") { mini::string s = ut::zip_safe_name(argv[2]); printf("%s\n", s.empty() ? "-" : s.c_str()); return 0; }
    ut::Zip z;
    mini::string err;
    if (!ut::zip_open(&z, argv[2], argc > 3 ? atoll(argv[3]) : 0, -1, &err)) { printf("ERROR\t%s\n", err.c_str()); return 1; }
    int bad = 0;
    for (auto &e : z.entries) {
        if (e.dir) continue;
        mini::string data;
        if (!ut::zip_read(&z, e, &data, &err)) { printf("ERROR\t%s\n", err.c_str()); bad++; continue; }
        ut::Sha256 s;
        s.update(data.data(), data.size());
        printf("%s\t%llu\t%08x\t%s\n", e.name.c_str(), (unsigned long long)data.size(), ut::crc32_update(0, data.data(), data.size()), s.hex().c_str());
    }
    ut::zip_close(&z);
    return bad ? 1 : 0;
}

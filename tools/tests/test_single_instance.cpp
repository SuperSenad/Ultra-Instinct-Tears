// [UIT 0272] launcher/single_instance.cpp: the folder hash and the object names (the Win32 part: check_windows_toolchain
// builds it; the hand-over itself is checked by hand, docs/testing-list.md).
// build: c++ -O2 -std=c++17 -fno-exceptions -fno-rtti -nostdlib++ -Ilauncher tools/tests/test_single_instance.cpp \
//            launcher/single_instance.cpp -o /tmp/test_single_instance && /tmp/test_single_instance
#include "single_instance.h"

#include <stdio.h>
#include <string.h>

using namespace ut;
static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main() {
    // the same folder however it is written: case, slashes, a trailing separator
    uint32_t h = si_dir_hash("C:\\Games\\UltraInstinctTears");
    CHECK(h == si_dir_hash("c:\\games\\ultrainstincttears"));
    CHECK(h == si_dir_hash("C:/Games/UltraInstinctTears/"));
    CHECK(h == si_dir_hash("C:\\Games\\UltraInstinctTears\\\\"));
    // other folders: other launchers
    CHECK(h != si_dir_hash("C:\\Games\\UltraInstinctTears2"));
    CHECK(h != si_dir_hash("D:\\Games\\UltraInstinctTears"));
    CHECK(si_dir_hash("") == 2166136261u);   // FNV-1a's start
    CHECK(si_dir_hash("a") == 0xe40c292cu);   // FNV-1a 32 of "a"
    // the names: Local\ (this session), 8 hex digits, the event's suffix
    mini::string m = si_name("C:\\Games\\UltraInstinctTears", nullptr), e = si_name("C:/games/ultrainstincttears/", "-raise");
    CHECK(!strncmp(m.c_str(), "Local\\UltraInstinctTearsLauncher-", 33) && m.size() == 33 + 8);
    CHECK(e == m + "-raise");
    char want[16];
    snprintf(want, sizeof want, "%08x", h);
    CHECK(!strcmp(m.c_str() + 33, want));
#ifndef _WIN32
    // not Windows: no check, every copy runs (and nothing to raise)
    CHECK(si_claim("/tmp/x", false, 0) == SI_ALONE && !si_raise_requested());
    si_release();
#endif
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
    return fails != 0;
}

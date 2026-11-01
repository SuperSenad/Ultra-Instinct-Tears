// [UIT 0070] launcher gating of the lift features (launcher/model.cpp) against the real launcher/settings.ini.
// build: c++ -std=c++17 -fno-exceptions -fno-rtti -Ilauncher -I<SDL3>/include tools/tests/test_launcher_lift.cpp
//        launcher/model.cpp -L<SDL3>/lib -lSDL3 -o test_launcher_lift && ./test_launcher_lift launcher/settings.ini
#include "model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace ut;
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const Setting *find(const mini::vector<Setting> &v, const char *k) {
    for (const auto &s : v) if (s.key == k) return &s;
    return nullptr;
}
static bool has(const mini::string &s, const char *sub) { return strstr(s.c_str(), sub) != nullptr; }

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: test_launcher_lift settings.ini\n"); return 2; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { printf("cannot read %s\n", argv[1]); return 2; }
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, fp);
    fclose(fp);
    mini::string err;
    auto schema = parse_schema(mini::string(buf, n), &err);
    CHECK(err.empty());
    const Setting *q = find(schema, "TOTK_QUALITY_REDUCTION"), *l = find(schema, "TOTK_LOD_IMPROVE"),
                  *sh = find(schema, "TOTK_SHADOW_RES"), *fx = find(schema, "TOTK_GAME_FXAA"),
                  *pr = find(schema, "TOTK_LIFT_PROBE");
    CHECK(q && l && sh && fx && pr);
    if (!(q && l && sh && fx && pr)) return 1;
    CHECK(q->lift == "quality" && l->lift == "lod" && sh->lift == "shadow");
    CHECK(fx->lift.empty() && pr->lift.empty());                 // the GPU FXAA path and the probe work in any build
    CHECK(q->def == "-" && l->def == "off" && sh->def == "-" && fx->def == "-" && pr->def == "on");   // safe defaults
    CHECK(q->group == "Detail and pop-in" && sh->group == "Detail and pop-in" && pr->group == "Troubleshooting");
    // the probe's path quoted in the greyed-out text must be the real entry
    CHECK(pr->tab == "advanced" && pr->name == "Shadow-size check");

    LiftFeatures none = parse_lift_features("");                 // a build lifted before 0070
    CHECK(!none.known);
    CHECK(has(lift_unavailable(*q, none), "run tools/setup.py again"));
    CHECK(lift_unavailable(*fx, none).empty());

    LiftFeatures f = parse_lift_features("# scan\r\nbuild=277178b7dba1b6d4\r\nquality=1\r\nfxaa=0\r\n"
                                         "fxaa_note=2 matches\r\nlod=0\r\nlod_note=build id differs\r\nshadow=0\r\n"
                                         "shadow_note=no unique site\r\nshadow_probes=3\r\n");
    CHECK(f.known);
    CHECK(lift_unavailable(*q, f).empty());
    CHECK(has(lift_unavailable(*l, f), "build id differs"));
    mini::string s = lift_unavailable(*sh, f);
    CHECK(has(s, "3 possible places") && has(s, "Advanced > Troubleshooting > Shadow-size check"));

    LiftFeatures g = parse_lift_features("build=x\nshadow=0\nshadow_probes=0\n");
    CHECK(has(lift_unavailable(*sh, g), "the game code was not found"));
    LiftFeatures h = parse_lift_features("build=x\nshadow=1\nlod=1\n");
    CHECK(lift_unavailable(*sh, h).empty() && lift_unavailable(*l, h).empty());
    CHECK(has(lift_unavailable(*q, h), "Unavailable in this build"));   // missing key = not found

    // the shadow choices are exactly what src/host/uit_display.c accepts
    const char *want[] = {"-", "512", "1024", "2048", "4096"};
    CHECK(sh->choices.size() == 5);
    for (size_t i = 0; i < sh->choices.size() && i < 5; i++) CHECK(sh->choices[i].value == want[i]);
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("all ok\n");
    return 0;
}

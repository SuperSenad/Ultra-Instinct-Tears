// [UIT 0251] the launcher's settings search matcher (launcher/model.cpp) against the real launcher/settings.ini.
// build: c++ -std=c++17 -fno-exceptions -fno-rtti -Ilauncher -I<SDL3>/include tools/tests/test_launcher_search.cpp
//        launcher/model.cpp -L<SDL3>/lib -lSDL3 -o test_launcher_search && ./test_launcher_search launcher/settings.ini
#include "model.h"
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

using namespace ut;
static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static Setting mk(const char *name, const char *summary, const char *detail, const char *key) {
    Setting s;
    s.name = name, s.summary = summary, s.detail = detail, s.key = key, s.group = "CPU";
    return s;
}
static int score(const Setting &s, const char *q) { return search_score(search_text(s), search_words(q)); }

int main(int argc, char **argv) {
    // normalising: lower case, punctuation to spaces, same length
    CHECK(search_norm("P-Cores (Big)") == "p cores  big ");
    CHECK(search_norm("TOTK_VK_PCORES") == "totk vk pcores");
    // words: split, lower case, punctuation ignored, duplicates once
    auto w = search_words("  CPU+  fps, FPS ");
    CHECK(w.size() == 2 && w[0] == "cpu" && w[1] == "fps");
    CHECK(search_words("").empty() && search_words(" +-/ ").empty());

    Setting a = mk("Frame rate limit", "Caps the frame rate at 30 or 60 FPS", "Uses the CPU less when capped.", "TOTK_FPS");
    Setting b = mk("Shader warm-up", "Compiles shaders before they are needed", "Fewer hitches; the first frame rate drop is shorter.", "TOTK_WARM_SHADERS");
    Setting c = mk("P-cores only", "Keeps the game threads on performance cores", "", "TOTK_VK_PCORES");
    CHECK(score(a, "frame") > 0 && score(a, "FRAME") > 0 && score(a, "fRaMe RaTe") > 0);   // case-insensitive
    CHECK(score(a, "rate frame") > 0);                                                         // any order
    CHECK(score(a, "frame shader") == 0);                                                      // every word must match
    CHECK(score(a, "60fps") > 0);                                                              // compact: "60 FPS"
    CHECK(score(a, "totk_fps") > 0 && score(a, "TOTK-FPS") > 0);                               // the cfg key, punctuation-tolerant
    CHECK(score(c, "pcores") > 0 && score(c, "p-cores") > 0 && score(c, "P cores") > 0);
    CHECK(score(b, "hitches") > 0);                                                            // the detail text
    CHECK(score(a, "cpu") > 0);                                                                // the group / detail
    CHECK(score(a, "zzz") == 0 && score(a, "") == 0);
    // name matches rank first: "frame rate" is in a's name, only in b's detail
    CHECK(score(a, "frame rate") > score(b, "frame rate"));
    CHECK(score(a, "caps") < score(a, "frame") && score(a, "caps") > score(a, "less"));     // name > summary > detail
    CHECK(score(a, "rame") > 0 && score(a, "rame") < score(a, "frame"));
    // a whole word of the cfg key counts as the name, and the whole key puts that setting first
    Setting d = mk("Show FPS counter", "Draws the frame rate in a corner", "", "TOTK_OVERLAY_FPS");
    CHECK(score(a, "fps") >= score(a, "frame") - 1 && score(a, "fps") > score(d, "fps"));
    CHECK(score(a, "totk_fps") > score(d, "totk fps") && score(d, "overlay fps") > score(a, "overlay fps"));
    CHECK(score(c, "vk") > score(b, "vk"));

    // highlight spans: byte ranges of the name, merged and sorted
    auto sp = search_spans("Frame rate limit", search_words("rate fr"));
    CHECK(sp.size() == 2 && sp[0].first == 0 && sp[0].second == 2 && sp[1].first == 6 && sp[1].second == 10);
    sp = search_spans("Frame rate limit", search_words("ram fram"));
    CHECK(sp.size() == 1 && sp[0].first == 0 && sp[0].second == 4);
    sp = search_spans("P-cores only", search_words("pcores"));   // a compact-only match has no span
    CHECK(sp.empty());

    // the real schema: every setting is findable by its own name and by its cfg key, and fast enough
    if (argc > 1) {
        FILE *fp = fopen(argv[1], "rb");
        if (!fp) { printf("cannot read %s\n", argv[1]); return 2; }
        static char buf[1 << 20];
        size_t n = fread(buf, 1, sizeof buf, fp);
        fclose(fp);
        mini::string err;
        auto schema = parse_schema(mini::string(buf, n), &err);
        CHECK(err.empty() && schema.size() > 50);
        mini::vector<SearchText> st;
        for (auto &s : schema) st.push_back(search_text(s));
        int bad = 0;
        for (size_t i = 0; i < schema.size(); i++) {
            if (search_score(st[i], search_words(schema[i].name)) < 12) { if (bad++ < 5) printf("name not found: %s\n", schema[i].name.c_str()); }
            if (search_score(st[i], search_words(schema[i].key)) == 0) { if (bad++ < 5) printf("key not found: %s\n", schema[i].key.c_str()); }
        }
        CHECK(bad == 0);
        Uint64 t0 = SDL_GetTicksNS();
        const char *qs[] = {"c", "cp", "cpu", "cpu cores", "fps", "shader", "frame rate", "x", "totk_", "zzzz"};
        int hits = 0;
        for (int rep = 0; rep < 100; rep++)
            for (const char *q : qs) {
                auto ww = search_words(q);
                for (auto &t : st) hits += search_score(t, ww) > 0;
            }
        double ms = (SDL_GetTicksNS() - t0) / 1e6 / 1000.0;   // per query over the whole schema
        printf("schema: %d settings, %.3f ms per query (%d hits)\n", (int)schema.size(), ms, hits);
        CHECK(ms < 2.0);
    }
    if (fails) { printf("%d of %d checks FAILED\n", fails, checks); return 1; }
    printf("all %d search checks passed\n", checks);
    return 0;
}

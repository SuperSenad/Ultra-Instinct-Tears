// [UIT 0387] The DLSS scene size a TOTK_DLSS_MODE value asks for, as a share of the output (canvas) size, shared by the
// memory plan (uit_display.c uit_scene_plan) and its test. dlaa: the full output size (1.0, DLSS anti-aliases at 1:1);
// own: the game's own size (0: 1600x900 at 1080p, 3200x1800 at 2160p, DLSS enlarges it 1.2x); the rest NVIDIA's
// ratios. An unknown or empty value: quality.
#pragma once
#include "uit_str.h"

static inline double uit_dlss_scene_share(const char *mode) {
    static const struct { const char *n; double s; } M[] = {{"dlaa", 1.0}, {"own", 0}, {"quality", 1 / 1.5}, {"balanced", 1 / 1.724},
                                                             {"performance", 0.5}, {"ultraperf", 1 / 3.0}, {"ultraperformance", 1 / 3.0},
                                                             {"ultra_performance", 1 / 3.0}};
    if (mode && *mode)
        for (unsigned i = 0; i < sizeof M / sizeof M[0]; i++) if (!uit_stricmp(mode, M[i].n)) return M[i].s;
    return 1 / 1.5;
}

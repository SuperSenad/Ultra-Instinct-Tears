// [UIT 0074] Fixed-point scaling of the host counter (plat_now_ns, rt_ticks).
// Based on valentderah/totk-pc-recomp runtime/platform/platform.h (commit 8e15d70).
// MIT License, Copyright (c) 2026 Valiantsin Dzerakh (see THIRD_PARTY_NOTICES.md).
#pragma once
#include <stdint.h>

// t * num / den for a fixed num / den without a 128-bit division per call: t * q + (t * f >> 64) with
// q = num / den and f = (num % den) * 2^64 / den, both set once at startup. Monotonic in t; at most one below the exact
// quotient while t * (num % den) / den fits 64 bits (tools/tests/test_timer_scale.c checks it).
typedef struct { uint64_t q, f; } PlatScale;
static inline PlatScale plat_scale(uint64_t num, uint64_t den) {
    return (PlatScale){num / den, (uint64_t)(((unsigned __int128)(num % den) << 64) / den)};
}
static inline uint64_t plat_scaled(PlatScale s, uint64_t t) {
    return t * s.q + (uint64_t)(((unsigned __int128)t * s.f) >> 64);
}

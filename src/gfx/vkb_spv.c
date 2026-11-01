// SPIR-V rewrites of translated shaders for the bind state (milestone 3.3 leftovers, the first part of the variant
// service). Vertex attributes of integer formats: Maxwell hands a shader the attribute's raw bits, and TOTK's skinned
// vertex shaders read bone indices that way (floatBitsToInt(attr) * 48). The default translation declares every input
// vec4 float, so the attribute had to be given as a scaled format (5 -> 5.0f) and the bone index was garbage. Here the
// input becomes uvec4 / ivec4 (the attribute is then given in its own integer format) and every load from it is
// bitcast back to float: the rest of the shader is unchanged and sees the raw bits, as on the hardware.
#include <stdlib.h>
#include <string.h>

#include "vkb_spv.h"

enum { OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_LOAD = 61,
       OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_DECORATE = 71, OP_BITCAST = 124 };
enum { DEC_LOCATION = 30, SC_INPUT = 1 };

typedef struct { uint32_t *w; size_t n, cap; } words;
static void put(words *o, uint32_t v) {
    if (o->n == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 1024;
        o->w = realloc(o->w, o->cap * sizeof *o->w);
        if (!o->w) abort();
    }
    o->w[o->n++] = v;
}
static void put_ins(words *o, uint32_t op, int n, const uint32_t *ops) {
    put(o, (uint32_t)(n + 1) << 16 | op);
    for (int i = 0; i < n; i++) put(o, ops[i]);
}

int vkb_spv_int_inputs(const uint32_t *in, size_t nw, uint32_t locmask, uint32_t signedmask, uint32_t **out, size_t *outw) {
    if (nw < 5 || !locmask) return 0;
    uint32_t bound = in[3];
    // ids by kind (0: absent)
    uint32_t f32 = 0, vec4f = 0, i32[2] = {0, 0}, ivec4[2] = {0, 0};   // [0] unsigned, [1] signed
    uint32_t *loc_of = calloc(bound, sizeof *loc_of);                  // id -> location + 1 (decorated)
    uint32_t *ptr_pointee = calloc(bound, sizeof *ptr_pointee);        // Input pointer type id -> pointee
    uint8_t *var = calloc(bound, 1);                                   // id -> 1 + signedness of a patched input
    uint8_t *chain = calloc(bound, 1);                                 // access chain result -> 1 + signedness
    if (!loc_of || !ptr_pointee || !var || !chain) abort();
    int ok = 1, patched = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        if (!wc || i + wc > nw) { ok = 0; break; }
        const uint32_t *a = in + i + 1;
        if (op == OP_DECORATE && wc >= 4 && a[1] == DEC_LOCATION && a[0] < bound) loc_of[a[0]] = a[2] + 1;
        else if (op == OP_TYPE_FLOAT && a[1] == 32) f32 = a[0];
        else if (op == OP_TYPE_INT && a[1] == 32) i32[a[2] ? 1 : 0] = a[0];
        else if (op == OP_TYPE_VECTOR && a[2] == 4) {
            if (a[1] == f32) vec4f = a[0];
            else if (a[1] == i32[0] && i32[0]) ivec4[0] = a[0];
            else if (a[1] == i32[1] && i32[1]) ivec4[1] = a[0];
        } else if (op == OP_TYPE_POINTER && a[1] == SC_INPUT && a[0] < bound) ptr_pointee[a[0]] = a[2];
        else if (op == OP_VARIABLE && a[2] == SC_INPUT && a[1] < bound && loc_of[a[1]]) {
            uint32_t loc = loc_of[a[1]] - 1;
            if (loc < 32 && (locmask >> loc & 1)) {
                if (a[0] >= bound || ptr_pointee[a[0]] != vec4f || !vec4f) ok = 0;   // only vec4 float inputs
                else { var[a[1]] = (uint8_t)(1 + (signedmask >> loc & 1)); patched++; }
            }
        }
        i += wc;
    }
    if (!ok || !patched) { free(loc_of); free(ptr_pointee); free(var); free(chain); return 0; }
    // new ids: missing integer scalar / vector types, Input pointers to them
    uint32_t next = bound, new_i32[2] = {0, 0}, new_ivec4[2] = {0, 0}, p_vec[2], p_scalar[2];
    for (int s = 0; s < 2; s++) {
        if (!i32[s]) i32[s] = new_i32[s] = next++;
        if (!ivec4[s]) ivec4[s] = new_ivec4[s] = next++;
        p_vec[s] = next++;
        p_scalar[s] = next++;
    }
    words o = {0};
    for (int k = 0; k < 5; k++) put(&o, in[k]);
    int types_done = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        const uint32_t *a = in + i + 1;
        if (op == OP_VARIABLE && a[1] < bound && var[a[1]]) {
            if (!types_done) {   // the new types, before the first patched input (all they refer to is declared above)
                for (int s = 0; s < 2; s++) {
                    if (new_i32[s]) put_ins(&o, OP_TYPE_INT, 3, (uint32_t[]){i32[s], 32, (uint32_t)s});
                    if (new_ivec4[s]) put_ins(&o, OP_TYPE_VECTOR, 3, (uint32_t[]){ivec4[s], i32[s], 4});
                    put_ins(&o, OP_TYPE_POINTER, 3, (uint32_t[]){p_vec[s], SC_INPUT, ivec4[s]});
                    put_ins(&o, OP_TYPE_POINTER, 3, (uint32_t[]){p_scalar[s], SC_INPUT, i32[s]});
                }
                types_done = 1;
            }
            int s = var[a[1]] - 1;
            put(&o, in[i]);
            put(&o, p_vec[s]);
            for (uint32_t k = 2; k < wc; k++) put(&o, in[i + k]);
        } else if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && a[2] < bound && var[a[2]]) {
            int s = var[a[2]] - 1;
            if (a[1] < bound) chain[a[1]] = (uint8_t)(1 + s);
            if (wc != 5) ok = 0;   // a component of the vector: one index
            put(&o, in[i]);
            put(&o, p_scalar[s]);
            for (uint32_t k = 2; k < wc; k++) put(&o, in[i + k]);
        } else if (op == OP_LOAD && a[2] < bound && (var[a[2]] || chain[a[2]])) {
            int whole = var[a[2]] != 0, s = (whole ? var[a[2]] : chain[a[2]]) - 1;
            uint32_t t = whole ? ivec4[s] : i32[s], raw = next++;
            put(&o, in[i]);
            put(&o, t);
            put(&o, raw);
            for (uint32_t k = 3; k < wc; k++) put(&o, in[i + k]);
            put_ins(&o, OP_BITCAST, 3, (uint32_t[]){a[0], a[1], raw});   // the original result id, as float again
        } else {
            for (uint32_t k = 0; k < wc; k++) put(&o, in[i + k]);
        }
        i += wc;
    }
    free(loc_of); free(ptr_pointee); free(var); free(chain);
    if (!ok) { free(o.w); return 0; }
    o.w[3] = next;
    *out = o.w;
    *outw = o.n;
    return patched;
}

// Storage buffers the shader reads: bindings first..first+n-1, bit i when a load or an atomic (they read too) goes through a
// pointer into binding first+i. The translator's reflection only tells writes; a draw that only writes a buffer need not
// wait for earlier draws' stores to it (vkb_draw.c). Pointers are followed through access chains and copies, which in
// the translator's SPIR-V come before their uses.
uint32_t vkb_spv_sbuf_reads(const uint32_t *in, size_t nw, uint32_t first, int n) {
    if (nw < 5 || n <= 0) return 0;
    uint32_t bound = in[3], mask = 0;
    int8_t *of = malloc(bound);   // id -> storage buffer index, -1: none
    uint32_t *bind = calloc(bound, sizeof *bind);
    if (!of || !bind) abort();
    memset(of, -1, bound);
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        if (!wc || i + wc > nw) break;
        const uint32_t *a = in + i + 1;
        if (op == OP_DECORATE && wc >= 4 && a[1] == 33 && a[0] < bound) bind[a[0]] = a[2] + 1;   // Binding
        else if (op == OP_VARIABLE && wc >= 4 && a[1] < bound && bind[a[1]] > first && bind[a[1]] - 1 - first < (uint32_t)n)
            of[a[1]] = (int8_t)(bind[a[1]] - 1 - first);
        else if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN || op == 67 || op == 83) && wc >= 4 && a[1] < bound && a[2] < bound)
            of[a[1]] = of[a[2]];   // OpPtrAccessChain 67, OpCopyObject 83
        else if ((op == OP_LOAD || (op >= 227 && op <= 242 && op != 228)) && wc >= 4 && a[2] < bound && of[a[2]] >= 0)
            mask |= 1u << of[a[2]];   // OpLoad, OpAtomic* except OpAtomicStore
        i += wc;
    }
    free(of);
    free(bind);
    return mask;
}
static uint32_t location_mask(const uint32_t *in, size_t nw, uint32_t storage);
uint32_t vkb_spv_input_mask(const uint32_t *in, size_t nw) { return location_mask(in, nw, SC_INPUT); }
uint32_t vkb_spv_output_mask(const uint32_t *in, size_t nw) { return location_mask(in, nw, 3); }   // StorageClass Output
static uint32_t location_mask(const uint32_t *in, size_t nw, uint32_t storage) {
    if (nw < 5) return 0;
    uint32_t bound = in[3], mask = 0;
    uint32_t *loc_of = calloc(bound, sizeof *loc_of);
    if (!loc_of) abort();
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        if (!wc || i + wc > nw) break;
        const uint32_t *a = in + i + 1;
        if (op == OP_DECORATE && wc >= 4 && a[1] == DEC_LOCATION && a[0] < bound) loc_of[a[0]] = a[2] + 1;
        else if (op == OP_VARIABLE && a[2] == storage && a[1] < bound && loc_of[a[1]] && loc_of[a[1]] <= 32)
            mask |= 1u << (loc_of[a[1]] - 1);
        i += wc;
    }
    free(loc_of);
    return mask;
}

// [UIT 0341] Varyings a vertex shader does not write. On the console a fragment input whose vertex output is never
// written reads (0, 0, 0, 1) (the attribute's reset value); in Vulkan it is undefined (in practice 0 or stale data).
// The translation is made one shader at a time, so a vertex shader leaves out what it never writes. TOTK's map draws its
// revealed areas (the Depths lit by lightroots) with the two fragment shaders that read such an input (b4022e9a and
// b44c5f87, both sampling the reveal mask): without the console's value the lit areas were not drawn on the map. vkb_spv_default_outputs adds the missing
// outputs to the vertex shader, written with that value before anything else.
enum { OP_ENTRY_POINT = 15, OP_CONSTANT = 43, OP_CONSTANT_COMPOSITE = 44, OP_FUNCTION = 54, OP_STORE = 62, OP_LABEL = 248,
       OP_LINE = 8, OP_NO_LINE = 317, SC_OUTPUT = 3 };

// Locations of Input variables of type vec4 of 32-bit float (the only inputs the vertex side is given defaults for).
uint32_t vkb_spv_input_f4_mask(const uint32_t *in, size_t nw) {
    if (nw < 5) return 0;
    uint32_t bound = in[3], mask = 0;
    uint32_t *loc_of = calloc(bound, sizeof *loc_of), *pointee = calloc(bound, sizeof *pointee);
    uint8_t *kind = calloc(bound, 1);   // 1: 32-bit float, 2: vec4 of it
    if (!loc_of || !pointee || !kind) abort();
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        if (!wc || i + wc > nw) break;
        const uint32_t *a = in + i + 1;
        if (op == OP_DECORATE && wc >= 4 && a[1] == DEC_LOCATION && a[0] < bound) loc_of[a[0]] = a[2] + 1;
        else if (op == OP_TYPE_FLOAT && wc >= 3 && a[0] < bound && a[1] == 32) kind[a[0]] = 1;
        else if (op == OP_TYPE_VECTOR && wc >= 4 && a[0] < bound && a[1] < bound && kind[a[1]] == 1 && a[2] == 4) kind[a[0]] = 2;
        else if (op == OP_TYPE_POINTER && wc >= 4 && a[0] < bound && a[1] == SC_INPUT) pointee[a[0]] = a[2];
        else if (op == OP_VARIABLE && wc >= 4 && a[2] == SC_INPUT && a[0] < bound && a[1] < bound && loc_of[a[1]] &&
                 loc_of[a[1]] <= 32 && pointee[a[0]] < bound && kind[pointee[a[0]]] == 2)
            mask |= 1u << (loc_of[a[1]] - 1);
        i += wc;
    }
    free(loc_of); free(pointee); free(kind);
    return mask;
}

int vkb_spv_default_outputs(const uint32_t *in, size_t nw, uint32_t locmask, uint32_t **out, size_t *outw) {
    if (nw < 5 || !locmask) return 0;
    uint32_t bound = in[3], f32 = 0, vec4f = 0, entry = 0, nloc = 0;
    size_t first_type = 0, first_fn = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        if (!wc || i + wc > nw) return 0;
        const uint32_t *a = in + i + 1;
        if (op == OP_ENTRY_POINT && wc >= 3 && a[0] == 0) { if (entry) return 0; entry = a[1]; }   // one Vertex entry point
        else if (op == OP_TYPE_FLOAT && wc >= 3 && a[1] == 32 && !f32) f32 = a[0];
        else if (op == OP_TYPE_VECTOR && wc >= 4 && f32 && a[1] == f32 && a[2] == 4 && !vec4f) vec4f = a[0];
        if (!first_type && ((op >= 19 && op <= 39) || (op >= 41 && op <= 52) || op == OP_VARIABLE)) first_type = i;
        if (!first_fn && op == OP_FUNCTION) first_fn = i;
        i += wc;
    }
    if (!entry || !f32 || !vec4f || !first_type || !first_fn) return 0;
    for (uint32_t m = locmask; m; m &= m - 1) nloc++;
    uint32_t next = bound, ptr = next++, c0 = next++, c1 = next++, cv = next++, var[32];
    for (uint32_t m = locmask, k = 0; m; m &= m - 1) var[k++] = next++;
    words o = {0};
    for (int k = 0; k < 5; k++) put(&o, in[k]);
    int in_entry = 0, stores_done = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        const uint32_t *a = in + i + 1;
        if (i == first_type) {   // annotations end here: the new outputs' locations
            uint32_t k = 0;
            for (uint32_t m = locmask; m; m &= m - 1) put_ins(&o, OP_DECORATE, 3, (uint32_t[]){var[k++], DEC_LOCATION, (uint32_t)__builtin_ctz(m)});
        }
        if (i == first_fn) {   // after every type: the pointer type, the constant (0, 0, 0, 1), the variables
            put_ins(&o, OP_TYPE_POINTER, 3, (uint32_t[]){ptr, SC_OUTPUT, vec4f});
            put_ins(&o, OP_CONSTANT, 3, (uint32_t[]){f32, c0, 0});
            put_ins(&o, OP_CONSTANT, 3, (uint32_t[]){f32, c1, 0x3f800000u});
            put_ins(&o, OP_CONSTANT_COMPOSITE, 6, (uint32_t[]){vec4f, cv, c0, c0, c0, c1});
            for (uint32_t k = 0; k < nloc; k++) put_ins(&o, OP_VARIABLE, 3, (uint32_t[]){ptr, var[k], SC_OUTPUT});
        }
        if (op == OP_ENTRY_POINT && a[0] == 0 && a[1] == entry) {   // the interface lists the new outputs
            put(&o, (wc + nloc) << 16 | op);
            for (uint32_t k = 1; k < wc; k++) put(&o, in[i + k]);
            for (uint32_t k = 0; k < nloc; k++) put(&o, var[k]);
            i += wc;
            continue;
        }
        if (op == OP_FUNCTION && wc >= 5) in_entry = a[1] == entry;
        if (in_entry && !stores_done && op != OP_FUNCTION && op != 55 /* OpFunctionParameter */ && op != OP_LABEL &&
            op != OP_VARIABLE && op != OP_LINE && op != OP_NO_LINE) {   // the entry's first instruction after its variables
            for (uint32_t k = 0; k < nloc; k++) put_ins(&o, OP_STORE, 2, (uint32_t[]){var[k], cv});
            stores_done = 1;
        }
        for (uint32_t k = 0; k < wc; k++) put(&o, in[i + k]);
        i += wc;
    }
    if (!stores_done) { free(o.w); return 0; }
    o.w[3] = next;
    *out = o.w;
    *outw = o.n;
    return (int)nloc;
}

// [UIT 0351] Depth compares clamp their reference to [0, 1] (vkb_spv.h).
enum { OP_EXT_INST_IMPORT = 11, OP_EXT_INST = 12, OP_MEMORY_MODEL = 14, OP_SAMPLE_DREF_IMPLICIT = 89,
       OP_SAMPLE_DREF_EXPLICIT = 90, OP_DREF_GATHER = 97, OP_SPARSE_SAMPLE_DREF_IMPLICIT = 307,
       OP_SPARSE_SAMPLE_DREF_EXPLICIT = 308, OP_SPARSE_DREF_GATHER = 315, GLSL_FCLAMP = 43 };
static int is_dref_op(uint32_t op) {
    return op == OP_SAMPLE_DREF_IMPLICIT || op == OP_SAMPLE_DREF_EXPLICIT || op == OP_DREF_GATHER ||
           op == OP_SPARSE_SAMPLE_DREF_IMPLICIT || op == OP_SPARSE_SAMPLE_DREF_EXPLICIT || op == OP_SPARSE_DREF_GATHER;
}
static int is_glsl450(const uint32_t *a, uint32_t wc) {   // OpExtInstImport's name operand (a[1..])
    static const char name[] = "GLSL.std.450";
    return wc >= 2 + (sizeof name + 3) / 4 && !memcmp(a + 1, name, sizeof name);
}
int vkb_spv_dref_clamp(const uint32_t *in, size_t nw, uint32_t **out, size_t *outw) {
    if (nw < 5) return 0;
    uint32_t bound = in[3], f32 = 0, glsl = 0;
    int compares = 0;
    for (size_t i = 5; i < nw;) {   // what is there: the compares, float, the GLSL.std.450 import
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        if (!wc || i + wc > nw) return 0;
        const uint32_t *a = in + i + 1;
        if (op == OP_TYPE_FLOAT && wc == 3 && a[1] == 32) f32 = a[0];
        else if (op == OP_EXT_INST_IMPORT && is_glsl450(a, wc)) glsl = a[0];
        else if (is_dref_op(op) && wc >= 6) compares++;
        i += wc;
    }
    if (!compares || !f32) return 0;   // (a reference is always a 32-bit float: no float type, no compare)
    uint32_t next = bound, new_glsl = glsl ? 0 : next++, c0 = next++, c1 = next++;
    if (!glsl) glsl = new_glsl;
    words o = {0};
    for (int k = 0; k < 5; k++) put(&o, in[k]);
    int done = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        const uint32_t *a = in + i + 1;
        if (op == OP_MEMORY_MODEL && new_glsl) {   // the import goes with the others, before the memory model
            static const char name[] = "GLSL.std.450";   // 13 bytes with the terminator: 4 words, zero padded
            uint32_t s[4] = {0};
            memcpy(s, name, sizeof name);
            put_ins(&o, OP_EXT_INST_IMPORT, 5, (uint32_t[]){new_glsl, s[0], s[1], s[2], s[3]});
        }
        if (is_dref_op(op) && wc >= 6) {   // result type, result, sampled image, coordinate, Dref, ...
            uint32_t clamped = next++;
            put_ins(&o, OP_EXT_INST, 7, (uint32_t[]){f32, clamped, glsl, GLSL_FCLAMP, a[4], c0, c1});
            put(&o, in[i]);
            for (uint32_t k = 1; k < wc; k++) put(&o, k == 5 ? clamped : in[i + k]);
            done++;
        } else {
            for (uint32_t k = 0; k < wc; k++) put(&o, in[i + k]);
            if (op == OP_TYPE_FLOAT && a[0] == f32) {   // the constants right after their type
                put_ins(&o, OP_CONSTANT, 3, (uint32_t[]){f32, c0, 0x00000000u});
                put_ins(&o, OP_CONSTANT, 3, (uint32_t[]){f32, c1, 0x3f800000u});
            }
        }
        i += wc;
    }
    o.w[3] = next;
    *out = o.w;
    *outw = o.n;
    return done;
}

// [UIT 0143] vkb_image.cpu_written (WriteTexels changed the texture's game memory: upload it before the next use) is
// set by a game thread (mtl_write_texels, vkb_tex.c) and taken by the recording thread that uploads (tex_upload). It was
// a plain int: the recorder cleared it with a store and then read the game memory, and a store followed by loads may be
// reordered (x86 too), so a write made between the recorder's read and the clear landing could be lost for good (the
// texture kept the old texels until something else made it stale). Now the game thread publishes with a release store
// after its writes, and the recorder takes the flag with one atomic exchange before it reads: either it sees the 1 (and
// then the writes before it), or the 1 lands after the exchange and the next use uploads again.
#pragma once
static inline void vkb_cpuw_mark(int *f) { __atomic_store_n(f, 1, __ATOMIC_RELEASE); }
static inline int vkb_cpuw_peek(const int *f) { return __atomic_load_n(f, __ATOMIC_ACQUIRE); }
static inline int vkb_cpuw_take(int *f) { return __atomic_exchange_n(f, 0, __ATOMIC_ACQ_REL); }

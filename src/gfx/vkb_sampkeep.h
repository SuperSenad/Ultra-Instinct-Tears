// [UIT 0207] What sampler() does with a sampler it just made, once it takes the registry lock again (box test:
// tools/tests/test_sampler_keep.c). Another thread may have made one for the same registration meanwhile, or the game
// may have registered something else in the slot (mtl_reg_samp). Before 0207 the last case kept nothing and never
// destroyed the new sampler: one leaked VkSampler per such race (Vulkan allows ~4000 on some drivers).
#pragma once
enum { VKB_SAMP_KEEP,        // stored in the registry; the registry owns it
       VKB_SAMP_USE_OTHER,   // destroy ours now (never used), use the one already stored
       VKB_SAMP_ONE_USE,     // re-registered meanwhile: use ours for this draw, destroy it after the submission being recorded
       VKB_SAMP_LEAK };      // as before 0207 (TOTK_VK_SAMPLER_FREE=0)
static inline int vkb_samp_keep(int still_valid, int same_params, int slot_has_sampler, int free_unkept) {
    if (still_valid && same_params) return slot_has_sampler ? VKB_SAMP_USE_OTHER : VKB_SAMP_KEEP;
    return free_unkept ? VKB_SAMP_ONE_USE : VKB_SAMP_LEAK;
}

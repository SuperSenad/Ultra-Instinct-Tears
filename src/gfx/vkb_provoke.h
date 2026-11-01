// [UIT 0205] The provoking vertex (where flat-shaded outputs come from). NVN follows GL: the last vertex of each
// primitive; Vulkan's default and Metal's (the layer this port came from) is the first. TOTK never changes the mode
// (nvnCommandBufferSetProvokingVertexMode is not called with another value in any session log). Whether any of its
// shaders give a triangle's vertices different flat values is not known, so the GL convention is a choice, default
// Off (TOTK_VK_PROVOKING_LAST=1 to try). Needs provokingVertexLast (VK_EXT_provoking_vertex). Box test:
// tools/tests/test_provoking.c.
#pragma once
enum { VKB_PV_FIRST = 0, VKB_PV_LAST = 1 };   // VkProvokingVertexModeEXT's values
static inline int vkb_provoking_mode(int want_last, int has_last) { return want_last && has_last ? VKB_PV_LAST : VKB_PV_FIRST; }

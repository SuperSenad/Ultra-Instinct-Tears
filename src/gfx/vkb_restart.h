// [UIT 0158] primitive restart for the Vulkan replay (vkb_draw.c), kept apart so tools/tests/test_prim_restart.c can check it.
#pragma once

enum { RS_OFF = 0, RS_GAME = 1, RS_STRIPS = 2 };   // TOTK_VK_PRIM_RESTART: 0, on (default), strips

// 1 = a draw restarts at the all-ones index. strip: the topology is a strip or fan (lists never restart: Vulkan needs
// primitiveTopologyListRestart for that). game_set: the game has called SetPrimitiveRestart; game_on: what it set.
static inline int vkb_restart_for(int mode, int strip, int game_set, int game_on) {
    if (!strip || mode == RS_OFF) return 0;
    if (mode == RS_STRIPS) return 1;
    return game_set ? game_on != 0 : 1;   // before the game sets it: on, as Metal (always restarts strips)
}

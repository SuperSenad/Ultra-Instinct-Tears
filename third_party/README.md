# Third-party code in this repository

Every piece keeps its license; the license file sits next to the code. All are compatible with the port's
GPL-3.0-or-later.

| Folder | What | From | License |
|---|---|---|---|
| `maxwell_shader/` | The Ryujinx 1.3.2 shader translator, unchanged apart from the patches below (Ryujinx Team and contributors) (`Ryujinx.Graphics.Shader`, `Ryujinx.Common`, `Spv.Generator`) | MacNativeGames `vendor/ryujinx` @b75be8c (Ryujinx 1.3.2, unmodified there), with the five patches of `shader_translator/patches/` applied here | MIT (`maxwell_shader/LICENSE.txt`) |
| `shader_translator/` | MacNativeGames' `totk_xlate`: the service around that translator that turns the game's NVN shaders into SPIR-V; built as `ultrs-shaders.exe` by `tools/build_xlate.py` | MacNativeGames `games/totk/tools/totk_xlate` @b75be8c, without its macOS build script | MIT (`shader_translator/LICENSE`) |
| `imgui/` | Dear ImGui 1.92.9 (Omar Cornut and contributors): `imgui*.cpp/h`, `imstb_*.h` and the SDL3 and SDL_Renderer backends, unchanged; the launcher's UI (`tools/build_launcher.py`) | github.com/ocornut/imgui, tag v1.92.9 | MIT (`imgui/LICENSE.txt`) |
| `inter/` | The Inter typeface, Regular and SemiBold (Rasmus Andersson); compiled into the launcher | github.com/rsms/inter, release 4.1 (font version 4.001) | SIL Open Font License 1.1 (`inter/LICENSE.txt`) |

Elsewhere in the tree: `src/gfx/tex/bc7enc/` (bc7enc and rgbcx, Rich Geldreich, MIT) and the files ported from
MacNativeGames (MIT) and LASRecomp (MIT), each named in its header comment;
`tools/shaders/inventory.py` (MacNativeGames, MIT).
Files under `src/` and `tools/` whose header or a section comment says "Based on" or "Adapted from valentderah/totk-pc-recomp"
(UIT 0074 and later: the timer scale, video memory blocks, switch points / asynchronous preemption in
`src/host/threads.c`, `src/host/preempt_win.c` and `src/runtime/dispatch.c`, and the spin wait gate and table in
`src/host/threads.c` and `tools/lifter/spin_waits*.{py,txt}`, and the PC zstd decoder `src/host/zstd_native.c` with its
call sites `tools/lifter/zstd_sites.py`) take code
from Valiantsin Dzerakh's MIT-licensed TotK recompilation (reference commit c3b9a4e); each keeps the copyright line
"Copyright (c) 2026 Valiantsin Dzerakh", and the license text is `licenses/valentderah-totk-recomp.txt`. Only its MIT
code is used: none of its tests that run on Unicorn (GPL), and nothing from yuzu. The spin wait search and gate
come in turn from seedds' nxnative (MIT, `licenses/nxnative.txt`) and MacNativeGames (MIT, `licenses/MacNativeGames.txt`).
`src/vk/sl_abi.h` restates NVIDIA Streamline 2.14.16's public API declarations in C (NVIDIA, MIT,
`src/vk/LICENSE-Streamline`); Streamline itself and DLSS are not in the tree: the player downloads them
(`tools/setup.py --get-dlss`, or by hand).

## Checked: no B2recomp code, no Eden / yuzu code (UIT 0090)

The sentence above used to list B2recomp (K0NL, GPL-3.0) as a source. No file of the tree says it comes from B2recomp,
and a line-by-line comparison (runs of 4 lines, spaces ignored) with every one of B2recomp's 36 commits (330e59a to
bec514e, 17 to 23 October 2026) and with ssbu-recomp (Denkishi, GPL-3.0; where B2recomp's lifter and runtime come from)
finds nothing shared beyond lists of standard `#include`s and Python's usual file-hash loop. The names the two have in
common are Windows, Vulkan, NVN and Nintendo SDK names, plus `prewarm_start` and `crash_filter`, whose code here is
different and was written for this port. So the mention was removed.
`tools/eden_xlate/`, `tools/run_eden_xlate.py` and `tools/compare_spirv_opt.py` were removed: a developer tool that built
Eden's (a yuzu fork, GPL) shader recompiler to compare its SPIR-V with the Ryujinx translator's, modeled on mk8-recomp
(GPL). The build and the package never used it. The port's own license (`LICENSE`, GPL-3.0-or-later, from UltraTears)
is unchanged.

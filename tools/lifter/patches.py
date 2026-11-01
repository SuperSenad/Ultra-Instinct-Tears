"""Per-instruction replacements applied by gen.py: guest address -> function(gen, addr) returning C code.

Every patch checks the original instruction word first, so lifting another game version (or a different build)
fails loudly instead of changing the wrong code. The optional patch render_res warns and keeps the original
code instead, and tells the runtime through table_flags(); so do output_res and aspect. Runtime switches live in src/gfx/nvn/hle_nvn.c and, for the frame
rate, src/host/fps_unlock.c. gen.py stops the lift if a patched address of main was never emitted (APPLIED).
"""
import sys

from lifter import emit_asm_batch

MAIN = 0x800000000   # main's load address (tools/gen_game.py); the offsets below are main-relative
APPLIED = set()      # addresses whose replacement was emitted (gen.py checks that none is missing)


def expect(gen, addr, word, what):
    got = gen.word(addr)
    if got != word:
        raise SystemExit(f'patch {what} at {addr:#x}: expected {word:#010x}, found {got:#010x} (not TOTK 1.4.3?)')
    APPLIED.add(addr)


def drs_off(gen, addr):
    """Dynamic resolution off (1.4.3). main+0x584300 is the scene resolution controller: each frame it fits the
    recent frame times and returns {scale (float), state} to the renderer (main+0x581968), which turns the scale into
    the scene size (+0xccc). Its first test is the controller's enable bit (+0x110 bit 0); with the bit clear it
    takes its own disabled path (main+0x584c3c): scale 1.0, history untouched. The patch takes that path unless
    TOTK_DRS=1. Why: the game flips the scene targets between 1600x896 and 1432x808 over the same memory and our
    per-size images then hand stale content to the other size (terrain gone, clouds over the scene);
    image quality is better at a fixed size and upscaling will come from FSR."""
    expect(gen, addr, 0x360042e8, 'drs_off')   # tbz w8, #0, 0x800584c3c
    return ('{ extern int totk_drs_enabled(void); '
            'if (!totk_drs_enabled() || ((c->x[8] >> 0) & 1) == 0) { goto L_800584c3c; } }')


# ---------------------------------------------------------------- frame rate (TOTK_FPS, src/host/fps_unlock.c)
# Ported from MacNativeGames games/totk/runtime/game_patches_v143.c (MIT; checked there against 1.4.3
# and, for the controller, by emulation: scripts/verify_v143_fps60.py). MacNativeGames rewrites the loaded image (a
# branch to a veneer in the text padding, a data page after the module); here the same instruction words are lifted
# in place, so a veneer becomes inline C at the site it was branched from and the data page is totk_frame_step[] in
# the host (guest addresses are host addresses). Every replacement is chosen at run time: TOTK_FPS=30 (the default)
# runs the original instructions, so stock behaviour is unchanged.
# totk_frame_step[]: {seconds * 30 (the 30 Hz ratio), seconds, seconds * 60}, the measured frame time, written by
# runtime_frame_step_update at every present.

def _code(gen, addr, word):
    """C for `word` lifted as if it were at `addr` (straight-line instructions only)."""
    ins = gen.lifter.lift(word, addr)
    if ins.kind == 'op':
        return ins.code
    if ins.kind == 'asm':
        return emit_asm_batch([ins.asm])
    raise SystemExit(f'patch at {addr:#x}: replacement {word:#010x} is a {ins.kind}, not a plain instruction')


def _orig(gen, addr):
    ins = gen.lift(addr)
    if ins.kind == 'op':
        return ins.code
    if ins.kind == 'asm':
        return emit_asm_batch([ins.asm])
    raise SystemExit(f'patch at {addr:#x}: the original instruction is a {ins.kind}')


_STEP = '{ extern float totk_frame_step[]; c->x[%d] = (uint64_t)(uintptr_t)totk_frame_step; }'


def _switched(cond, new, old):
    return f'{{ extern int {cond}(void); if ({cond}()) {{ {new} }} else {{ {old} }} }}'


def _site(off, original, what, cond, build):
    """A patch at main+off: check the original word, then `build(gen, addr)` (C) when cond() is true at run time."""
    def patch(gen, addr):
        expect(gen, addr, original, what)
        return _switched(cond, build(gen, addr), _orig(gen, addr))
    return MAIN + off, patch


def _words(*words):
    return lambda gen, addr: ' '.join(_code(gen, addr, w) for w in words)


def _with_step(reg, *words):
    return lambda gen, addr: (_STEP % reg) + ' ' + ' '.join(_code(gen, addr, w) for w in words)


NOP = lambda gen, addr: '/* removed */'   # noqa: E731

# 60 FPS: the adaptive present-interval controller (main+0x2db174) may pick interval 1 (60 / s) as well as 2 and 3;
# shipping code clamps it to 2..3. Seven instructions, (offset, original, replacement).
FPS60 = [
    (0x2db204, 0x6b08015f, 0x7100055f),   # cmp w10, w8         -> cmp w10, #1
    (0x2db208, 0x1a89b10a, 0x1a9fa52a),   # csel w10, w8, w9, lt -> csinc w10, w9, wzr, ge
    (0x2db22c, 0xb940468a, 0xd503201f),   # ldr w10, [x20, #0x44] -> nop
    (0x2db360, 0x6b0801bf, 0x710005bf),   # cmp w13, w8         -> cmp w13, #1
    (0x2db36c, 0x1a88c1ac, 0x1a9fc5ac),   # csel w12, w13, w8, gt -> csinc w12, w13, wzr, gt
    (0x2db340, 0xb9400288, 0x52800028),   # ldr w8, [x20]       -> mov w8, #1
    (0x2db34c, 0x2a0a03f5, 0x1a9fa555),   # mov w21, w10        -> csinc w21, w10, wzr, ge
]

FRAME_RATE = [_site(off, orig, 'fps60', 'totk_fps_controller', _words(new)) for off, orig, new in FPS60]

# Real-time frame step: FrameRateMgr's nominal 60/FPS interval becomes the measured one, before the original 0.5/1.0
# selector (main+0x3e5658, x8 free: the next ldxr overwrites it), and the time object's 30 Hz ratio is the measured
# seconds * 30 (main+0x2daa54..78). The ratio/inverse/seconds/FPS arithmetic after it is the game's own.
FRAME_RATE += [
    _site(0x3e5658, 0x1e210808, 'realtime', 'totk_fps_realtime',
          _with_step(8, 0xbd400900, 0x1e210808)),        # ldr s0, [x8, #8]; fmul s8, s0, s1
    _site(0x2daa54, 0xbd425260, 'realtime', 'totk_fps_realtime', lambda gen, addr: _STEP % 9),   # x9 = the step
    _site(0x2daa58, 0x52a84e08, 'realtime', 'totk_fps_realtime', _words(0xbd400120)),           # ldr s0, [x9]
    _site(0x2daa6c, 0x5e21d800, 'realtime', 'totk_fps_realtime', NOP),
    _site(0x2daa70, 0x1e201820, 'realtime', 'totk_fps_realtime', NOP),
    _site(0x2daa74, 0x1e2c1001, 'realtime', 'totk_fps_realtime', NOP),
    _site(0x2daa78, 0x1e210800, 'realtime', 'totk_fps_realtime', NOP),
]

# Cloth / hair: the actor animation update's second clock builds {factor / 30, factor} per rendered frame (at 120 fps
# four seconds per second). Two inline sites take the measured ratio and seconds; the two calls of the out-of-line
# update (main+0x2a0e40) get s2/s3 scaled by the measured 30 Hz ratio first. Its third caller already passes measured
# time and is not touched. TOTK_CLOTH_REALTIME=0 keeps the original clock (comparison).
FRAME_RATE += [
    _site(0x246eac, 0x1e200940, 'cloth', 'totk_fps_cloth',   # ratio * factor -> sp+0x364; s0 = seconds * factor
          _with_step(8, 0xbd400100, 0x1e200940, 0xbd0367e0, 0xbd400500, 0x1e200940)),
    _site(0x345904, 0x1e210801, 'cloth', 'totk_fps_cloth',   # ratio * factor -> sp+0x114; s1 = seconds * factor
          _with_step(8, 0xbd400101, 0x1e210801, 0xbd0117e1, 0xbd400501, 0x1e210801)),
]


def _cloth_call(off, original):
    def patch(gen, addr):
        expect(gen, addr, original, 'cloth call')
        delta = original & 0x03FFFFFF
        target = addr + ((delta - 0x04000000 if delta & 0x02000000 else delta) << 2)
        if target != MAIN + 0x2a0e40:
            raise SystemExit(f'patch cloth call at {addr:#x}: calls {target:#x}, not main+0x2a0e40')
        scale = (_STEP % 8) + ' ' + ' '.join(_code(gen, addr, w) for w in (0xbd400104, 0x1e240842, 0x1e240863))
        return (f'{{ extern int totk_fps_cloth(void); if (totk_fps_cloth()) {{ {scale} }} }} '   # ldr s4, [x8]; s2, s3 *= s4
                f'{{ void f_{target:x}(Cpu *c); c->x[30] = {addr + 4:#x}ull; f_{target:x}(c); }}')
    return MAIN + off, patch


FRAME_RATE += [_cloth_call(0x24944c, 0x94015e7d), _cloth_call(0x28bc98, 0x9400546a)]

# Above 60 fps (MacNativeGames #61): the trampoline contact code (main+0x562a64) cuts the bounce by 0.1 when the time
# object's X+0x28 (30 / (ratio * time scale)) > 100, meant as "time slowed below 0.3" at 30 Hz; it compares
# X+0x28 * X+0x38 (= 30 / time scale) instead. main+0x562a68 (mov w9, #100.0) stays the original.
FRAME_RATE += [
    _site(0x562a64, 0xbd402920, 'bounce', 'totk_fps_highfps_fix',
          _words(0xbd402920, 0xbd403921, 0x1e210800)),   # ldr s0, [x9, #0x28]; ldr s1, [x9, #0x38]; fmul s0, s0, s1
]
_BOUNCE_GUARDS = [(0x562a68, 0x52a85909), (0x562a6c, 0x1e270121)]   # checked, not changed

# Above ~63 fps (MacNativeGames #466, #556, #538: translucent rocks, lanterns, walls): LOD and pre-actor fade counters
# advance by trunc(2.1 * ratio30), 0 above 1/0.476 s^-1; clamped to at least 1 at four sites.
_FADE = [0x1e380008, 0x7100011f, 0x1a9f1508]   # fcvtzs w8, s0; cmp w8, #0; csinc w8, w8, wzr, ne
_FADE10 = [0x1e38000a, 0x7100015f, 0x1a9f154a]   # the same with w10
FRAME_RATE += [
    _site(0x0a7ac4, 0x1e380008, 'fade', 'totk_fps_fade_fix', _words(*_FADE, 0x3942792a, 0x0b080148, 0x39027928)),
    _site(0x0a7ac8, 0x3942792a, 'fade', 'totk_fps_fade_fix', NOP),   # ldrb w10, [x9, #0x9e] (moved into the first)
    _site(0x0a7acc, 0x0b080148, 'fade', 'totk_fps_fade_fix', NOP),   # add w8, w10, w8
    _site(0x0a7ad0, 0x39027928, 'fade', 'totk_fps_fade_fix', NOP),   # strb w8, [x9, #0x9e]
    _site(0x95dd24, 0x1e380008, 'fade', 'totk_fps_fade_fix', _words(*_FADE, 0x0b080128)),   # ... add w8, w9, w8
    _site(0x95dd28, 0x0b080128, 'fade', 'totk_fps_fade_fix', NOP),
    _site(0x6155e4, 0x1e38000a, 'fade', 'totk_fps_fade_fix', _words(*_FADE10)),
    _site(0x615660, 0x1e38000a, 'fade', 'totk_fps_fade_fix', _words(*_FADE10)),
]


def _guard(off, original, what):
    def patch(gen, addr):
        expect(gen, addr, original, what)
        return _orig(gen, addr)
    return MAIN + off, patch


FRAME_RATE += [_guard(off, w, 'bounce guard') for off, w in _BOUNCE_GUARDS]


# ---------------------------------------------------------------- scene size (TOTK_RENDER_RES / DLSS, src/vk/dlss.c)
# The scene render width / height (floats, renderer +0xcc4 / +0xcc8) that the game then scales by canvas width / 1920.
# MacNativeGames game_patches_v143.c (patch_render_res / render_res_publish, MIT) lists the 1.4.3 words:
#   main+0x585e48  scvtf s0, w11  (0x1e220160)   main+0x585e50  str s0, [x19, #0xcc4]  (0xbd0cc660)
#   main+0x585e54  scvtf s0, w10  (0x1e220140)   main+0x585e60  str s0, [x19, #0xcc8]  (0xbd0cca60)
# and rewrites them into "adrp x3, page; ldp s0, s1, [x3, #16]; str s0; str s1". Here the two stores are lifted with
# s0 = totk_render_size[0] / [1] loaded just before them when totk_render_res_on() (x3 saved and restored), so
# what runs between the conversions and the stores does not matter; otherwise the original instructions run.
# (0009 had mistaken the replacement words "ldr s0, [x3]" for the originals at the scvtf sites: the lift stopped.)
# Optional: when any of the four words differs, the lift prints a warning and keeps the original code, and
# main_render_res_patch (main_table.c) is 0, so src/vk/dlss.c logs it and DLSS runs at the game's own size.
_RR_WORDS = [(0x585e48, 0x1e220160), (0x585e50, 0xbd0cc660), (0x585e54, 0x1e220140), (0x585e60, 0xbd0cca60)]
_rr_state = []   # [ok] once checked


def render_res_ok(gen):
    if not _rr_state:
        bad = [(off, w, gen.word(MAIN + off)) for off, w in _RR_WORDS
               if gen.lo <= MAIN + off < gen.hi and gen.word(MAIN + off) != w]
        _rr_state.append(not bad)
        for off, w, got in bad:
            print(f'WARNING: optional patch render_res at {MAIN + off:#x}: expected {w:#010x}, found {got:#010x}: '
                  'skipped (the game keeps its own render size; DLSS runs at it, like DLAA)', file=sys.stderr, flush=True)
    return _rr_state[0]


def _render_res_store(index):
    def patch(gen, addr):
        APPLIED.add(addr)
        if not render_res_ok(gen):
            return _orig(gen, addr)
        load = ('{ extern float totk_render_size[]; uint64_t x3_ = c->x[3]; '
                'c->x[3] = (uint64_t)(uintptr_t)totk_render_size; ' + _code(gen, addr, 0xbd400060 | index << 10) +
                ' c->x[3] = x3_; }')   # ldr s0, [x3, #4 * index]
        return _switched('totk_render_res_on', load + ' ' + _orig(gen, addr), _orig(gen, addr))
    return patch


RENDER_RES = [(MAIN + 0x585e50, _render_res_store(0)), (MAIN + 0x585e60, _render_res_store(1))]


# ---------------------------------------------------------------- output canvas (TOTK_OUTPUT_RES, src/host/uit_display.c)
# The game's display canvas (swapchain, UI and composition targets) is 1920x1080 docked. MacNativeGames
# game_patches_v143.c (patch_output_res, MIT) lists the 1.4.3 words:
#   main+0xbdb578  movz x8, #1920            (0xd280f008)   the display configuration's default size, width
#   main+0xbdb584  movk x8, #1080, lsl #32   (0xf2c08708)   ... and height
#   main+0x3dcb28  lsr x4, x3, #32           (0xd360fc64)   the display configuration publishing its size (x3 = h<<32|w)
#   main+0x3dcb2c  str x3, [x19, #0x508]     (0xf9028663)   its active crop (checked, not changed)
#   main+0x3dcb30  adrp x8, ...              (0xd001ac08)   (checked, not changed)
# and writes the canvas size into the first two and, at the third, also into display 0's canvas (+0x10), which sizes
# every UI / composition target (the crop at +0x508 alone leaves them at 1080p). x3 at that point is only the size
# being published. Here the same is lifted in place: when totk_output_res_on() the moves give totk_output_size and
# the publication point stores it to [x19, #0x10] and x3 first. The game multiplies its scene size by canvas width /
# 1920, which src/vk/dlss.c divides out again. Optional, like render_res: any other word warns and keeps the original
# code, and main_output_res_patch is 0 (the setting then logs that it is unavailable and the canvas stays 1080p).
_OR_WORDS = [(0xbdb578, 0xd280f008), (0xbdb584, 0xf2c08708), (0x3dcb28, 0xd360fc64), (0x3dcb2c, 0xf9028663),
             (0x3dcb30, 0xd001ac08)]
_or_state = []


def _words_ok(gen, words, state, name, effect):
    if not state:
        bad = [(off, w, gen.word(MAIN + off)) for off, w in words
               if gen.lo <= MAIN + off < gen.hi and gen.word(MAIN + off) != w]
        state.append(not bad)
        for off, w, got in bad:
            print(f'WARNING: optional patch {name} at {MAIN + off:#x}: expected {w:#010x}, found {got:#010x}: '
                  f'skipped ({effect})', file=sys.stderr, flush=True)
    return state[0]


def output_res_ok(gen):
    return _words_ok(gen, _OR_WORDS, _or_state, 'output_res', 'the canvas stays 1920x1080')


_OS = 'extern uint64_t totk_output_size; '


def _output_res(kind):
    def patch(gen, addr):
        APPLIED.add(addr)
        orig = _orig(gen, addr)
        if not output_res_ok(gen):
            return orig
        if kind == 'w':     # movz x8, #w
            new = '{ ' + _OS + 'c->x[8] = totk_output_size & 0xffffu; }'
        elif kind == 'h':   # movk x8, #h, lsl #32
            new = '{ ' + _OS + 'c->x[8] = (c->x[8] & ~0xffff00000000ull) | (totk_output_size & 0xffff00000000ull); }'
        else:               # the size being published: the canvas, also into display 0's canvas, then the original lsr
            new = '{ ' + _OS + 'c->x[3] = totk_output_size; } ' + _code(gen, addr, 0xf9000a63) + ' ' + orig
        return _switched('totk_output_res_on', new, orig)
    return patch


OUTPUT_RES = [(MAIN + 0xbdb578, _output_res('w')), (MAIN + 0xbdb584, _output_res('h')),
              (MAIN + 0x3dcb28, _output_res('pub'))]


# ---------------------------------------------------------------- picture shape (TOTK_ASPECT, src/host/uit_display.c)
# The places that hand an aspect ratio to the camera's perspective projection (1.4.3, from MacNativeGames
# aspect_patch.h / game_patches_v143.c, MIT, found there with a watchpoint on the copies of DefaultAspect):
#   main+0x628c74 camera update, main+0xb43460 camera setup, main+0x1ff04f4 and main+0x253fbb0 viewport.
# Each is an instruction that leaves the ratio in an s register (fdiv s<d>, ... or ldr s<d>, [x<n>, #imm]); it runs
# as shipped and then, when totk_aspect_on(), s<d> is loaded from totk_aspect_ratio (x3 saved and restored, as for
# render_res). The vertical view stays (Hor+). MacNativeGames does not list the original words, only the two forms,
# so this checks the form; any other instruction warns and keeps the original code (main_aspect_patch = 0).
_ASPECT_SITES = (0x628c74, 0xb43460, 0x1ff04f4, 0x253fbb0)
_as_state = []


def _aspect_form(w):
    return (w & 0xffe0fc00) == 0x1e201800 or (w & 0xffc00000) == 0xbd400000   # fdiv s / ldr s, [xn, #imm]


def aspect_ok(gen):
    if not _as_state:
        bad = [(off, gen.word(MAIN + off)) for off in _ASPECT_SITES
               if gen.lo <= MAIN + off < gen.hi and not _aspect_form(gen.word(MAIN + off))]
        _as_state.append(not bad)
        for off, got in bad:
            print(f'WARNING: optional patch aspect at {MAIN + off:#x}: found {got:#010x}, not fdiv s / ldr s: skipped '
                  '(the camera keeps 16:9)', file=sys.stderr, flush=True)
    return _as_state[0]


def _aspect(gen, addr):
    APPLIED.add(addr)
    orig = _orig(gen, addr)
    if not aspect_ok(gen):
        return orig
    d = gen.word(addr) & 31
    load = ('{ extern float totk_aspect_ratio; uint64_t x3_ = c->x[3]; '
            'c->x[3] = (uint64_t)(uintptr_t)&totk_aspect_ratio; ' + _code(gen, addr, 0xbd400060 | d) +
            ' c->x[3] = x3_; }')   # ldr s<d>, [x3]
    return _switched('totk_aspect_on', orig + ' ' + load, orig)


ASPECT = [(MAIN + off, _aspect) for off in _ASPECT_SITES]


# ---------------------------------------------------------------- cheats (src/host/cheats.c, overlay / launcher Cheats)
# [UIT 0175] Instruction words from the TotK 1.4.3 (build ID 277178B7DBA1B6D4) entries of TKMM's Cheats.json (MIT,
# credits @Arch9SK7 / MyNXCheats); only the addresses and words are taken. Each site checks the shipped word; any
# other word warns and keeps the original code (main_cheat_*_patch = 0, the setting then says it is not in this build).
# All are switched at run time (uit_cheat_durability / _amiibo / _stamina / _arrows): with the setting off the original
# instructions run.
# Infinite item durability:
#   main+0x21eca9c  msub w8, w10, w9, w8 (0x1b09a148) -> msub w8, wzr, w9, w8 (0x1b09a3e8): the wear taken off a
#                   weapon / bow / shield per hit becomes 0 (the value stays w8)
#   main+0x282e58, main+0x2829f4  ldr w1, [x8, #8] (0xb9400901) -> ldr w1, [x8, #0x18]; str w1, [x8, #8]: the
#                   current durability read for the item is its maximum, written back (an item already worn is
#                   repaired the next time it is read)
# Limitless amiibo scanning:
#   main+0x1f87e08  tbnz w0, #0, main+0x1f87e44 (0x370001e0): the "already used today" answer; with the cheat the
#                   branch is never taken (the public cheat writes a nop there). The 1.4.2 entry of the same file (BID 5CB42B1C...,
#                   main+0x1f81e88, the same word) is marked verified; 1.2.1's (0x194c8ec tbz w0 -> b) is the same check
#                   the other way round: bit 0 set = used today. [UIT 0180] Each run of the check is logged.
# [UIT 0344] Infinite stamina:
#   main+0x31d398   fmadd s0, s0, s8, s2 (0x1f080800) -> fmov s0, s2 (0x1e204040): the stamina after this frame's
#                   use (s2 + s0 * s8) stays the stamina before it (s2). Registers only (s0 written, as before), no
#                   flags, no memory: safe to switch per execution.
# [UIT 0345] Infinite arrows:
#   main+0x1b78cd0  mov w1, #-1 (0x12800001) -> mov w1, wzr (0x2a1f03e1): the change to the arrow count the game
#                   passes on (one arrow fewer) becomes zero. Registers only (w1 written, as before), no flags, no
#                   memory: safe to switch per execution.
#   The original words are the file's [Restore Code] entries for the same addresses; the lift checks them against the
#   player's own game code, so another version keeps the game's code and the setting says "Not in this build".
_DUR_WORDS = [(0x21eca9c, 0x1b09a148), (0x282e58, 0xb9400901), (0x2829f4, 0xb9400901)]
_AMIIBO_WORDS = [(0x1f87e08, 0x370001e0)]
_STAMINA_WORDS = [(0x31d398, 0x1f080800)]
_ARROW_WORDS = [(0x1b78cd0, 0x12800001)]
_dur_state, _amiibo_state, _stamina_state, _arrow_state = [], [], [], []


def _simple_cheat(words, state, name, cond, effect, new):
    """[UIT 0344] a one-word cheat switched at run time by cond() (src/host/cheats.c); the original code otherwise"""
    def patch(gen, addr):
        APPLIED.add(addr)
        orig = _orig(gen, addr)
        if not _words_ok(gen, words, state, name, effect):
            return orig
        return _switched(cond, _words(new)(gen, addr), orig)
    return patch


def _durability(new):
    def patch(gen, addr):
        APPLIED.add(addr)
        orig = _orig(gen, addr)
        if not _words_ok(gen, _DUR_WORDS, _dur_state, 'cheat_durability', 'Infinite item durability is not available'):
            return orig
        return _switched('uit_cheat_durability', _words(*new)(gen, addr), orig)
    return patch


def _amiibo_limit(gen, addr):
    APPLIED.add(addr)
    ins = gen.lift(addr)
    orig = ' '.join(gen.render_single(ins, addr, gen._jump, gen._refs_now))
    if (not _words_ok(gen, _AMIIBO_WORDS, _amiibo_state, 'cheat_amiibo', 'Limitless amiibo scanning is not available')
            or ins.kind != 'bcond' or ins.target != MAIN + 0x1f87e44):
        if _amiibo_state and _amiibo_state[0]:
            _amiibo_state[0] = False
            print(f'WARNING: optional patch cheat_amiibo at {addr:#x}: not a conditional branch to main+0x1f87e44: '
                  'skipped (Limitless amiibo scanning is not available)', file=sys.stderr, flush=True)
        return orig
    # [UIT 0180] through src/host/cheats.c's uit_cheat_amiibo_site: it logs what the game answered here and whether
    # the cheat let it through (a report then shows if this check is ever reached)
    return (f'{{ extern int uit_cheat_amiibo_site(int used); if (uit_cheat_amiibo_site(({ins.cond}) != 0)) '
            f'{{ {gen._jump(ins.target)} }} }}')


CHEATS = [(MAIN + 0x21eca9c, _durability([0x1b09a3e8])),
          (MAIN + 0x282e58, _durability([0xb9401901, 0xb9000901])),
          (MAIN + 0x2829f4, _durability([0xb9401901, 0xb9000901])),
          (MAIN + 0x1f87e08, _amiibo_limit),
          (MAIN + 0x31d398, _simple_cheat(_STAMINA_WORDS, _stamina_state, 'cheat_stamina', 'uit_cheat_stamina',
                                          'Infinite stamina is not available', 0x1e204040)),
          (MAIN + 0x1b78cd0, _simple_cheat(_ARROW_WORDS, _arrow_state, 'cheat_arrows', 'uit_cheat_arrows',
                                           'Infinite arrows is not available', 0x2a1f03e1))]   # [UIT 0345]


def table_flags(lo, hi):
    """C definitions for the module's table file (gen.py): which optional patches this lift applied."""
    if not lo <= MAIN + 0x585e50 < hi:
        return ''
    on = int(bool(_rr_state and _rr_state[0] and MAIN + 0x585e50 in APPLIED and MAIN + 0x585e60 in APPLIED))
    out = int(bool(_or_state and _or_state[0] and all(a in APPLIED for a, _ in OUTPUT_RES)))
    asp = int(bool(_as_state and _as_state[0] and all(a in APPLIED for a, _ in ASPECT)))
    import scan   # [UIT 0070] the sites tools/lifter/scan.py found in this main (0 = not found: unavailable)
    dur = int(bool(_dur_state and _dur_state[0] and all(MAIN + o in APPLIED for o, _ in _DUR_WORDS)))
    ami = int(bool(_amiibo_state and _amiibo_state[0] and MAIN + 0x1f87e08 in APPLIED))
    def note(st, on_):   # data/generated/lift-features.ini (the launcher greys the setting out with it)
        return '' if on_ else ('the TotK 1.4.3 instructions are not at the cheat\'s addresses (another game version?)'
                               if st and not st[0] else 'the cheat\'s code was in no lifted function')
    arr = int(bool(_arrow_state and _arrow_state[0] and all(MAIN + o in APPLIED for o, _ in _ARROW_WORDS)))   # [UIT 0345]
    sta = int(bool(_stamina_state and _stamina_state[0] and all(MAIN + o in APPLIED for o, _ in _STAMINA_WORDS)))
    scan.STATE['extra'] = {'cheat_durability': (dur, note(_dur_state, dur)), 'cheat_amiibo': (ami, note(_amiibo_state, ami)),
                           'cheat_stamina': (sta, note(_stamina_state, sta)), 'cheat_arrows': (arr, note(_arrow_state, arr))}
    return (f'const int main_render_res_patch = {on};\n'
            f'const int main_output_res_patch = {out};\n'
            f'const int main_aspect_patch = {asp};\n'
            f'const int main_cheat_durability_patch = {dur};\n'   # [UIT 0175]
            f'const int main_cheat_amiibo_patch = {ami};\n'
            f'const int main_cheat_stamina_patch = {sta};\n'   # [UIT 0344]
            f'const int main_cheat_arrows_patch = {arr};\n' + scan.table_c(APPLIED))   # [UIT 0345]


PATCHES = {
    0x8005843e0: drs_off,
    **dict(FRAME_RATE),
    **dict(RENDER_RES),
    **dict(OUTPUT_RES),
    **dict(ASPECT),
    **dict(CHEATS),
}
OPTIONAL = {a for a, _ in OUTPUT_RES + ASPECT + CHEATS}   # gen.py: not emitted -> a warning, not a stopped lift
assert len(PATCHES) == 1 + len(FRAME_RATE) + len(RENDER_RES) + len(OUTPUT_RES) + len(ASPECT) + len(CHEATS), 'a patch site is listed twice'

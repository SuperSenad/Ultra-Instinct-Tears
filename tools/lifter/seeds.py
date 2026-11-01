"""Extra entry points for addresses the game reaches but the lifter's discovery missed (gen.py).

Each address gets an alias entry: a function in the dispatch table under its own name that re-enters the lifted
function containing it at that instruction (Cpu.entry_at; the containing function keeps its boundaries, labels and
jump tables, so nothing else is re-partitioned). That covers both ways a missing address is reached: a computed
branch of the containing function whose table missed it, and a branch or call from anywhere else. The lift log
(data/generated/main.log) gets one "seed" line per address naming the containing function and its indirect
branches, which is what an upstream issue needs.

Addresses are guest addresses of main (load address 0x800000000) for TOTK 1.4.3. Only add an address from a
"FATAL: no function at <address>" line of a real run: the alias makes it a valid jump target for the whole game.
"""

SEEDS = {
    # In-engine cutscene early in the game, 100% reproducible at 30 and 60 FPS (UltraTears 1970c7a + FSR/FPS
    # patches, 2026-10-29): "FATAL: no function at 8015b9fb0 (lr=8 ?+8)" under f_801529980 <- f_801517ea0
    # <- f_801518320 <- f_801517a30 <- f_801511fe0. lr=8 (x30 used as a scratch register) points at a jump table
    # in a large function rather than a call (blr would have set lr to the call's return address).
    0x8015b9fb0: 'cutscene crash, run 2026-10-29',
}

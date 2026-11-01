/* [UIT 0175] Cheats, switched live: the lifted game code asks these at the patched instructions
 * (tools/lifter/patches.py CHEATS, words from TKMM's Cheats.json for TotK 1.4.3, MIT, credits @Arch9SK7 / MyNXCheats).
 *
 *   TOTK_CHEAT_DURABILITY=1  Infinite item durability: a hit takes no wear off a weapon, bow or shield, and an item's
 *                            durability reads as its maximum (main+0x21eca9c, main+0x282e58, main+0x2829f4)
 *   TOTK_CHEAT_AMIIBO=1      Limitless amiibo scanning: every scan is a figure with a new serial number, and the
 *                            once-a-day check (main+0x1f87e08) always goes the "not used today" way [UIT 0210]
 *
 * Off (the default) runs the game's own instructions. The overlay's Cheats tab flips them while playing; the launcher's
 * Game > Cheats sets the start value (Mods > Disable Mods turns both off). main_cheat_*_patch = 0 (the 1.4.3 words
 * were not found when the code was lifted): the switch stays off and says why. Cost: one relaxed load per call. */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "uit_str.h"

extern const int main_cheat_durability_patch, main_cheat_amiibo_patch;   /* the lift's main_table.c */

static atomic_int g_dur = -1, g_ami = -1;

static int env_on(const char *k) {
    const char *e = getenv(k);
    return e && (!strcmp(e, "1") || !uit_stricmp(e, "on") || !uit_stricmp(e, "true") || !uit_stricmp(e, "yes"));
}

static int get(atomic_int *v, const char *key, int avail) {
    int x = atomic_load_explicit(v, memory_order_relaxed);
    if (__builtin_expect(x < 0, 0)) {
        int want = avail && env_on(key), was = -1;
        atomic_compare_exchange_strong(v, &was, want);   /* a set() first wins */
        x = atomic_load_explicit(v, memory_order_relaxed);
    }
    return x;
}

int uit_cheat_durability(void) { return get(&g_dur, "TOTK_CHEAT_DURABILITY", main_cheat_durability_patch); }
int uit_cheat_amiibo(void) { return get(&g_ami, "TOTK_CHEAT_AMIIBO", main_cheat_amiibo_patch); }
int uit_cheat_durability_available(void) { return main_cheat_durability_patch != 0; }
int uit_cheat_amiibo_available(void) { return main_cheat_amiibo_patch != 0; }

/* [UIT 0180, 0210] The patched instruction itself (main+0x1f87e08, tools/lifter/patches.py _amiibo_limit) calls this with
 * the game's answer: 1 = its tbnz branch would be taken (the branch the public 1.4.3 cheat writes a nop over). Returns
 * whether to take it.
 *
 * Why not always the nop (0180): a report (36fb2fc, 31 Oct) had the check answer 1 for the first figure of the session
 * with the cheat on, the branch skipped, and no reward. The public cheats for 1.0 / 1.1 / 1.2.1 force the other
 * instruction there (tbz -> b), so which way 1.4.3's branch goes cannot be told from the words alone, and the game code
 * isn't here to read. So, with the cheat on (TOTK_CHEAT_AMIIBO_BRANCH):
 *   learn (default)  amiibo.c gives every scanned figure a new serial number (UIT 0210), which the game cannot have
 *                    seen today; the first answer this check gives is therefore the "not used today" one, whichever
 *                    way that is coded. Every later run takes that same branch: the limit is gone either way.
 *   nop              the public cheat: never taken
 *   game             the game's own answer (only the new serial numbers)
 * Each run is logged ([amiibo] / diag/amiibo.txt: the first 64, then every 100th) with the game's answer and what was
 * done, so a report shows the check ran for each scan. */
void runtime_amiibo_log(const char *fmt, ...);   /* nfp.c */
static atomic_uint g_ami_hits;
static atomic_int g_ami_fresh = -1;   /* learn: the branch a never-seen serial number gets */
enum { AMI_LEARN, AMI_NOP, AMI_GAME };
static int ami_mode(void) {
    const char *e = getenv("TOTK_CHEAT_AMIIBO_BRANCH");
    if (e && !uit_stricmp(e, "nop")) return AMI_NOP;
    if (e && !uit_stricmp(e, "game")) return AMI_GAME;
    return AMI_LEARN;
}
int uit_cheat_amiibo_site(int taken) {
    taken = taken != 0;
    int on = uit_cheat_amiibo(), take = taken;
    unsigned n = atomic_fetch_add_explicit(&g_ami_hits, 1, memory_order_relaxed) + 1;
    const char *what;
    if (!on) what = "Limitless amiibo scanning is off: the game's own answer";
    else switch (ami_mode()) {
    case AMI_NOP: take = 0; what = "TOTK_CHEAT_AMIIBO_BRANCH=nop: not taken (the public cheat)"; break;
    case AMI_GAME: what = "TOTK_CHEAT_AMIIBO_BRANCH=game: the game's own answer (new serial numbers only)"; break;
    default: {
        int was = -1;
        if (atomic_compare_exchange_strong(&g_ami_fresh, &was, taken))
            what = "Limitless amiibo scanning: the first check with a new serial number, so this is the game's "
                   "\"not used today\" answer; every later check gets the same";
        else what = (take = atomic_load(&g_ami_fresh)) == taken ? "Limitless amiibo scanning: the same as a figure never seen"
                                                              : "Limitless amiibo scanning: changed to what a figure never seen gets";
    } }
    if (n <= 64 || n % 100 == 0)
        runtime_amiibo_log("the game's once-a-day amiibo check ran (main+0x1f87e08, %u time%s so far): its branch %s; %s: %s",
                           n, n == 1 ? "" : "s", taken ? "would be taken" : "would not be taken", what,
                           take ? "taken" : "not taken");
    return take;
}
/* 1 = now on / off as asked; 0 = not in this build (stays off) */
int uit_cheat_set_durability(int on) {
    atomic_store(&g_dur, on && main_cheat_durability_patch);
    return !on || main_cheat_durability_patch;
}
int uit_cheat_set_amiibo(int on) {
    atomic_store(&g_ami, on && main_cheat_amiibo_patch);
    return !on || main_cheat_amiibo_patch;
}

// [UIT 0177] nn::swkbd::ShowKeyboard without the keyboard applet. The SDK's own code builds the ShowKeyboardArg (the
// presets, the texts) and would then start the library applet, which this host does not have (the crash naming a
// horse: CreateLibraryApplet -> StartLibraryAppletImmediately). Both overloads are taken here: the argument is read
// (swkbd.c), the text is typed in the overlay's dialog (overlay_live.c / uit_overlay.c, on the window thread) and
// written into the game's String. The game's thread waits like a Horizon wait (runtime_core_away), so the window keeps
// running and presenting meanwhile.
//   TOTK_SWKBD_ANSWER=<text>   answer every request with this text without a dialog (<cancel>: cancel, <initial>:
//                              the game's initial text) -- for tests and scripted runs
// The text check callback (KeyboardConfig.textCheck) is not called: the answer is what the filters let through.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "overlay_live.h"
#include "swkbd.h"
#include "../runtime/hle.h"

int runtime_core_away(void);
void runtime_core_back(int *away);

static void u8(const uint16_t *t, size_t max, char *out, size_t cap) {
    size_t w = uit_swkbd_utf16_to_utf8(t, uit_swkbd_u16len(t, max), out, cap - 1);
    out[w] = 0;
}

// TOTK_SWKBD_ANSWER: 1 answered (ok / t / n set), 0 not set
static int scripted(const UitSwkbdReq *r, int *ok, uint16_t *t, int *n) {
    const char *a = getenv("TOTK_SWKBD_ANSWER");
    if (!a) return 0;
    *ok = 1;
    if (!strcmp(a, "<cancel>")) { *ok = 0; *n = 0; return 1; }
    if (!strcmp(a, "<initial>")) { *n = r->initial_len; memcpy(t, r->initial, (size_t)*n * 2); return 1; }
    uint16_t tmp[UIT_SWKBD_LIMIT * 2];
    size_t m = uit_swkbd_utf8_to_utf16(a, strlen(a), tmp, sizeof tmp / 2);
    m = uit_swkbd_clean(r, tmp, m);
    memcpy(t, tmp, m * 2);
    *n = (int)m;
    return 1;
}

static pthread_mutex_t one = PTHREAD_MUTEX_INITIALIZER;   // one keyboard at a time, as the applet

static uint32_t show(uint64_t str, uint64_t arg) {
    if (!str || !arg) { fprintf(stderr, "[swkbd] ShowKeyboard without a String or an argument: cancelled\n"); return UIT_SWKBD_RESULT_CANCELED; }
    __attribute__((cleanup(runtime_core_back))) int away = runtime_core_away();   // blocks like a Horizon wait
    (void)away;
    pthread_mutex_lock(&one);
    static UitSwkbdReq req;   // under `one` (2.5 KB: off the game thread's stack)
    static uint16_t text[UIT_SWKBD_LIMIT + 1];
    uit_swkbd_parse((const uint8_t *)(uintptr_t)arg, &req);
    char head[0x41 * 3 + 1], init[UIT_SWKBD_LIMIT * 3 + 1];
    u8(req.header, 0x40, head, sizeof head);
    u8(req.initial, (size_t)req.initial_len, init, sizeof init);
    fprintf(stderr, "[swkbd] ShowKeyboard: mode %u, %d..%d characters, disabled 0x%x, %s%s, header \"%s\", initial \"%s\"%s\n",
            req.mode, req.min_len, req.max_len, req.invalid, req.utf8 ? "UTF-8" : "UTF-16", req.password ? ", password" : "",
            head, req.password ? "(hidden)" : init, req.work_at ? "" : " (no work buffer found)");
    if (req.text_check) fprintf(stderr, "[swkbd] the game's text check is not called\n");
    int ok = 0, n = 0;
    const char *how = "dialog";
    if (scripted(&req, &ok, text, &n)) how = "TOTK_SWKBD_ANSWER";
    else {
        int r = uit_overlay_kb_run(&req, text, &n);
        if (r >= 0) ok = r;
        else {   // no window to type in: the game's own text if it gave one, else cancelled
            how = "no dialog";
            n = req.initial_len;
            memcpy(text, req.initial, (size_t)n * 2);
            ok = n > 0;
        }
    }
    uint64_t buf = *(const uint64_t *)(uintptr_t)str, size = *(const uint64_t *)(uintptr_t)(str + 8);
    uint32_t res = buf && size ? uit_swkbd_finish(&req, ok, text, (size_t)n, (void *)(uintptr_t)buf, size)
                               : UIT_SWKBD_RESULT_CANCELED;
    char out[UIT_SWKBD_LIMIT * 3 + 1];
    u8(text, (size_t)n, out, sizeof out);
    if (res) fprintf(stderr, "[swkbd] cancelled (%s)\n", how);
    else fprintf(stderr, "[swkbd] answered (%s): \"%s\" (%d units)\n", how, req.password ? "(hidden)" : out, n);
    pthread_mutex_unlock(&one);
    return res;
}

static uint32_t show_trigger(uint64_t str, uint64_t arg, uint64_t trigger) { (void)trigger; return show(str, arg); }

HLE_NATIVE("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", show, 'w')
HLE_NATIVE("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgENS0_7TriggerE", show_trigger, 'w')

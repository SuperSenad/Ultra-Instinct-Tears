/* nn::nfp for the game: one always-present reader fed by the overlay's Amiibo tab (amiibo files on the computer).
 * [UIT 0175] The state machine and dump parsing are in amiibo.c (portable, unit-tested); this file binds the game's
 * imports to it, raises the game's events, and keeps each figure's application area in
 * <profile>/amiibo/<serial>-<amiibo id>.bin (4-byte access ID, then the area). No keys: nothing is decrypted.
 * Replaces the "no NFC reader" stubs that were in overrides_applet.c. Signatures: switchbrew.org NFC_services and the
 * game's own import names.
 * [UIT 0180] Everything the game asks of the reader, the overlay's scans and the amiibo cheat's check go to the log
 * ("[amiibo] ...", only with Play with log) and always to diag/amiibo.txt in the game folder (this session's, at most
 * AM_LOG_MAX lines; the launcher's Report a problem adds it), so a report shows what happened without a log. */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "amiibo.h"
#include "cheats.h"
#include "host.h"
#include "../runtime/hle.h"

const char *runtime_profile_dir(void);
int runtime_handheld_mode(void);

static _Atomic(void *) ev_activate, ev_deactivate, ev_avail;

static uint64_t clock_ms(void) { return runtime_now_ns() / 1000000u; }

static int area_path(const char *key, char *path, size_t n, int make_dir) {
    const char *dir = runtime_profile_dir();
    if (make_dir) {
        if (mkdir(dir, 0700) && errno != EEXIST) return -1;
        char sub[4096];
        if (snprintf(sub, sizeof sub, "%s/amiibo", dir) >= (int)sizeof sub) return -1;
        if (mkdir(sub, 0700) && errno != EEXIST) return -1;
    }
    return snprintf(path, n, "%s/amiibo/%s.bin", dir, key) >= (int)n ? -1 : 0;
}
static int area_load(const char *key, uint8_t *buf, size_t cap, uint32_t *access_id) {
    char path[4096];
    if (area_path(key, path, sizeof path, 0)) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t hdr[4];
    int n = -1;
    if (fread(hdr, 1, 4, f) == 4) {
        *access_id = (uint32_t)hdr[0] | (uint32_t)hdr[1] << 8 | (uint32_t)hdr[2] << 16 | (uint32_t)hdr[3] << 24;
        n = (int)fread(buf, 1, cap, f);
    }
    fclose(f);
    return n;
}
static int area_save(const char *key, const uint8_t *buf, size_t n, uint32_t access_id) {
    char path[4096], tmp[4200];
    if (area_path(key, path, sizeof path, 1)) { fprintf(stderr, "[totk] amiibo: can't create the amiibo save folder\n"); return -1; }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { fprintf(stderr, "[totk] amiibo: can't write %s\n", tmp); return -1; }
    uint8_t hdr[4] = {(uint8_t)access_id, (uint8_t)(access_id >> 8), (uint8_t)(access_id >> 16), (uint8_t)(access_id >> 24)};
    int ok = fwrite(hdr, 1, 4, f) == 4 && fwrite(buf, 1, n, f) == n;
    ok = !fclose(f) && ok;
    if (ok) { remove(path); ok = !rename(tmp, path); }   /* Windows rename does not replace */
    if (!ok) { remove(tmp); fprintf(stderr, "[totk] amiibo: can't save %s\n", path); return -1; }
    return 0;
}

/* ---- [UIT 0180] the amiibo log ---- */
#define AM_LOG_MAX 4000
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_file;
static int log_lines, log_failed;
static void log_line(const char *line) {   /* any thread */
    pthread_mutex_lock(&log_mu);
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char hms[16] = "--:--:--";
    if (tm) strftime(hms, sizeof hms, "%H:%M:%S", tm);
    fprintf(stderr, "[amiibo] %s\n", line);
    if (!log_file && !log_failed) {
        mkdir("diag", 0777);
        log_file = fopen("diag/amiibo.txt", "w");
        if (!log_file) log_failed = 1;
        else {
            char day[32] = "";
            if (tm) strftime(day, sizeof day, "%Y-%m-%d %H:%M:%S", tm);
            fprintf(log_file, "# Ultra Instinct Tears: the amiibo reader and the amiibo cheat this session (src/host/nfp.c), "
                              "started %s (local time)\n", day);
        }
    }
    if (log_file && log_lines < AM_LOG_MAX) {
        log_lines++;
        fprintf(log_file, "%s %s\n", hms, log_lines == AM_LOG_MAX ? "(full: the rest only in the log)" : line);
        fflush(log_file);
    }
    pthread_mutex_unlock(&log_mu);
}
void runtime_amiibo_log(const char *fmt, ...) {
    char b[400];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    log_line(b);
}

__attribute__((constructor)) static void nfp_setup(void) {
    static const UitNfpStore st = {area_load, area_save};
    uit_nfp_setup(clock_ms, &st);
    uit_nfp_set_log(log_line);
    /* [UIT 0210] Limitless amiibo scanning gives each scan a new serial number (amiibo.c) */
    uit_nfp_set_fresh_serial(uit_cheat_amiibo, ((uint64_t)time(NULL) * 0x100000001b3ull) ^ (uint64_t)(uintptr_t)&st ^ 1);
    const char *e = getenv("TOTK_AMIIBO_READER_MS");   /* [UIT 0208] */
    if (e && *e) {
        int set = uit_nfp_set_present_ms(atoi(e));
        char b[160];
        snprintf(b, sizeof b, "TOTK_AMIIBO_READER_MS=%s: a scanned figure stays on the reader %d ms", e, set);
        log_line(b);
    }
}

/* Raise what the state machine owes the game; any thread (the overlay's tick runs on the main thread). */
static void flush_signals(void) {
    unsigned s = uit_nfp_take_signals();
    void *e;
    if ((s & UIT_NFP_SIG_ACTIVATE) && (e = atomic_load(&ev_activate))) hle_signal_system_event_alive(e);
    else if (s & UIT_NFP_SIG_ACTIVATE) runtime_amiibo_log("the game has no activate event attached: it is not told a figure arrived");
    if ((s & UIT_NFP_SIG_DEACTIVATE) && (e = atomic_load(&ev_deactivate))) hle_signal_system_event_alive(e);
    if ((s & UIT_NFP_SIG_AVAILABILITY) && (e = atomic_load(&ev_avail))) hle_signal_system_event_alive(e);
}
#define DONE(expr) do { uint32_t r_ = (expr); flush_signals(); return r_; } while (0)

/* ---- overlay side (overlay_live.c, main thread) ---- */
int runtime_nfp_present(const UitAmiibo *a) { int r = uit_nfp_present(a); flush_signals(); return r; }
void runtime_nfp_tick(void) { uit_nfp_tick(); flush_signals(); }

/* ---- the game's imports ---- */
static uint64_t H(const void *handle) { uint64_t h = 0; if (handle) memcpy(&h, handle, 8); return h; }
static void attach(_Atomic(void *) *slot, void *ev, int autoclear) {
    hle_forget_system_event(ev);   /* the game may have destroyed an earlier event at this address */
    hle_bind_system_event(ev, autoclear);
    atomic_store(slot, ev);
}

static unsigned nfp_initialize(void) { DONE(uit_nfp_initialize()); }
static unsigned nfp_finalize(void) {
    atomic_store(&ev_activate, NULL); atomic_store(&ev_deactivate, NULL); atomic_store(&ev_avail, NULL);
    uint32_t r = uit_nfp_finalize();
    uit_nfp_take_signals();
    return r;
}
static int nfp_get_state(void) { return uit_nfp_state(); }
static unsigned nfp_list(void *handles, int *count, int max) {
    uint64_t h[1];
    uint32_t r = uit_nfp_list(h, count, max);
    if (!r) memcpy(handles, h, 8);
    DONE(r);
}
static unsigned attach_said(const char *call, uint32_t r) {
    runtime_amiibo_log("%s: %s", call, uit_nfp_result_text(r));
    return r;
}
static unsigned nfp_attach_availability(void *ev) {
    if (!ev) return attach_said("AttachAvailabilityChangeEvent", UIT_NFP_INVALID_ARGUMENT);
    attach(&ev_avail, ev, 0);
    return attach_said("AttachAvailabilityChangeEvent", 0);
}
static unsigned nfp_attach_activate(void *ev, const void *h) {
    uint32_t r = uit_nfp_check(H(h));
    if (!r && !ev) r = UIT_NFP_INVALID_ARGUMENT;
    if (!r) attach(&ev_activate, ev, 1);
    return attach_said("AttachActivateEvent", r);
}
static unsigned nfp_attach_deactivate(void *ev, const void *h) {
    uint32_t r = uit_nfp_check(H(h));
    if (!r && !ev) r = UIT_NFP_INVALID_ARGUMENT;
    if (!r) attach(&ev_deactivate, ev, 1);
    return attach_said("AttachDeactivateEvent", r);
}
static int nfp_device_state(const void *h) {
    static _Atomic int seen = -1;   /* [UIT 0180] the game polls this: only a change is logged */
    int s = uit_nfp_device_state(H(h));
    if (atomic_exchange(&seen, s) != s) runtime_amiibo_log("GetDeviceState: %s", uit_nfp_state_text(s));
    flush_signals();
    return s;
}
static unsigned nfp_npad_id(uint32_t *out, const void *h) {
    uint32_t r = uit_nfp_check(H(h));
    if (r) runtime_amiibo_log("GetNpadId: %s", uit_nfp_result_text(r));
    if (!r && out) *out = runtime_handheld_mode() ? 0x20 : 0;   /* handheld, else player 1 */
    return r;
}
static unsigned nfp_start(const void *h) { DONE(uit_nfp_start(H(h))); }
static unsigned nfp_stop(const void *h) { DONE(uit_nfp_stop(H(h))); }
static unsigned nfp_mount(const void *h, int model_type, int target) { DONE(uit_nfp_mount(H(h), model_type, target)); }
static unsigned nfp_unmount(const void *h) { DONE(uit_nfp_unmount(H(h))); }
static unsigned nfp_tag_info(void *out, const void *h) { DONE(out ? uit_nfp_tag_info(H(h), out) : UIT_NFP_INVALID_ARGUMENT); }
static unsigned nfp_model_info(void *out, const void *h) { DONE(out ? uit_nfp_model_info(H(h), out) : UIT_NFP_INVALID_ARGUMENT); }
static unsigned nfp_common_info(void *out, const void *h) { DONE(out ? uit_nfp_common_info(H(h), out) : UIT_NFP_INVALID_ARGUMENT); }
static unsigned nfp_register_info(void *out, const void *h) { DONE(out ? uit_nfp_register_info(H(h), out) : UIT_NFP_INVALID_ARGUMENT); }
static unsigned nfp_open_area(const void *h, uint32_t access_id) { DONE(uit_nfp_open_area(H(h), access_id)); }
static unsigned nfp_get_area(void *buf, uint64_t *size, const void *h, uint64_t cap) { DONE(uit_nfp_get_area(H(h), buf, size, cap)); }
static unsigned nfp_set_area(const void *h, const void *buf, uint64_t n) { DONE(uit_nfp_set_area(H(h), buf, n)); }
/* nn::nfp::ApplicationAreaCreateInfo: access ID (u32), then the initial data pointer and its size (8-byte aligned). */
static unsigned nfp_create_area(const void *h, const uint8_t *info) {
    if (!info) return UIT_NFP_INVALID_ARGUMENT;
    uint32_t access; uint64_t ptr, n;
    memcpy(&access, info, 4); memcpy(&ptr, info + 8, 8); memcpy(&n, info + 16, 8);
    n &= 0xFFFFFFFFu;
    DONE(uit_nfp_create_area(H(h), access, (const void *)(uintptr_t)ptr, n));
}
static unsigned nfp_flush(const void *h) { DONE(uit_nfp_flush(H(h))); }

/* The 14 the game imports (TotK 1.4.3)... */
HLE_NATIVE("_ZN2nn3nfp10InitializeEv", nfp_initialize, 'w')
HLE_NATIVE("_ZN2nn3nfp8FinalizeEv", nfp_finalize, 'w')
HLE_NATIVE("_ZN2nn3nfp8GetStateEv", nfp_get_state, 'w')
HLE_NATIVE("_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", nfp_list, 'w')
HLE_NATIVE("_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", nfp_attach_availability, 'w')
HLE_NATIVE("_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", nfp_device_state, 'w')
HLE_NATIVE("_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", nfp_mount, 'w')
HLE_NATIVE("_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", nfp_npad_id, 'w')
HLE_NATIVE("_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", nfp_attach_activate, 'w')
HLE_NATIVE("_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", nfp_attach_deactivate, 'w')
HLE_NATIVE("_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", nfp_start, 'w')
HLE_NATIVE("_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", nfp_stop, 'w')
HLE_NATIVE("_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", nfp_tag_info, 'w')
HLE_NATIVE("_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", nfp_model_info, 'w')
/* ...and the rest of the usual amiibo flow, in case a mod or another path reaches them (unused entries cost nothing). */
HLE_NATIVE("_ZN2nn3nfp7UnmountERKNS0_12DeviceHandleE", nfp_unmount, 'w')
HLE_NATIVE("_ZN2nn3nfp13GetCommonInfoEPNS0_10CommonInfoERKNS0_12DeviceHandleE", nfp_common_info, 'w')
HLE_NATIVE("_ZN2nn3nfp15GetRegisterInfoEPNS0_12RegisterInfoERKNS0_12DeviceHandleE", nfp_register_info, 'w')
HLE_NATIVE("_ZN2nn3nfp19OpenApplicationAreaERKNS0_12DeviceHandleEj", nfp_open_area, 'w')
HLE_NATIVE("_ZN2nn3nfp18GetApplicationAreaEPvPmRKNS0_12DeviceHandleEm", nfp_get_area, 'w')
HLE_NATIVE("_ZN2nn3nfp18SetApplicationAreaERKNS0_12DeviceHandleEPKvm", nfp_set_area, 'w')
HLE_NATIVE("_ZN2nn3nfp21CreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE", nfp_create_area, 'w')
HLE_NATIVE("_ZN2nn3nfp5FlushERKNS0_12DeviceHandleE", nfp_flush, 'w')

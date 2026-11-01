/* amiibo from files: dump parsing and the nn::nfp device the game talks to. [UIT 0175]
 *
 * Portable core (no SDL, no guest): nfp.c is the HLE glue, overlay_live.c the overlay side, tools/tests/test_amiibo.c
 * the unit test. No Nintendo keys are used anywhere: only the plaintext parts of a dump are read (the tag UID and the
 * amiibo ID), the encrypted sections are ignored, and the application area a game keeps on its amiibo lives in a
 * per-amiibo file under the profile folder (nfp.c) instead of on the "tag".
 *
 * Layouts and values: switchbrew.org NFC_services (TagInfo, CommonInfo, RegisterInfo, ModelInfo, DeviceState, Raw and
 * Plain formats) and Error_codes (module 115 = nfp). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- dumps ---- */
typedef struct {
    uint8_t uid[10]; int uid_len;   /* 7-byte NTAG215 serial number (without the check bytes) */
    uint8_t id[8];                  /* amiibo ID, as stored at tag offset 0x54 */
    char name[64];                  /* what the overlay shows: known name, JSON "Name", or the file name */
} UitAmiibo;

enum {
    UIT_AMIIBO_OK = 0,
    UIT_AMIIBO_EMPTY,        /* nothing to read */
    UIT_AMIIBO_SIZE,         /* not 532, 540 or 572 bytes, and not JSON or Flipper .nfc */
    UIT_AMIIBO_NOT_AMIIBO,   /* right size, but no amiibo header / ID / serial check bytes */
    UIT_AMIIBO_BAD_JSON,     /* JSON without a usable "AmiiboId" */
};
const char *uit_amiibo_error_text(int err);
/* d/n: the whole file. file_name: shown when the ID has no known name (may be NULL). */
int uit_amiibo_parse(const uint8_t *d, size_t n, const char *file_name, UitAmiibo *out);
const char *uit_amiibo_known_name(const uint8_t id[8]);   /* NULL if not in the table */
void uit_amiibo_key(const UitAmiibo *a, char *buf, size_t n);   /* "<uid hex>-<id hex>": app-area file name */
uint64_t uit_amiibo_id64(const uint8_t id[8]);
/* [UIT 0181] The built-in figures (the Zelda series amiibo of the names table, no file needed): how many, and the i-th
 * (0 = ok, -1 = no such one). Same ID and a fixed serial number each time, so the game's once-a-day limit applies. */
int uit_amiibo_builtin_count(void);
int uit_amiibo_builtin(int i, UitAmiibo *out);

/* ---- nn::nfp device (one reader, always present) ---- */
enum { UIT_NFP_DS_INIT = 0, UIT_NFP_DS_SEARCHING = 1, UIT_NFP_DS_FOUND = 2, UIT_NFP_DS_REMOVED = 3, UIT_NFP_DS_MOUNTED = 4 };
#define UIT_NFP_HANDLE 0x0000000155495431ull   /* the one DeviceHandle we hand out ("1TIU", 1) */
#define UIT_NFP_RES(desc) ((uint32_t)(((desc) << 9) | 115))
#define UIT_NFP_DEVICE_NOT_FOUND    UIT_NFP_RES(64)
#define UIT_NFP_INVALID_ARGUMENT    UIT_NFP_RES(65)
#define UIT_NFP_AREA_TOO_LARGE      UIT_NFP_RES(68)
#define UIT_NFP_NOT_MOUNTED         UIT_NFP_RES(73)
#define UIT_NFP_NEED_RESTART        UIT_NFP_RES(96)
#define UIT_NFP_INVALID_STATE       UIT_NFP_RES(97)
#define UIT_NFP_AREA_NOT_CREATED    UIT_NFP_RES(128)
#define UIT_NFP_ACCESS_ID_MISMATCH  UIT_NFP_RES(152)
#define UIT_NFP_AREA_EXISTS         UIT_NFP_RES(168)
#define UIT_NFP_AREA_SIZE 0xD8

#define UIT_NFP_PRESENT_MS 2500    /* a scan holds the figure on the reader this long */
#define UIT_NFP_HOLD_MS    20000   /* a scan while the game isn't searching waits this long for it */
#define UIT_NFP_ARRIVE_MS  300     /* a waiting figure "arrives" this long after the game starts searching */
/* [UIT 0208] How long a scanned figure stays on the reader (TOTK_AMIIBO_READER_MS, Game > Amiibo > Time on the reader;
   default UIT_NFP_PRESENT_MS). set clamps to 1000..15000 ms and returns what it set; get is what the reader uses. */
#define UIT_NFP_PRESENT_MIN_MS 1000
#define UIT_NFP_PRESENT_MAX_MS 15000
int uit_nfp_set_present_ms(int ms);
int uit_nfp_present_ms(void);
/* [UIT 0210] Limitless amiibo scanning at the reader: while want() answers 1 (nfp.c: the Limitless amiibo scanning
   cheat), each figure arriving on the reader gets a new random serial number (04 + 6 bytes, as an NTAG215's) in
   GetTagInfo. The game keeps its once-a-day record per serial number, so it has never seen the figure. The amiibo ID
   (ModelInfo) and the application-area file (keyed by the file's own serial) are unchanged. NULL: off. seed: tests. */
void uit_nfp_set_fresh_serial(int (*want)(void), uint64_t seed);
/* [UIT 0211] The serial number the game reads for the figure on the reader (hex, "" when none) */
void uit_nfp_tag_serial(char *buf, size_t n);

/* Signals the glue owes the game after a call (collected under the lock, raised outside it). */
enum { UIT_NFP_SIG_ACTIVATE = 1, UIT_NFP_SIG_DEACTIVATE = 2, UIT_NFP_SIG_AVAILABILITY = 4 };

/* Application-area storage (nfp.c: files under the profile; tests: memory). load returns the size or -1. */
typedef struct {
    int (*load)(const char *key, uint8_t *buf, size_t cap, uint32_t *access_id);
    int (*save)(const char *key, const uint8_t *buf, size_t n, uint32_t access_id);
} UitNfpStore;
void uit_nfp_setup(uint64_t (*now_ms)(void), const UitNfpStore *store);
void uit_nfp_reset(void);   /* everything back to "never initialised" (tests) */

uint32_t uit_nfp_initialize(void);
uint32_t uit_nfp_finalize(void);
int uit_nfp_state(void);    /* nn::nfp::State: 0 NonInitialized, 1 Initialized */
uint32_t uit_nfp_list(uint64_t *handles, int *count, int max);
int uit_nfp_device_state(uint64_t h);
uint32_t uit_nfp_check(uint64_t h);   /* 0 if h is our device and nfp is initialised */
uint32_t uit_nfp_start(uint64_t h);
uint32_t uit_nfp_stop(uint64_t h);
uint32_t uit_nfp_mount(uint64_t h, int model_type, int target);
uint32_t uit_nfp_unmount(uint64_t h);
uint32_t uit_nfp_tag_info(uint64_t h, uint8_t out[0x58]);
uint32_t uit_nfp_model_info(uint64_t h, uint8_t out[0x40]);
uint32_t uit_nfp_common_info(uint64_t h, uint8_t out[0x40]);
uint32_t uit_nfp_register_info(uint64_t h, uint8_t out[0x100]);
uint32_t uit_nfp_open_area(uint64_t h, uint32_t access_id);
uint32_t uit_nfp_get_area(uint64_t h, void *buf, uint64_t *size, uint64_t cap);
uint32_t uit_nfp_set_area(uint64_t h, const void *buf, uint64_t n);
uint32_t uit_nfp_create_area(uint64_t h, uint32_t access_id, const void *buf, uint64_t n);
uint32_t uit_nfp_flush(uint64_t h);

/* Overlay side. present: 1 = the game was searching and sees the figure now, 2 = kept on the reader for
 * UIT_NFP_HOLD_MS until the game searches. */
int uit_nfp_present(const UitAmiibo *a);
void uit_nfp_tick(void);              /* timers: removal after UIT_NFP_PRESENT_MS, expiry of a held figure */
unsigned uit_nfp_take_signals(void);  /* UIT_NFP_SIG_* raised since the last call */
/* What happened to the last scan, for the overlay's status line: 0 nothing yet, 1 waiting for the game, 2 the game
 * saw it, 3 the wait ran out, 4 the game read it (mounted). */
int uit_nfp_scan_result(void);
/* [UIT 0180] Each of the game's calls, its answer and the figure's comings and goings as one line of words (no trailing
 * newline) to sink, called with the state machine's lock held (sink must not call back into uit_nfp_*). NULL: off. */
void uit_nfp_set_log(void (*sink)(const char *line));
const char *uit_nfp_result_text(uint32_t r);   /* "ok", "WrongDeviceState (2115-0097)", ... */
const char *uit_nfp_state_text(int ds);        /* "idle", "searching", "figure found", ... */

#ifdef __cplusplus
}
#endif

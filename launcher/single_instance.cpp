// [UIT 0272] one launcher per folder: single_instance.h
#include "single_instance.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace ut {

uint32_t si_dir_hash(const mini::string &dir) {
    size_t n = dir.size();
    while (n > 1 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) n--;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)dir[i];
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        h = (h ^ c) * 16777619u;
    }
    return h;
}

mini::string si_name(const mini::string &dir, const char *suffix) {
    static const char hex[] = "0123456789abcdef";
    uint32_t h = si_dir_hash(dir);
    mini::string s = "Local\\UltraInstinctTearsLauncher-";
    for (int k = 28; k >= 0; k -= 4) s += hex[h >> k & 15];
    if (suffix) s += suffix;
    return s;
}

#ifdef _WIN32
static HANDLE g_mutex, g_event;

static HANDLE make(const mini::string &name, bool mutex, bool *existed) {
    wchar_t w[128];
    int n = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, w, 128);
    if (n <= 0) return nullptr;
    HANDLE h = mutex ? CreateMutexW(nullptr, FALSE, w) : CreateEventW(nullptr, FALSE, FALSE, w);   // auto-reset event
    if (existed) *existed = h && GetLastError() == ERROR_ALREADY_EXISTS;
    return h;
}

SiResult si_claim(const mini::string &dir, bool relaunch, unsigned wait_ms) {
    if (!g_mutex) {
        bool existed = false;
        g_mutex = make(si_name(dir, nullptr), true, &existed);
        if (!g_mutex) return SI_ALONE;
        (void)existed;   // not enough by itself: a copy that let go (Restart launcher) still has the handle open
    }
    if (!g_event) g_event = make(si_name(dir, "-raise"), false, nullptr);
    DWORD r = WaitForSingleObject(g_mutex, relaunch ? wait_ms : 0);
    if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) return SI_FIRST;   // abandoned: the old copy crashed; ours now
    if (r == WAIT_TIMEOUT && relaunch) return SI_ALONE;   // the old copy did not close in time: run anyway, not hand over to it
    if (r == WAIT_TIMEOUT) {
        // the running copy may take the foreground for its window (we have the right: the user just started us)
        AllowSetForegroundWindow(ASFW_ANY);
        if (g_event) SetEvent(g_event);
        CloseHandle(g_mutex), g_mutex = nullptr;
        if (g_event) CloseHandle(g_event), g_event = nullptr;
        return SI_HANDED_OVER;
    }
    return SI_ALONE;
}

bool si_raise_requested() { return g_event && WaitForSingleObject(g_event, 0) == WAIT_OBJECT_0; }

void si_release() {
    if (g_mutex) ReleaseMutex(g_mutex);   // the handle stays: si_claim waits on it again if Restart launcher failed
}
#else
SiResult si_claim(const mini::string &, bool, unsigned) { return SI_ALONE; }
bool si_raise_requested() { return false; }
void si_release() {}
#endif

}  // namespace ut

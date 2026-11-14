// Ultra Instinct Tears launcher: OS calls. Windows is the real target; elsewhere the game cannot run, so start_game says so
// (the Linux build exists for compile checks and screenshots).
#include "platform.h"

#include <SDL3/SDL.h>
#include <time.h>

#include "version.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ut {

mini::string join(const mini::string &dir, const mini::string &name) {
    if (dir.empty()) return name;
    char last = dir.back();
    if (last == '/' || last == '\\') return dir + name;
#ifdef _WIN32
    return dir + "\\" + name;
#else
    return dir + "/" + name;
#endif
}

bool file_exists(const mini::string &path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
}

mini::string timestamp() {
    time_t t = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tmv);
    return buf;
}

#ifdef _WIN32
static mini::wstring wide(const mini::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    mini::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

static mini::string narrow(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    mini::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

GpuInfo detect_gpu() {
    GpuInfo g;
    DISPLAY_DEVICEW dd;
    for (DWORD i = 0;; i++) {
        ZeroMemory(&dd, sizeof dd);
        dd.cb = sizeof dd;
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) break;
        mini::string name = narrow(dd.DeviceString);
        bool primary = dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE;
        bool nv = name.find("NVIDIA") != mini::string::npos;
        if (g.name.empty() || primary || (nv && !g.nvidia)) {
            g.name = name;
            g.nvidia = nv;
            g.rtx = nv && name.find("RTX") != mini::string::npos;
            if (primary) break;
        }
    }
    return g;
}

bool Game::running() {
    if (!process) return false;
    if (WaitForSingleObject((HANDLE)process, 0) == WAIT_TIMEOUT) return true;
    DWORD code = 0;
    GetExitCodeProcess((HANDLE)process, &code);
    exit_code = (int)code;
    CloseHandle((HANDLE)process);
    process = nullptr;
    return false;
}

// [UIT 0259] Play and Play with log start the game the same way: same flags, same inherited handles, standard output
// and error given in both. Play gives it the NUL device (the game then keeps no log and buffers what it writes),
// Play with log a new file in logs\. Before 0259 Play gave no handles: the game, then a console program, got a console
// window from Windows and wrote its log into it one character at a time, which made Play hitch more than Play with log.
bool start_game(const mini::string &exe, const mini::string &dir, const mini::string &log, Game *game, mini::string *error) {
    mini::wstring cmd = L"\"" + wide(exe) + L"\"";
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE logh;
    if (!log.empty()) {
        logh = CreateFileW(wide(log).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (logh == INVALID_HANDLE_VALUE) { *error = "Could not create " + log; return false; }
    } else {
        logh = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        if (logh == INVALID_HANDLE_VALUE) { *error = "Could not open NUL for the game's output"; return false; }
    }
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = logh;
    si.hStdError = logh;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);
    // [UIT 0294] the game knows the launcher started it: Overlay > Restart game exits with UIT_EXIT_RESTART and
    // poll_game starts it again (src/host/game_exit.h); only the game gets the variable
    SetEnvironmentVariableW(L"UIT_LAUNCHER_RESTART", L"1");
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, wide(dir).c_str(),
                             &si, &pi);
    DWORD err = GetLastError();
    SetEnvironmentVariableW(L"UIT_LAUNCHER_RESTART", nullptr);
    CloseHandle(logh);
    if (!ok) {
        *error = "Windows could not start UltraInstinctTears.exe (error " + mini::to_string(err) + ").";
        return false;
    }
    CloseHandle(pi.hThread);
    game->process = pi.hProcess;
    return true;
}

bool start_process(const mini::string &cmdline, const mini::string &dir, const mini::string &log, Game *proc, mini::string *error) {
    mini::wstring cmd = wide(cmdline);
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE logh = CreateFileW(wide(log).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logh == INVALID_HANDLE_VALUE) { *error = "Could not create " + log; return false; }
    // [UIT 0055] stdin from NUL, not INVALID_HANDLE_VALUE: Python's subprocess duplicates every std handle a child is not
    // given, and an invalid one failed the update's build with "[WinError 6] The handle is invalid".
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = logh;
    si.hStdError = logh;
    si.hStdInput = nul;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                             wide(dir).c_str(), &si, &pi);
    DWORD err = GetLastError();
    CloseHandle(logh);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        *error = "Windows could not run: " + cmdline + " (error " + mini::to_string(err) + "). Is Python installed? "
                 "The Python command is in Advanced > Updates and reports.";
        return false;
    }
    CloseHandle(pi.hThread);
    proc->process = pi.hProcess;
    return true;
}

void stop_process(Game *proc) {   // [UIT 0320]
    if (!proc->process) return;
    TerminateProcess((HANDLE)proc->process, 1);   // asynchronous: never waited for (py.exe ends its python.exe with it)
    CloseHandle((HANDLE)proc->process);
    proc->process = nullptr;
    proc->exit_code = 1;
}

void open_path(const mini::string &path) {
    ShellExecuteW(nullptr, L"open", wide(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// [UIT 0046] Shares read, write and delete: the helpers replace their progress file (write .tmp, rename over) while
// the launcher polls it, and Windows refuses that rename if the file is open without FILE_SHARE_DELETE (SDL_LoadFile
// opens with FILE_SHARE_READ only: update.py failed with "[WinError 5] Access is denied"). Open, read, close at once.
mini::string read_shared(const mini::string &path) {
    for (int attempt = 0; attempt < 3; attempt++) {
        HANDLE h = CreateFileW(wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED) { Sleep(5); continue; }   // mid-rename
            return {};
        }
        mini::string out;
        LARGE_INTEGER size{};
        if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (64 << 20)) {
            out.resize((size_t)size.QuadPart);
            DWORD got = 0;
            if (!ReadFile(h, &out[0], (DWORD)size.QuadPart, &got, nullptr)) got = 0;
            out.resize(got);
        }
        CloseHandle(h);
        return out;
    }
    return {};
}

namespace {
struct Proc { unsigned pid = 0, ppid = 0; unsigned long long cpu_ns = 0; mini::string name; };
}
static unsigned root_pid(Game *g) { return g && g->process ? (unsigned)GetProcessId((HANDLE)g->process) : 0; }
static void list_procs(mini::vector<Proc> &out) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W e;
    e.dwSize = sizeof e;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        Proc p;
        p.pid = e.th32ProcessID, p.ppid = e.th32ParentProcessID;
        char nm[MAX_PATH * 3];
        int n = WideCharToMultiByte(CP_UTF8, 0, e.szExeFile, -1, nm, sizeof nm, nullptr, nullptr);
        p.name = n > 0 ? mini::string(nm) : mini::string("?");
        out.push_back(p);
    }
    CloseHandle(snap);
}
static bool proc_cpu(Proc &p) {   // only asked for the helper's own processes
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid);
    if (!h) return false;
    FILETIME c, x, k, u;
    bool ok = GetProcessTimes(h, &c, &x, &k, &u);
    CloseHandle(h);
    auto u64 = [](FILETIME f) { return ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime; };
    if (ok) p.cpu_ns = (u64(k) + u64(u)) * 100;
    return ok;
}
int cpu_threads() { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
void keep_awake(bool on) { SetThreadExecutionState(on ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS); }
void notify_sound(bool ok) { MessageBeep(ok ? MB_ICONASTERISK : MB_ICONHAND); }

double cpu_busy() {   // [UIT 0431] GetSystemTimes: kernel time includes idle time
    static unsigned long long last_idle = 0, last_total = 0;
    FILETIME fi, fk, fu;
    if (!GetSystemTimes(&fi, &fk, &fu)) return -1;
    auto u64 = [](FILETIME f) { return ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime; };
    unsigned long long idle = u64(fi), total = u64(fk) + u64(fu);
    double r = -1;
    if (last_total && total > last_total) r = 100.0 * (1.0 - (double)(idle - last_idle) / (double)(total - last_total));
    last_idle = idle, last_total = total;
    return r;
}

mini::string read_tail(const mini::string &path, long long max_bytes) {
    HANDLE h = CreateFileW(wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    mini::string out;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0) {
        long long n = size.QuadPart < max_bytes ? size.QuadPart : max_bytes;
        LARGE_INTEGER at;
        at.QuadPart = size.QuadPart - n;
        if (SetFilePointerEx(h, at, nullptr, FILE_BEGIN)) {
            out.resize((size_t)n);
            DWORD got = 0;
            if (!ReadFile(h, &out[0], (DWORD)n, &got, nullptr)) got = 0;
            out.resize(got);
        }
    }
    CloseHandle(h);
    return out;
}

#else   // not Windows

GpuInfo detect_gpu() { return {}; }
bool Game::running() {   // only helper processes run here (start_process); the pid is kept in process
    if (!process) return false;
    int st = 0;
    pid_t pid = (pid_t)(intptr_t)process;
    pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == 0) return true;
    exit_code = r == pid && WIFEXITED(st) ? WEXITSTATUS(st) : 255;
    process = nullptr;
    return false;
}

bool start_process(const mini::string &cmdline, const mini::string &dir, const mini::string &log, Game *proc, mini::string *error) {
    int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { *error = "Could not create " + log; return false; }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fd, 1), dup2(fd, 2);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, 0);
        if (chdir(dir.c_str()) != 0) _exit(127);
        execl("/bin/sh", "sh", "-c", cmdline.c_str(), (char *)nullptr);
        _exit(127);
    }
    close(fd);
    if (pid < 0) { *error = "Could not run: " + cmdline; return false; }
    proc->process = (void *)(intptr_t)pid;
    return true;
}

void stop_process(Game *proc) {   // [UIT 0320]
    if (!proc->process) return;
    pid_t pid = (pid_t)(intptr_t)proc->process;
    kill(pid, SIGKILL);
    int st = 0;
    waitpid(pid, &st, 0);
    proc->process = nullptr;
    proc->exit_code = 1;
}

bool start_game(const mini::string &, const mini::string &, const mini::string &, Game *, mini::string *error) {
    *error = "Ultra Instinct Tears runs on Windows. This build of the launcher can only edit settings.";
    return false;
}

void open_path(const mini::string &path) {
    mini::string url = "file://" + path;
    SDL_OpenURL(url.c_str());
}

namespace {
struct Proc { unsigned pid = 0, ppid = 0; unsigned long long cpu_ns = 0; mini::string name; };
}
static unsigned root_pid(Game *g) { return g && g->process ? (unsigned)(intptr_t)g->process : 0; }
static bool read_stat(unsigned pid, Proc &p) {   // /proc/PID/stat: "pid (comm) state ppid ... utime stime" (fields 14, 15)
    char path[64], t[1024];
    SDL_snprintf(path, sizeof path, "/proc/%u/stat", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, t, sizeof t - 1);
    close(fd);
    if (n <= 0) return false;
    t[n] = 0;
    char *l = SDL_strchr(t, '('), *r = SDL_strrchr(t, ')');
    if (!l || !r) return false;
    p.pid = pid, p.name = mini::string(l + 1, (size_t)(r - l - 1));
    unsigned long long ut = 0, st = 0;
    int ppid = 0;
    char state = 0;
    if (SDL_sscanf(r + 2, "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &state, &ppid, &ut, &st) != 4) return false;
    long hz = sysconf(_SC_CLK_TCK);
    p.ppid = (unsigned)ppid, p.cpu_ns = (ut + st) * (1000000000ull / (unsigned long long)(hz > 0 ? hz : 100));
    return true;
}
static void list_procs(mini::vector<Proc> &out) {
    DIR *d = opendir("/proc");
    if (!d) return;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        Proc p;
        if (read_stat((unsigned)SDL_atoi(e->d_name), p)) out.push_back(p);
    }
    closedir(d);
}
static bool proc_cpu(Proc &) { return true; }   // read with the list
int cpu_threads() { return SDL_GetNumLogicalCPUCores(); }
void keep_awake(bool) {}
void notify_sound(bool) {}

double cpu_busy() {   // [UIT 0431] /proc/stat's first line (Linux); elsewhere unknown
    static unsigned long long last_idle = 0, last_total = 0;
    char t[512];
    int fd = open("/proc/stat", O_RDONLY);   // a size-0 file: read, not SDL_LoadFile
    if (fd < 0) return -1;
    ssize_t r0 = read(fd, t, sizeof t - 1);
    close(fd);
    if (r0 <= 0) return -1;
    t[r0] = 0;
    unsigned long long v[8] = {0};
    int got = SDL_sscanf(t, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    if (got < 4) return -1;
    unsigned long long idle = v[3] + v[4], total = 0;
    for (int k = 0; k < 8; k++) total += v[k];
    double r = -1;
    if (last_total && total > last_total) r = 100.0 * (1.0 - (double)(idle - last_idle) / (double)(total - last_total));
    last_idle = idle, last_total = total;
    return r;
}

mini::string read_tail(const mini::string &path, long long max_bytes) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return {};
    off_t size = lseek(fd, 0, SEEK_END);
    mini::string out;
    if (size > 0) {
        off_t n = size < max_bytes ? size : (off_t)max_bytes;
        if (lseek(fd, size - n, SEEK_SET) >= 0) {
            out.resize((size_t)n);
            ssize_t r = read(fd, &out[0], (size_t)n);
            out.resize(r > 0 ? (size_t)r : 0);
        }
    }
    close(fd);
    return out;
}

mini::string read_shared(const mini::string &path) {   // POSIX: a rename over an open file is always allowed
    size_t n = 0;
    void *data = SDL_LoadFile(path.c_str(), &n);
    if (!data) return {};
    mini::string t((const char *)data, n);
    SDL_free(data);
    return t;
}
#endif


// ---------------------------------------------------------------- [UIT 0431] a helper's process tree
TreeCpu tree_cpu(Game *root) {
    static mini::map<unsigned, unsigned long long> last;   // pid -> CPU time at the last call
    static Uint64 last_t = 0;
    TreeCpu r;
    unsigned top = root_pid(root);
    if (!top) { last.clear(); last_t = 0; return r; }
    mini::vector<Proc> all;
    list_procs(all);
    mini::vector<Proc> tree;
    for (auto &p : all) if (p.pid == top) { tree.push_back(p); break; }
    for (size_t w = 0; w < tree.size() && tree.size() < 4096; w++)   // breadth first: the helper, its children, theirs
        for (auto &p : all) {
            if (p.ppid != tree[w].pid || p.pid == tree[w].pid) continue;
            bool seen = false;
            for (auto &t : tree) if (t.pid == p.pid) { seen = true; break; }
            if (!seen) tree.push_back(p);
        }
    Uint64 now = SDL_GetTicksNS();
    double wall = last_t ? (double)(now - last_t) : 0;
    mini::map<unsigned, unsigned long long> next;
    unsigned long long busy = 0;
    struct Name { mini::string n; int count = 0; unsigned long long ns = 0; };
    mini::vector<Name> names;
    for (auto &p : tree) {
        if (!proc_cpu(p)) continue;
        next[p.pid] = p.cpu_ns;
        auto it = last.find(p.pid);
        unsigned long long d = it != last.end() ? (p.cpu_ns > it->second ? p.cpu_ns - it->second : 0) : (last_t ? p.cpu_ns : 0);
        if (last_t && it == last.end() && wall > 0 && (double)d > wall * 64) d = 0;   // an old process new to the list: not this second's
        busy += d;
        if (d * 50 < (unsigned long long)wall) continue;   // under 2 % of a thread: not named
        bool found = false;
        for (auto &n : names) if (n.n == p.name) { n.count++, n.ns += d; found = true; }
        if (!found) { Name n; n.n = p.name, n.count = 1, n.ns = d; names.push_back(n); }
    }
    r.procs = (int)tree.size();
    if (wall > 0) r.cores = (double)busy / wall;
    for (size_t a = 0; a < names.size(); a++)   // busiest first (a handful of names: a plain selection sort)
        for (size_t b = a + 1; b < names.size(); b++)
            if (names[b].ns > names[a].ns) { Name t = names[a]; names[a] = names[b]; names[b] = t; }
    for (size_t k = 0; k < names.size() && k < 3; k++)
        r.names += (k ? ", " : "") + names[k].n + (names[k].count > 1 ? " x" + mini::to_string(names[k].count) : mini::string());
    last = next, last_t = now;
    return r;
}

// ---------------------------------------------------------------- crash report
// launcher-crash.txt next to the launcher (or in %TEMP% if that folder is read-only): the exception, the faulting
// address and a backtrace as module+offset, like the game's own crash lines. Offsets in UltraInstinctTearsLauncher.exe map to
// source with llvm-addr2line -f -C -e UltraInstinctTearsLauncher.exe 0x140000000+offset (mingw: DWARF, no PDB).
#ifdef _WIN32
static wchar_t g_crash_path[MAX_PATH];
static wchar_t g_crash_tmp[MAX_PATH];

static void crash_frame(char *out, size_t cap, int n, DWORD64 pc) {
    HMODULE m = nullptr;
    char mod[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)pc, &m) && m) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(m, path, sizeof path)) {
            const char *b = strrchr(path, '\\');
            snprintf(mod, sizeof mod, "%s", b ? b + 1 : path);
        }
    }
    size_t l = strlen(out);
    if (l + 1 < cap)
        snprintf(out + l, cap - l, "  #%-2d %016llx %s+%llx\r\n", n, (unsigned long long)pc, mod,
                 (unsigned long long)(m ? pc - (DWORD64)(uintptr_t)m : pc));
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    static char buf[16384];
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    SYSTEMTIME st;
    GetLocalTime(&st);
    const char *what = er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? "access violation"
                       : er->ExceptionCode == EXCEPTION_STACK_OVERFLOW ? "stack overflow"
                       : er->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION ? "illegal instruction"
                       : er->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO ? "divide by zero" : "exception";
    snprintf(buf, sizeof buf,
             "Ultra Instinct Tears Launcher %s (%s) crashed %04u-%02u-%02u %02u:%02u:%02u (local time)\r\n"
             "%s %08lx at %016llx", ULTRS_VERSION, ULTRS_COMMIT, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, what, er->ExceptionCode, (unsigned long long)(uintptr_t)er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        size_t l = strlen(buf);
        snprintf(buf + l, sizeof buf - l, ", %s %#llx",
                 er->ExceptionInformation[0] == 1 ? "writing" : er->ExceptionInformation[0] == 8 ? "executing" : "reading",
                 (unsigned long long)er->ExceptionInformation[1]);
    }
    strncat(buf, "\r\nbacktrace (module+offset; repeated frames folded):\r\n", sizeof buf - strlen(buf) - 1);
    // walk the faulting thread's stack from the exception context with the x64 unwind tables (no dbghelp needed)
    CONTEXT c = *ep->ContextRecord;
    DWORD64 last = 0;
    int repeats = 0, shown = 0;
    for (int n = 0; n < 4096 && c.Rip && shown < 48; n++) {
        if (c.Rip == last) {
            repeats++;
        } else {
            if (repeats) {
                size_t l = strlen(buf);
                snprintf(buf + l, sizeof buf - l, "      (the frame above %d more times)\r\n", repeats);
            }
            repeats = 0;
            crash_frame(buf, sizeof buf, n, c.Rip);
            shown++;
            last = c.Rip;
        }
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
        if (!fn) {                                   // a leaf function: the return address is on top of the stack
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery((void *)(uintptr_t)c.Rsp, &mi, sizeof mi) || mi.State != MEM_COMMIT) break;
            c.Rip = *(DWORD64 *)(uintptr_t)c.Rsp;
            c.Rsp += 8;
            continue;
        }
        void *handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &handler_data, &frame, nullptr);
    }
    if (repeats) {
        size_t l = strlen(buf);
        snprintf(buf + l, sizeof buf - l, "      (the frame above %d more times)\r\n", repeats);
    }
    strncat(buf, "Please send this file with the report.\r\n", sizeof buf - strlen(buf) - 1);
    for (const wchar_t *p : {g_crash_path, g_crash_tmp}) {
        if (!p[0]) continue;
        HANDLE h = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD wrote = 0;
        WriteFile(h, buf, (DWORD)strlen(buf), &wrote, nullptr);
        CloseHandle(h);
        break;
    }
    return EXCEPTION_EXECUTE_HANDLER;               // end the process quietly (no "stopped working" dialog)
}

void install_crash_handler() {
    ULONG guarantee = 64 << 10;                      // room for the report after a stack overflow
    SetThreadStackGuarantee(&guarantee);
    DWORD n = GetModuleFileNameW(nullptr, g_crash_path, MAX_PATH);
    if (n && n < MAX_PATH) {
        wchar_t *b = wcsrchr(g_crash_path, L'\\');
        if (b && (size_t)(b + 1 - g_crash_path) + 20 < MAX_PATH) wcscpy(b + 1, L"launcher-crash.txt");
        else g_crash_path[0] = 0;
    } else {
        g_crash_path[0] = 0;
    }
    DWORD t = GetTempPathW(MAX_PATH - 24, g_crash_tmp);
    if (t && t < MAX_PATH - 24) wcscat(g_crash_tmp, L"launcher-crash.txt");
    else g_crash_tmp[0] = 0;
    SetUnhandledExceptionFilter(crash_filter);
}
#else
static char g_crash_path[4096];

static void crash_signal(int sig) {
    char head[256];
    int fd = g_crash_path[0] ? open(g_crash_path, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
    if (fd >= 0) {
        int l = snprintf(head, sizeof head, "Ultra Instinct Tears Launcher %s (%s) crashed: signal %d\nbacktrace:\n", ULTRS_VERSION,
                         ULTRS_COMMIT, sig);
        if (write(fd, head, (size_t)l) < 0) {}
        void *bt[64];
        backtrace_symbols_fd(bt, backtrace(bt, 64), fd);
        close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void install_crash_handler() {
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
    return;                                          // the sanitizer's own report is better (test builds)
#endif
#endif
    char exe[4000];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n] = 0;
    char *b = strrchr(exe, '/');
    if (b) *b = 0;
    snprintf(g_crash_path, sizeof g_crash_path, "%s/launcher-crash.txt", exe);
    static char alt[64 << 10];
    stack_t ss = {};
    ss.ss_sp = alt, ss.ss_size = sizeof alt;
    sigaltstack(&ss, nullptr);
    struct sigaction sa = {};
    sa.sa_handler = crash_signal;
    sa.sa_flags = SA_ONSTACK;
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(s, &sa, nullptr);
}
#endif

}  // namespace ut

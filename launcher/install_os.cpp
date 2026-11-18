// Ultra Instinct Tears setup: OS calls (see install_os.h). [UIT 0433]
#include "install_os.h"

#include <SDL3/SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <signal.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ut {

#ifdef _WIN32
static mini::wstring wide(const mini::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    mini::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
#endif

bool is_dir(const mini::string &path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

static mini::string parent_dir(const mini::string &p) {
    size_t a = p.rfind('/'), b = p.rfind('\\');
    size_t cut = a == mini::string::npos ? b : b == mini::string::npos ? a : mini::max(a, b);
    if (cut == mini::string::npos || cut == 0) return {};
    mini::string up = p.substr(0, cut);
    if (up.size() == 2 && up[1] == ':') up += '\\';
    return up;
}

namespace {
struct Ctx { mini::string root; mini::string err; bool ok = true; };
SDL_EnumerationResult SDLCALL rm_cb(void *ud, const char *dirname, const char *fname) {
    Ctx *c = (Ctx *)ud;
    mini::string p = mini::string(dirname) + fname;
    SDL_PathInfo info;
    if (SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY) {
        Ctx sub;
        if (!SDL_EnumerateDirectory(p.c_str(), rm_cb, &sub)) sub.ok = false;
        if (!sub.ok && c->ok) c->ok = false, c->err = sub.err;
    }
    if (!SDL_RemovePath(p.c_str())) {
#ifdef _WIN32
        SetFileAttributesW(wide(p).c_str(), FILE_ATTRIBUTE_NORMAL);   // read-only files
#endif
        if (!SDL_RemovePath(p.c_str()) && c->ok) c->ok = false, c->err = p + ": " + SDL_GetError();
    }
    return SDL_ENUM_CONTINUE;
}
}  // namespace

bool remove_tree(const mini::string &path, mini::string *error) {
    if (!is_dir(path)) {
        if (SDL_GetPathInfo(path.c_str(), nullptr) && !SDL_RemovePath(path.c_str())) { *error = SDL_GetError(); return false; }
        return true;
    }
    Ctx c;
    SDL_EnumerateDirectory(path.c_str(), rm_cb, &c);
    if (!SDL_RemovePath(path.c_str()) && c.ok) c.ok = false, c.err = path + ": " + SDL_GetError();
    if (!c.ok) *error = c.err;
    return c.ok;
}

#ifdef _WIN32
static mini::string narrow(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    mini::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

mini::string self_exe() {
    wchar_t b[32768];
    DWORD n = GetModuleFileNameW(nullptr, b, 32768);
    return n ? narrow(b) : mini::string();
}

int64_t free_bytes(const mini::string &path) {
    if (const char *f = SDL_getenv("UIT_SETUP_TEST_FREE_GB")) return (int64_t)(SDL_atof(f) * 1073741824.0);   // the tests
    mini::string p = path;
    while (!p.empty()) {
        ULARGE_INTEGER avail;
        if (is_dir(p) && GetDiskFreeSpaceExW(wide(p).c_str(), &avail, nullptr, nullptr)) return (int64_t)avail.QuadPart;
        p = parent_dir(p);
    }
    return -1;
}

mini::vector<mini::string> fixed_drives() {
    mini::vector<mini::string> out;
    DWORD m = GetLogicalDrives();
    for (int k = 0; k < 26; k++) {
        if (!(m & (1u << k))) continue;
        wchar_t root[4] = {(wchar_t)(L'A' + k), L':', L'\\', 0};
        if (GetDriveTypeW(root) == DRIVE_FIXED) out.push_back(narrow(root));
    }
    return out;
}

// The builder package's trimmed MinGW headers have no shlobj.h, shobjidl.h or winhttp.h (the launcher is also built on
// the player's PC with them): the few calls needed are looked up in shell32.dll / winhttp.dll at run time, and the
// shell link interfaces are declared here (their layout is fixed by COM).
static FARPROC sym(const char *dll, const char *name) {
    HMODULE m = LoadLibraryA(dll);
    return m ? GetProcAddress(m, name) : nullptr;
}
static mini::string csidl(int id) {   // SHGetFolderPathW: the desktop, the Start menu's Programs, the profile
    typedef HRESULT(WINAPI * Fn)(HWND, int, HANDLE, DWORD, LPWSTR);
    Fn f = (Fn)(void *)sym("shell32.dll", "SHGetFolderPathW");
    wchar_t b[MAX_PATH + 1] = {0};
    if (!f || FAILED(f(nullptr, id, nullptr, 0, b))) return {};
    return narrow(b);
}
mini::string home_folder() { return csidl(0x0028 /* CSIDL_PROFILE */); }
mini::string desktop_folder() {
    if (const char *d = SDL_getenv("UIT_SETUP_TEST_DESKTOP")) return d;   // the tests
    return csidl(0x0010 /* CSIDL_DESKTOPDIRECTORY */);
}
mini::string start_menu_folder() {
    if (const char *d = SDL_getenv("UIT_SETUP_TEST_START")) return d;   // the tests
    return csidl(0x0002 /* CSIDL_PROGRAMS */);
}

namespace {
typedef void *HNET;
struct WinHttp {
    HNET(WINAPI *Open)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
    HNET(WINAPI *Connect)(HNET, LPCWSTR, WORD, DWORD);
    HNET(WINAPI *OpenRequest)(HNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
    BOOL(WINAPI *SendRequest)(HNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
    BOOL(WINAPI *ReceiveResponse)(HNET, LPVOID);
    BOOL(WINAPI *QueryHeaders)(HNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
    BOOL(WINAPI *ReadData)(HNET, LPVOID, DWORD, LPDWORD);
    BOOL(WINAPI *SetTimeouts)(HNET, int, int, int, int);
    BOOL(WINAPI *SetOption)(HNET, DWORD, LPVOID, DWORD);
    BOOL(WINAPI *Close)(HNET);
    bool ok = false;
};
const WinHttp &winhttp() {
    static WinHttp w;
    static bool tried = false;
    if (tried) return w;
    tried = true;
    HMODULE m = LoadLibraryA("winhttp.dll");
    if (!m) return w;
#define UIT_SYM(field, name) *(FARPROC *)&w.field = GetProcAddress(m, name)
    UIT_SYM(Open, "WinHttpOpen"); UIT_SYM(Connect, "WinHttpConnect"); UIT_SYM(OpenRequest, "WinHttpOpenRequest");
    UIT_SYM(SendRequest, "WinHttpSendRequest"); UIT_SYM(ReceiveResponse, "WinHttpReceiveResponse");
    UIT_SYM(QueryHeaders, "WinHttpQueryHeaders"); UIT_SYM(ReadData, "WinHttpReadData"); UIT_SYM(SetTimeouts, "WinHttpSetTimeouts");
    UIT_SYM(SetOption, "WinHttpSetOption"); UIT_SYM(Close, "WinHttpCloseHandle");
#undef UIT_SYM
    w.ok = w.Open && w.Connect && w.OpenRequest && w.SendRequest && w.ReceiveResponse && w.QueryHeaders && w.ReadData && w.SetTimeouts &&
           w.SetOption && w.Close;
    return w;
}
// the winhttp.h values used
const DWORD kAutoProxy = 4, kDefaultProxy = 0, kRedirectPolicy = 88, kRedirectNoHttpsToHttp = 1, kFlagSecure = 0x00800000,
            kQueryStatus = 19, kQueryLength = 5, kQueryNumber = 0x20000000;
struct Url { mini::wstring host, path; WORD port = 443; bool secure = true; };
bool split_url(const mini::string &url, Url *u) {
    // [UIT 0435] the tests' fake GitHub: http://127.0.0.1:PORT, only when UIT_SETUP_TEST_HTTP is set
    bool plain = SDL_getenv("UIT_SETUP_TEST_HTTP") && url.substr(0, 17) == "http://127.0.0.1:";
    if (!plain && url.substr(0, 8) != "https://") return false;
    size_t from = plain ? 7 : 8;
    size_t slash = url.find('/', from);
    mini::string host = url.substr(from, slash == mini::string::npos ? mini::string::npos : slash - from);
    if (plain) {
        size_t colon = host.find(':');
        u->port = (WORD)SDL_atoi(host.c_str() + colon + 1);
        u->secure = false;
        host = host.substr(0, colon);
    }
    u->host = wide(host);
    u->path = wide(slash == mini::string::npos ? mini::string("/") : url.substr(slash));
    return !host.empty();
}
struct Net {
    HNET s = nullptr, c = nullptr, r = nullptr;
    ~Net() {
        const WinHttp &w = winhttp();
        if (r) w.Close(r);
        if (c) w.Close(c);
        if (s) w.Close(s);
    }
};
// Opens the request and waits for the answer's headers; WinHTTP follows redirects (GitHub's downloads go to another host).
bool open_get(Net &n, const mini::string &url, mini::string *err, int64_t *total) {
    const WinHttp &w = winhttp();
    if (!w.ok) { *err = "Windows' internet functions (winhttp.dll) are missing"; return false; }
    Url u;
    if (!split_url(url, &u)) { *err = "not an https address: " + url; return false; }
    n.s = w.Open(L"UltraInstinctTears-setup", kAutoProxy, nullptr, nullptr, 0);
    if (!n.s) n.s = w.Open(L"UltraInstinctTears-setup", kDefaultProxy, nullptr, nullptr, 0);
    if (!n.s) { *err = "the internet connection could not be opened (error " + mini::to_string((unsigned long)GetLastError()) + ")"; return false; }
    w.SetTimeouts(n.s, 30000, 30000, 30000, 60000);
    DWORD redir = kRedirectNoHttpsToHttp;
    w.SetOption(n.s, kRedirectPolicy, &redir, sizeof redir);
    n.c = w.Connect(n.s, u.host.c_str(), u.port, 0);
    if (n.c) n.r = w.OpenRequest(n.c, L"GET", u.path.c_str(), nullptr, nullptr, nullptr, u.secure ? kFlagSecure : 0);
    if (!n.r || !w.SendRequest(n.r, nullptr, 0, nullptr, 0, 0, 0) || !w.ReceiveResponse(n.r, nullptr)) {
        *err = "could not reach " + narrow(u.host.c_str()) + " (error " + mini::to_string((unsigned long)GetLastError()) + ")";
        return false;
    }
    DWORD code = 0, sz = sizeof code;
    w.QueryHeaders(n.r, kQueryStatus | kQueryNumber, nullptr, &code, &sz, nullptr);
    if (code != 200) { *err = "the server answered " + mini::to_string((unsigned long)code) + " for " + url; return false; }
    wchar_t len[32];
    DWORD lsz = sizeof len;
    *total = 0;
    if (w.QueryHeaders(n.r, kQueryLength, nullptr, len, &lsz, nullptr)) *total = _wtoi64(len);
    return true;
}
}  // namespace

bool http_get(const mini::string &url, const mini::string &path, volatile int *stop,
              void (*progress)(void *, int64_t, int64_t), void *ud, mini::string *error) {
    Net n;
    int64_t total = 0, done = 0;
    if (!open_get(n, url, error, &total)) return false;
    const WinHttp &w = winhttp();
    SDL_IOStream *out = SDL_IOFromFile(path.c_str(), "wb");
    if (!out) { *error = "cannot write " + path + ": " + SDL_GetError(); return false; }
    mini::string buf;
    buf.resize(1 << 15);   // [UIT 0435] small reads: Stop and the MB count answer within a moment, whatever WinHTTP does with a big buffer
    bool ok = true;
    for (;;) {
        if (stop && *stop) { *error = "stopped"; ok = false; break; }
        DWORD got = 0;
        if (!w.ReadData(n.r, &buf[0], (DWORD)buf.size(), &got)) {
            *error = "the download stopped (error " + mini::to_string((unsigned long)GetLastError()) + ")"; ok = false; break;
        }
        if (!got) break;
        if (SDL_WriteIO(out, buf.data(), got) != got) { *error = "cannot write " + path + " (is the disk full?)"; ok = false; break; }
        done += got;
        if (progress) progress(ud, done, total);
    }
    if (!SDL_CloseIO(out) && ok) { *error = "cannot write " + path; ok = false; }
    if (ok && total && done != total) { *error = "the download was cut short"; ok = false; }
    return ok;
}

bool http_get_text(const mini::string &url, mini::string *out, mini::string *error) {
    Net n;
    int64_t total = 0;
    if (!open_get(n, url, error, &total)) return false;
    const WinHttp &w = winhttp();
    out->clear();
    char buf[16384];
    for (;;) {
        DWORD got = 0;
        if (!w.ReadData(n.r, buf, sizeof buf, &got)) { *error = "the answer was cut short"; return false; }
        if (!got) break;
        out->append(buf, got);
        if (out->size() > (16u << 20)) { *error = "the answer is too large"; return false; }
    }
    return true;
}

namespace {
// IShellLinkW and IPersistFile as plain vtables (only the slots used are named)
struct ShellLinkVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(void *);
    ULONG(STDMETHODCALLTYPE *Release)(void *);
    void *GetPath, *GetIDList, *SetIDList, *GetDescription;
    HRESULT(STDMETHODCALLTYPE *SetDescription)(void *, LPCWSTR);
    void *GetWorkingDirectory;
    HRESULT(STDMETHODCALLTYPE *SetWorkingDirectory)(void *, LPCWSTR);
    void *GetArguments, *SetArguments, *GetHotkey, *SetHotkey, *GetShowCmd, *SetShowCmd, *GetIconLocation;
    HRESULT(STDMETHODCALLTYPE *SetIconLocation)(void *, LPCWSTR, int);
    void *SetRelativePath, *Resolve;
    HRESULT(STDMETHODCALLTYPE *SetPath)(void *, LPCWSTR);
};
struct PersistFileVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(void *);
    ULONG(STDMETHODCALLTYPE *Release)(void *);
    void *GetClassID, *IsDirty, *Load;
    HRESULT(STDMETHODCALLTYPE *Save)(void *, LPCOLESTR, BOOL);
};
struct ComObj { void *vtbl; };
const GUID kClsidShellLink = {0x00021401, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const GUID kIidShellLinkW = {0x000214F9, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const GUID kIidPersistFile = {0x0000010b, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
}  // namespace

bool make_shortcut(const mini::string &link, const mini::string &target, const mini::string &workdir,
                   const mini::string &description, mini::string *error) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ComObj *sl = nullptr;
    HRESULT hr = CoCreateInstance(kClsidShellLink, nullptr, CLSCTX_INPROC_SERVER, kIidShellLinkW, (void **)&sl);
    if (SUCCEEDED(hr) && sl) {
        ShellLinkVtbl *v = (ShellLinkVtbl *)sl->vtbl;
        v->SetPath(sl, wide(target).c_str());
        v->SetWorkingDirectory(sl, wide(workdir).c_str());
        v->SetDescription(sl, wide(description).c_str());
        v->SetIconLocation(sl, wide(target).c_str(), 0);
        ComObj *pf = nullptr;
        hr = v->QueryInterface(sl, kIidPersistFile, (void **)&pf);
        if (SUCCEEDED(hr) && pf) {
            PersistFileVtbl *pv = (PersistFileVtbl *)pf->vtbl;
            hr = pv->Save(pf, wide(link).c_str(), TRUE);
            pv->Release(pf);
        }
        v->Release(sl);
    }
    if (SUCCEEDED(init)) CoUninitialize();
    if (FAILED(hr)) { *error = "Windows could not make the shortcut (error " + mini::to_string((unsigned long)hr) + ")"; return false; }
    return true;
}

void stop_tree(Game *proc) {
    if (!proc->process) return;
    DWORD root = GetProcessId((HANDLE)proc->process);
    // the whole tree first (parents can end before their children are found), then end them, children last
    mini::vector<DWORD> pids, parents, tree;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W e;
        e.dwSize = sizeof e;
        for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) pids.push_back(e.th32ProcessID), parents.push_back(e.th32ParentProcessID);
        CloseHandle(snap);
    }
    tree.push_back(root);
    for (size_t k = 0; k < tree.size(); k++)
        for (size_t i = 0; i < pids.size(); i++)
            if (parents[i] == tree[k] && pids[i] != root && mini::find(tree.begin(), tree.end(), pids[i]) == tree.end()) tree.push_back(pids[i]);
    for (size_t k = 0; k < tree.size(); k++) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, tree[k]);
        if (!h) continue;
        TerminateProcess(h, 1);
        WaitForSingleObject(h, 3000);
        CloseHandle(h);
    }
    WaitForSingleObject((HANDLE)proc->process, 3000);
    CloseHandle((HANDLE)proc->process);
    proc->process = nullptr;
    proc->exit_code = 1;
}

bool start_detached(const mini::string &exe, const mini::string &args, const mini::string &dir, mini::string *error) {
    if (const char *rec = SDL_getenv("UIT_SETUP_TEST_STARTED")) {   // the tests: what would have started
        mini::string t = exe + " " + args + "\n";
        SDL_SaveFile(rec, t.data(), t.size());
        return true;
    }
    mini::wstring cmd = L"\"" + wide(exe) + L"\"" + (args.empty() ? mini::wstring() : L" " + wide(args));
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, wide(dir).c_str(), &si, &pi)) {
        *error = "Windows could not start " + exe + " (error " + mini::to_string((unsigned long)GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

#else   // Linux: the tests

mini::string self_exe() {
    char b[4096];
    ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
    if (n <= 0) return {};
    b[n] = 0;
    return b;
}

int64_t free_bytes(const mini::string &path) {
    if (const char *f = SDL_getenv("UIT_SETUP_TEST_FREE_GB")) return (int64_t)(SDL_atof(f) * 1073741824.0);   // the tests
    mini::string p = path;
    while (!p.empty()) {
        struct statvfs s;
        if (is_dir(p) && statvfs(p.c_str(), &s) == 0) return (int64_t)s.f_bavail * (int64_t)s.f_frsize;
        p = parent_dir(p);
    }
    return -1;
}

mini::vector<mini::string> fixed_drives() { return {home_folder()}; }
mini::string home_folder() { const char *h = SDL_getenv("HOME"); return h ? h : "/tmp"; }
mini::string desktop_folder() { const char *d = SDL_getenv("UIT_SETUP_TEST_DESKTOP"); return d ? mini::string(d) : join(home_folder(), "Desktop"); }
mini::string start_menu_folder() { const char *d = SDL_getenv("UIT_SETUP_TEST_START"); return d ? mini::string(d) : join(home_folder(), ".local/share/applications"); }

static bool plain_http_ok(const mini::string &url) {   // [UIT 0435] the tests' fake GitHub on this machine
    return SDL_getenv("UIT_SETUP_TEST_HTTP") && url.substr(0, 17) == "http://127.0.0.1:";
}
// curl, polled: *stop ends it at once, progress sees the file grow ([UIT 0435]; the Windows build does the same with WinHTTP)
static bool run_curl(const mini::string &url, const mini::string &path, volatile int *stop, void (*progress)(void *, int64_t, int64_t), void *ud,
                     mini::string *error) {
    const char *proto = plain_http_ok(url) ? "=http,https" : "=https";
    pid_t pid = fork();
    if (pid == 0) {
        execlp("curl", "curl", "-fsSL", "--proto", proto, "-A", "UltraInstinctTears-setup", "-o", path.c_str(), url.c_str(), (char *)nullptr);
        _exit(127);
    }
    if (pid < 0) { *error = "could not start curl"; return false; }
    int st = 0;
    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) break;
        if (r < 0) { *error = "could not download " + url; return false; }
        if (stop && *stop) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            *error = "stopped";
            return false;
        }
        SDL_PathInfo info;
        if (progress && SDL_GetPathInfo(path.c_str(), &info)) progress(ud, (int64_t)info.size, 0);
        SDL_Delay(30);
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { *error = "could not download " + url; return false; }
    return true;
}
bool http_get(const mini::string &url, const mini::string &path, volatile int *stop, void (*progress)(void *, int64_t, int64_t), void *ud,
              mini::string *error) {
    if (url.substr(0, 8) != "https://" && !plain_http_ok(url)) { *error = "not an https address: " + url; return false; }
    if (!run_curl(url, path, stop, progress, ud, error)) return false;
    SDL_PathInfo info;
    if (progress && SDL_GetPathInfo(path.c_str(), &info)) progress(ud, (int64_t)info.size, (int64_t)info.size);
    return true;
}
bool http_get_text(const mini::string &url, mini::string *out, mini::string *error) {
    mini::string tmp = "/tmp/uit-setup-" + mini::to_string((long)getpid()) + ".txt";
    if (!run_curl(url, tmp, nullptr, nullptr, nullptr, error)) { SDL_RemovePath(tmp.c_str()); return false; }
    *out = read_shared(tmp);
    SDL_RemovePath(tmp.c_str());
    return true;
}

bool make_shortcut(const mini::string &link, const mini::string &target, const mini::string &workdir, const mini::string &description,
                   mini::string *error) {
    mini::string t = "[Desktop Entry]\nType=Application\nName=" + description + "\nExec=\"" + target + "\"\nPath=" + workdir + "\n";
    if (!SDL_SaveFile(link.c_str(), t.data(), t.size())) { *error = SDL_GetError(); return false; }
    return true;
}

static void kids(pid_t root, mini::vector<pid_t> &tree) {
    DIR *d = opendir("/proc");
    if (!d) return;
    mini::vector<pid_t> all, par;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[64], t[512];
        SDL_snprintf(p, sizeof p, "/proc/%s/stat", e->d_name);
        SDL_IOStream *io = SDL_IOFromFile(p, "rb");
        if (!io) continue;
        size_t n = SDL_ReadIO(io, t, sizeof t - 1);
        SDL_CloseIO(io);
        t[n] = 0;
        char *r = SDL_strrchr(t, ')');
        int pp = 0;
        if (r && SDL_sscanf(r + 2, "%*c %d", &pp) == 1) all.push_back((pid_t)SDL_atoi(e->d_name)), par.push_back((pid_t)pp);
    }
    closedir(d);
    tree.push_back(root);
    for (size_t k = 0; k < tree.size(); k++)
        for (size_t i = 0; i < all.size(); i++)
            if (par[i] == tree[k]) tree.push_back(all[i]);
}
void stop_tree(Game *proc) {
    if (!proc->process) return;
    pid_t root = (pid_t)(intptr_t)proc->process;
    mini::vector<pid_t> tree;
    kids(root, tree);
    for (pid_t p : tree) kill(p, SIGKILL);
    int st = 0;
    waitpid(root, &st, 0);
    proc->process = nullptr;
    proc->exit_code = 1;
}

bool start_detached(const mini::string &exe, const mini::string &args, const mini::string &dir, mini::string *error) {
    if (const char *rec = SDL_getenv("UIT_SETUP_TEST_STARTED")) {   // the tests: what would have started
        mini::string t = exe + " " + args + "\n";
        SDL_SaveFile(rec, t.data(), t.size());
        return true;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (chdir(dir.c_str()) != 0) _exit(127);
        mini::string cmd = "\"" + exe + "\" " + args;
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char *)nullptr);
        _exit(127);
    }
    if (pid < 0) { *error = "could not start " + exe; return false; }
    return true;
}
#endif

}  // namespace ut

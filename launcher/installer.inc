    // ================================================================ [UIT 0433] the first-time installer
    // UltraInstinctTears-Setup.exe is this launcher with the source package appended (tools/build_installer.py). Started
    // that way (or with --install) it shows the installer instead of the settings: Welcome -> Install folder -> Game files
    // -> Ready -> the install (the update screen: step list, stage line, bar, elapsed time, counts, CPU, Show details) ->
    // Done (Play, Open launcher, a desktop shortcut). Same window, colours, fonts and keyboard / controller handling as
    // the launcher, plain words only, one clear choice per screen.
    // [UIT 0435] First it asks GitHub for the newest release and, if that is newer than the package inside this exe, installs that
    // one instead (checked by its SHA-256; anything wrong: the package inside is used). See "the newest release, online" below.
    // What it does: copies the source package into the folder, gets the build tools (the upstream builder package from
    // GitHub, as tools/setup.py does; a builder zip next to the installer is used instead), then runs tools/update.py
    // firstbuild (tools/setup.py --dump <the player's own game files>). Nothing from the game is ever downloaded or
    // bundled: the player points it at their own dump, checked here first (SHA-256 of the three code files, the same
    // facts tools/setup.py checks). Over an existing install it updates / repairs instead (tools/update.py install: backed
    // up, saves and settings kept, put back on a failure). A fresh install that fails or is stopped can be tried again
    // or removed: the installer then removes everything it put on the PC (only a folder carrying its own mark).
    // Included inside struct App (launcher/main.cpp); the OS calls are in install_os.cpp, the zip reading in unzip.cpp.
    enum InstPage { IP_Welcome, IP_Folder, IP_Game, IP_Ready, IP_Work, IP_Done, IP_Failed, IP_Stopped };
    static constexpr int64_t kInstNeed = 32ll << 30;      // README: about 32 GB free for the build
    static constexpr const char *kInstMark = ".uit-setup";  // in a folder this installer is filling (removable)
    struct Inst {
        bool on = false;
        InstPage page = IP_Welcome;
        mini::string payload;                  // the file holding the source package (this exe, or --payload)
        int64_t pay_start = 0, pay_end = -1;
        mini::string version, commit;          // the package's VERSION.txt
        // [UIT 0435] the version inside this exe, and the package this try installs (the worker thread's: this exe's own, or a
        // newer download); version / commit above become the installed one when the work has finished
        mini::string emb_version, emb_commit;
        mini::string pk_path, pk_version, pk_commit;
        int64_t pk_start = 0, pk_end = -1;
        mini::string newest_zip;               // a newer package downloaded for this try (removed when the install has used it)
        bool used_newest = false;
        mini::string folder, game;             // the chosen install folder and game files
        mini::string folder_note, game_note;   // what the check found, in plain words
        int folder_ok = 0, game_ok = 0;        // 0 nothing yet, 1 fine, -1 not usable, 2 (game) checking
        bool repair = false;                   // an install is already in the folder: update / repair it
        mini::string have_version;             // the version installed there
        int64_t free = -1;
        bool made_folder = false, fresh = true;
        mini::vector<mini::string> made_parents;   // folders above it that the install made (removed again with it when empty)
        bool nvidia = false, dlss = false, shortcut = true;
        int focus = 0;
        mini::string remembered;               // the folder of the last install (the installer's own settings)
        // the game files check (a thread)
        SDL_Thread *chk = nullptr;
        SDL_AtomicInt chk_done;
        int chk_ok = 0;
        mini::string chk_note, chk_path, chk_for;
        // the work before the build (a thread): copying the files, the build tools
        SDL_Thread *work = nullptr;
        SDL_Mutex *mu = nullptr;
        volatile int stop = 0;
        SDL_AtomicInt work_done;
        int w_at = 1, w_result = 0;            // result: 1 ok, -1 failed, -2 stopped
        long long w_done = -1, w_total = -1;
        mini::string w_unit, w_title, w_err, w_tail, w_body;
        float w_pct = 0;
        double t0 = 0;
        mini::string log_path, prog;
        // the end
        mini::string end_title, end_body, end_log;
        bool can_report = false, removing = false, cancelling = false, retried = false;
        SDL_Thread *rm = nullptr;
        SDL_AtomicInt rm_done;
        bool rm_ok = true;
        mini::string rm_err;
        bool quit_after_rm = false;
        int ask = 0, ask_focus = 1;            // 1: the stop question while installing
        mini::string started;                  // the launcher started at the end (tests)
        mini::string repair_zip, commit_had;   // the package written out for tools/update.py install; the commit installed before
        mini::string typed_folder, typed_game; // the typed paths last applied (the Type a path dialog)
        struct Ctl { mini::function<void()> fn; bool primary; mini::string label; };
        mini::vector<Ctl> ctls;                // this frame's buttons, in order (focus, keyboard and controller)
        bool want_primary = true;              // a new page: focus its main button
        mini::string cmd_note, shown;          // the last page's text (the tests)
    } inst;

    // ---------------------------------------------------------------- the package and the paths
    static mini::string inst_sep() {
#ifdef _WIN32
        return "\\";
#else
        return "/";
#endif
    }
    static mini::string inst_native(const mini::string &p) {   // '/' or '\\' as this OS writes them
        mini::string o;
        for (char c : p) o += (c == '/' || c == '\\') ? inst_sep()[0] : c;
        return o;
    }
    static mini::string inst_parent(const mini::string &p) {
        size_t a = p.rfind('/'), b = p.rfind('\\');
        size_t cut = a == mini::string::npos ? b : b == mini::string::npos ? a : mini::max(a, b);
        if (cut == mini::string::npos) return {};
        mini::string up = p.substr(0, cut);
        if (up.size() == 2 && up[1] == ':') up += '\\';
        if (up.empty()) up = "/";
        return up;
    }
    static mini::string inst_base(const mini::string &p) {
        mini::string q = trim_slash(p);
        size_t a = q.rfind('/'), b = q.rfind('\\');
        size_t cut = a == mini::string::npos ? b : b == mini::string::npos ? a : mini::max(a, b);
        return cut == mini::string::npos ? q : q.substr(cut + 1);
    }
    static mini::string inst_lower(const mini::string &s) { mini::string o; for (char c : s) o += (char)SDL_tolower((unsigned char)c); return o; }
    static bool inst_is_root(const mini::string &p) {
        mini::string q = trim_slash(p);
        return q.empty() || q == "/" || (q.size() == 2 && q[1] == ':');
    }
    static bool inst_inside(const mini::string &inner, const mini::string &outer) {   // inner is outer or below it
        mini::string a = inst_lower(inst_native(trim_slash(inner))), b = inst_lower(inst_native(trim_slash(outer)));
        return !b.empty() && (a == b || (a.size() > b.size() && a.substr(0, b.size()) == b && (a[b.size()] == '/' || a[b.size()] == '\\')));
    }
    static mini::string gb(int64_t b) { return num((b + (1ll << 29)) >> 30) + " GB"; }
    static bool inst_has(const mini::string &folder, const char *rel) { return file_exists(join(folder, inst_native(rel))); }
    static bool inst_empty_dir(const mini::string &p) {
        bool any = false;
        SDL_EnumerateDirectory(p.c_str(), [](void *ud, const char *, const char *) { *(bool *)ud = true; return SDL_ENUM_SUCCESS; }, &any);
        return !any;
    }
    // "Ultra Instinct Tears 1.0.4 (commit abc1234)" -> version, commit
    static void inst_parse_version(const mini::string &t, mini::string *v, mini::string *c) {
        mini::string line = split(t, '\n').empty() ? mini::string() : split(t, '\n')[0];
        const char *pre = "Ultra Instinct Tears ";
        size_t at = line.find(pre);
        mini::string rest = at == mini::string::npos ? line : line.substr(at + SDL_strlen(pre));
        size_t cp = rest.find(" (commit ");
        *v = cp == mini::string::npos ? rest : rest.substr(0, cp);
        *c = cp == mini::string::npos ? mini::string() : rest.substr(cp + 9);
        while (!c->empty() && (c->back() == ')' || c->back() == ' ')) c->pop_back();
    }
    mini::string inst_pref() const {   // the installer's own small folder (the last install, a copy of a removed install's log)
        char *p = SDL_GetPrefPath("UltraInstinctTears", "Setup");
        mini::string o = p ? p : "";
        SDL_free(p);
        if (const char *t = SDL_getenv("UIT_SETUP_TEST_PREF")) o = t;   // the tests
        return o;
    }

    // Started as the installer? The package appended to this exe ends with "UITSETUP" and its length (8 bytes, little
    // endian); --install --payload ZIP does the same with a zip file (the tests).
    bool inst_detect(int argc, char **argv) {
        mini::string pay;
        bool force = false;
        for (int k = 1; k < argc; k++) {
            mini::string a = argv[k];
            if (a == "--install") force = true;
            else if (a == "--payload" && k + 1 < argc) pay = argv[++k];
        }
        if (!pay.empty()) { inst.payload = pay, inst.pay_start = 0, inst.pay_end = -1; inst.on = true; }
        else {
            mini::string me = self_exe();
            SDL_IOStream *io = me.empty() ? nullptr : SDL_IOFromFile(me.c_str(), "rb");
            if (io) {
                int64_t size = SDL_GetIOSize(io);
                unsigned char t[16];
                if (size > 16 && SDL_SeekIO(io, size - 16, SDL_IO_SEEK_SET) == size - 16 && SDL_ReadIO(io, t, 16) == 16 && !SDL_memcmp(t, "UITSETUP", 8)) {
                    int64_t n = 0;
                    for (int k = 7; k >= 0; k--) n = n << 8 | t[8 + k];
                    if (n > 0 && n <= size - 16) inst.payload = me, inst.pay_end = size - 16, inst.pay_start = size - 16 - n, inst.on = true;
                }
                SDL_CloseIO(io);
            }
        }
        if (!inst.on && !force) return false;
        inst.on = true;
        if (const char *lf = SDL_getenv("UIT_SETUP_TEST_LOG"))   // the tests on Windows (a GUI program has no console for SDL_Log)
            SDL_SetLogOutputFunction([](void *ud, int, SDL_LogPriority, const char *m) {
                if (SDL_IOStream *io = SDL_IOFromFile((const char *)ud, "ab")) { SDL_WriteIO(io, m, SDL_strlen(m)); SDL_WriteIO(io, "\n", 1); SDL_CloseIO(io); }
            }, (void *)lf);
        if (!inst.payload.empty()) {
            Zip z;
            mini::string err, data;
            if (zip_open(&z, inst.payload, inst.pay_start, inst.pay_end, &err)) {
                for (auto &e : z.entries)
                    if (e.name == "UltraInstinctTears/VERSION.txt" && zip_read(&z, e, &data, &err)) inst_parse_version(data, &inst.version, &inst.commit);
                zip_close(&z);
            }
        }
        inst.emb_version = inst.version, inst.emb_commit = inst.commit;
        inst.mu = SDL_CreateMutex();
        SDL_SetAtomicInt(&inst.chk_done, 0);
        SDL_SetAtomicInt(&inst.work_done, 0);
        SDL_SetAtomicInt(&inst.rm_done, 0);
        inst.remembered = trim_slash(read_shared(join(inst_pref(), "install.txt")));
        while (!inst.remembered.empty() && (inst.remembered.back() == '\n' || inst.remembered.back() == '\r')) inst.remembered.pop_back();
        if (!inst.remembered.empty() && !(inst_has(inst.remembered, "tools/setup.py") && inst_has(inst.remembered, "VERSION.txt"))) inst.remembered.clear();
        return true;
    }

    // the folder to suggest: <drive>\Games\UltraInstinctTears on the system drive, or the fixed drive with the most room
    mini::string inst_default_folder() {
        mini::string best;
#ifdef _WIN32
        mini::string sys = SDL_getenv("SystemDrive") ? mini::string(SDL_getenv("SystemDrive")) + "\\" : mini::string("C:\\");
        int64_t best_free = -1, sys_free = free_bytes(sys);
        for (auto &dr : fixed_drives()) {
            int64_t f = free_bytes(dr);
            if (f > best_free) best_free = f, best = dr;
        }
        if (sys_free >= kInstNeed + (8ll << 30) || best.empty()) best = sys;
        return best + "Games\\UltraInstinctTears";
#else
        best = home_folder();
        return join(join(best, "Games"), "UltraInstinctTears");
#endif
    }

    // ---------------------------------------------------------------- page 2: the install folder
    void inst_set_folder(mini::string f) {
        f = trim_slash(f);
        inst.repair = false, inst.have_version.clear();
        if (f.empty()) { inst.folder.clear(); inst.folder_ok = 0; inst.folder_note = "Choose a folder for the game."; return; }
        // a folder that has other things in it gets a new folder of its own inside
        bool ours = inst_has(f, "tools/setup.py") && inst_has(f, "VERSION.txt");
        if (!ours && (inst_is_root(f) || (is_dir(f) && !inst_empty_dir(f)))) f = join(f, "UltraInstinctTears");
        inst.folder = f;
        inst_check_folder();
    }
    void inst_check_folder() {
        const mini::string &f = inst.folder;
        inst.free = free_bytes(f);
        inst.folder_ok = -1;
        for (char c : f)
            if (c == ' ' || (unsigned char)c >= 128) {
                inst.folder_note = "Please choose a folder whose path has no spaces or special letters, for example "
                                   + inst_default_folder() + ".";
                return;
            }
#ifdef _WIN32
        for (const char *ev : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432", "SystemRoot"}) {
            const char *v = SDL_getenv(ev);
            if (v && *v && inst_inside(f, v)) {
                inst.folder_note = "Windows protects this folder, so the game can't be built there. Choose another one, like " + inst_default_folder() + ".";
                return;
            }
        }
#endif
        if (!inst.game.empty() && (inst_inside(f, inst.game) || inst_inside(inst.game, f))) {
            inst.folder_note = "This is where your game files are. Choose a separate folder for Ultra Instinct Tears.";
            return;
        }
        if (inst_has(f, "tools/setup.py") && inst_has(f, "VERSION.txt")) {
            if (file_exists(join(f, kInstMark))) {   // an install this installer started and did not finish: continue it
                inst.folder_ok = 1, inst.fresh = true;
                inst.folder_note = "An unfinished install is in this folder. It will pick up where it stopped.";
                return;
            }
            mini::string c;
            inst_parse_version(read_shared(join(f, "VERSION.txt")), &inst.have_version, &c);
            inst.repair = true, inst.fresh = false, inst.folder_ok = 1;
            inst.folder_note = "Ultra Instinct Tears is already installed here" + (inst.have_version.empty() ? mini::string() : " (version " + inst.have_version + ")") +
                               ". It will be " + (inst_same_version(c) ? "repaired" : "updated") + ". Your saves and settings are kept.";
            return;
        }
        inst.fresh = true;
        if (inst.free >= 0 && inst.free < kInstNeed) {
            mini::string drive = f.size() > 1 && f[1] == ':' ? f.substr(0, 2) : mini::string("this drive");
            inst.folder_note = "There isn't enough free space on " + drive + ". It has " + gb(inst.free) + " free and the game needs about " +
                               gb(kInstNeed) + ". Choose a folder on another drive, or free up some space.";
            return;
        }
        inst.folder_ok = 1;
        inst.folder_note = inst.free >= 0 ? gb(inst.free) + " free. The game needs about " + gb(kInstNeed) + "." : "The game needs about " + gb(kInstNeed) + ".";
    }
    bool inst_same_version(const mini::string &commit) const { return !commit.empty() && commit == inst.commit; }

    // ---------------------------------------------------------------- page 3: the game files (checked in a thread)
    // The same facts tools/setup.py checks (EXEFS there; tools/tests/test_installer.py keeps the two the same): the
    // SHA-256 of the three code files of Tears of the Kingdom 1.4.3, and Pack\ZsDic.pack.zs in the RomFS.
    static constexpr const char *kExefs[3][2] = {
        {"main", "e84a90d551fed0824b4b74933b9c6d043c458521363a7cea58b2faca1822dc9e"},
        {"subsdk0", "767c35bcd7d23e78f9063c69b638290b8b83f91e839beb10799e6576960179b1"},
        {"sdk", "1d187f29784cd0af46e9e123fc2a62c77b3b07c5a39c8633d90d6c80de64e4ad"}};
    static bool inst_packed_name(const mini::string &n) {
        mini::string l = inst_lower(n);
        for (const char *x : {".nsp", ".nsz", ".xci", ".xcz", ".nca", ".ncz"})
            if (l.size() > SDL_strlen(x) && l.substr(l.size() - SDL_strlen(x)) == x) return true;
        return false;
    }
    static mini::vector<mini::string> inst_list(const mini::string &dir, bool dirs) {
        struct L { mini::string dir; bool dirs; mini::vector<mini::string> out; } l{dir, dirs, {}};
        SDL_EnumerateDirectory(dir.c_str(), [](void *ud, const char *d0, const char *f) {
            L *l = (L *)ud;
            mini::string p = mini::string(d0) + f;
            if (is_dir(p) == l->dirs) l->out.push_back(f);
            return SDL_ENUM_CONTINUE;
        }, &l);
        return l.out;
    }
    // where the game files are, starting from what the player chose (the folder itself, its parent when they picked
    // exefs or romfs, or one folder down); sets *note when they are not usable
    static int inst_game_check(const mini::string &chosen, mini::string *path, mini::string *note, const mini::string &test_hashes) {
        mini::string c = trim_slash(chosen);
        if (file_exists(c)) {
            *note = inst_packed_name(c) ? "This is a packed game file. Ultra Instinct Tears needs the game's files unpacked into two folders, "
                                          "exefs and romfs. It can't open .nsp or .xci files."
                                        : "Choose the folder that holds your exefs and romfs folders, not a single file.";
            return -1;
        }
        if (!is_dir(c)) { *note = "That folder doesn't exist any more. Choose the folder that holds your exefs and romfs folders."; return -1; }
        mini::vector<mini::string> cands = {c};
        mini::string b = inst_lower(inst_base(c));
        if (b == "exefs" || b == "romfs") cands.insert(cands.begin(), inst_parent(c));
        for (auto &sub : inst_list(c, true)) cands.push_back(join(c, sub));
        mini::string at;
        for (auto &k : cands)
            if (is_dir(join(k, "exefs")) || is_dir(join(k, "romfs"))) { at = k; break; }
        if (at.empty()) {
            for (auto &f : inst_list(c, false))
                if (inst_packed_name(f)) {
                    *note = "This folder has a packed game file (" + f + "). Ultra Instinct Tears needs the game's files unpacked into two folders, "
                            "exefs and romfs. It can't open packed files.";
                    return -1;
                }
            *note = "There are no game files in this folder. Choose the folder that has two folders inside it, named exefs and romfs.";
            return -1;
        }
        *path = at;
        mini::string ex = join(at, "exefs"), ro = join(at, "romfs");
        for (auto &f : inst_list(ex, false))
            if (inst_packed_name(f)) { *note = "The exefs folder has a packed file (" + f + ") instead of the game's own files. Dump the game again as plain files."; return -1; }
        if (!is_dir(ex)) { *note = "The exefs folder is missing. It should be next to the romfs folder."; return -1; }
        if (!is_dir(ro)) { *note = "The romfs folder is missing. It should be next to the exefs folder."; return -1; }
        for (auto &e : kExefs) {
            mini::string f = join(ex, e[0]), want = e[1];
            if (!test_hashes.empty()) {   // the tests: stand-in files with their own hashes ("name=sha256;...")
                size_t p = test_hashes.find(mini::string(e[0]) + "=");
                if (p != mini::string::npos) want = test_hashes.substr(p + SDL_strlen(e[0]) + 1, 64);
            }
            if (!file_exists(f)) {
                *note = "The exefs folder is missing a file named " + mini::string(e[0]) + ". Dump the game again with update 1.4.3 applied.";
                return -1;
            }
            if (sha256_file(f) != want) {
                *note = "These game files are a different version. Ultra Instinct Tears needs Tears of the Kingdom with update 1.4.3 applied. "
                        "Dump the game again with that update installed on your Switch.";
                return -1;
            }
        }
        if (!file_exists(join(join(ro, "Pack"), "ZsDic.pack.zs"))) {
            *note = "The romfs folder doesn't look like Tears of the Kingdom's. Dump it again from the same game.";
            return -1;
        }
        *note = "Tears of the Kingdom 1.4.3 found.";
        return 1;
    }
    static int SDLCALL inst_check_thread(void *ud) {
        App *a = (App *)ud;
        mini::string path, note;
        int ok = inst_game_check(a->inst.chk_for, &path, &note, a->cmds.empty() ? mini::string() : mini::string(SDL_getenv("UIT_SETUP_TEST_EXEFS") ? SDL_getenv("UIT_SETUP_TEST_EXEFS") : ""));
        SDL_LockMutex(a->inst.mu);
        a->inst.chk_ok = ok, a->inst.chk_note = note, a->inst.chk_path = path;
        SDL_UnlockMutex(a->inst.mu);
        SDL_SetAtomicInt(&a->inst.chk_done, 1);
        return 0;
    }
    void inst_set_game(const mini::string &g) {
        if (inst.chk) return;   // one check at a time
        inst.game = trim_slash(g);
        if (inst.game.empty()) { inst.game_ok = 0; inst.game_note.clear(); return; }
        if (!inst.folder.empty() && (inst_inside(inst.game, inst.folder) || inst_inside(inst.folder, inst.game))) {
            inst.game_ok = -1;
            inst.game_note = "These game files are inside the install folder. Keep them in a folder of their own, or go back and choose another install folder.";
            return;
        }
        inst.game_ok = 2, inst.game_note = "Checking your game files\xE2\x80\xA6";
        inst.chk_for = inst.game;
        SDL_SetAtomicInt(&inst.chk_done, 0);
        inst.chk = SDL_CreateThread(inst_check_thread, "uit-check", this);
        if (!inst.chk) { inst.game_ok = -1; inst.game_note = "The game files couldn't be checked (" + mini::string(SDL_GetError()) + ")."; }
    }
    void inst_check_poll() {
        if (!inst.chk || !SDL_GetAtomicInt(&inst.chk_done)) return;
        SDL_WaitThread(inst.chk, nullptr);
        inst.chk = nullptr;
        SDL_LockMutex(inst.mu);
        inst.game_ok = inst.chk_ok, inst.game_note = inst.chk_note;
        if (inst.chk_ok == 1) inst.game = inst.chk_path;
        SDL_UnlockMutex(inst.mu);
        if (inst.game_ok == 1 && !inst.folder.empty() && (inst_inside(inst.game, inst.folder) || inst_inside(inst.folder, inst.game))) {
            inst.game_ok = -1;   // found one folder up or down: that one may hold the install folder
            inst.game_note = "These game files are inside the install folder. Keep them in a folder of their own, or go back and choose another install folder.";
        }
        if (inst.game_ok == 1 && inst.page == IP_Game) inst.want_primary = true;   // on to Next
    }

    // ---------------------------------------------------------------- the work: copy the files, get the build tools
    void inst_log(const mini::string &t) {   // into the installer's log and the last lines (Show details); any thread
        SDL_LockMutex(inst.mu);
        mini::string line = "[" + clock_text(SDL_GetTicksNS() / 1e9 - inst.t0) + "] " + t;
        if (!inst.log_path.empty()) {
            SDL_IOStream *io = SDL_IOFromFile(inst.log_path.c_str(), "ab");
            if (io) { SDL_WriteIO(io, line.data(), line.size()); SDL_WriteIO(io, "\n", 1); SDL_CloseIO(io); }
        }
        inst.w_tail = last_of(inst.w_tail + "\n" + line, 12);
        SDL_UnlockMutex(inst.mu);
        SDL_Log("setup: %s", t.c_str());
    }
    void inst_progress(int at, const mini::string &title, float pct, long long done, long long total, const mini::string &unit) {
        SDL_LockMutex(inst.mu);
        inst.w_at = at, inst.w_title = title, inst.w_pct = pct, inst.w_done = done, inst.w_total = total, inst.w_unit = unit;
        SDL_UnlockMutex(inst.mu);
    }
    static bool inst_write(const mini::string &path, const mini::string &data, mini::string *err) {
        SDL_CreateDirectory(inst_parent(path).c_str());
        mini::string part = path + ".part";
        SDL_IOStream *io = SDL_IOFromFile(part.c_str(), "wb");
        if (!io) { *err = "cannot write " + path + ": " + SDL_GetError(); return false; }
        bool ok = data.empty() || SDL_WriteIO(io, data.data(), data.size()) == data.size();
        ok = SDL_CloseIO(io) && ok;
        if (ok) { SDL_RemovePath(path.c_str()); ok = SDL_RenamePath(part.c_str(), path.c_str()); }
        if (!ok) { SDL_RemovePath(part.c_str()); *err = "cannot write " + path + " (is the disk full?)"; }
        return ok;
    }
    // step 1: the package's files (its top folder UltraInstinctTears/ left out) into the folder
    bool inst_copy_files(mini::string *err) {
        Zip z;
        if (!zip_open(&z, inst.pk_path, inst.pk_start, inst.pk_end, err)) { *err = "the installer's own files are damaged (" + *err + "). Download the installer again."; return false; }
        mini::vector<int> files;
        for (int k = 0; k < (int)z.entries.size(); k++) if (!z.entries[k].dir) files.push_back(k);
        long long n = 0;
        for (int k : files) {
            if (inst.stop) { zip_close(&z); *err = "stopped"; return false; }
            const ZipEntry &e = z.entries[k];
            mini::string rel = zip_safe_name(e.name);
            if (rel.substr(0, 19) == "UltraInstinctTears/") rel = rel.substr(19);
            else if (!rel.empty()) { size_t sl = rel.find('/'); rel = sl == mini::string::npos ? rel : rel.substr(sl + 1); }
            if (rel.empty()) { zip_close(&z); *err = "the installer's own files are damaged (" + e.name + "). Download the installer again."; return false; }
            mini::string data;
            if (!zip_read(&z, e, &data, err) || !inst_write(join(inst.folder, inst_native(rel)), data, err)) { zip_close(&z); return false; }
            n++;
            if (n % 16 == 0 || n == (long long)files.size()) inst_progress(1, "Getting ready", 4.0f + 4.0f * n / files.size(), n, (long long)files.size(), "files");
        }
        zip_close(&z);
        inst_log("copied " + num(n) + " files into " + inst.folder);
        return true;
    }
    bool inst_tools_ready() const {
        for (const char *p : {"deps/python/python.exe", "deps/clang64/bin/clang.exe", "deps/clang64/bin/ninja.exe", "deps/shaders/ultrs-shaders.exe"})
            if (!inst_has(inst.folder, p)) return false;
        return true;
    }
    static bool inst_builder_name(const mini::string &n) {
        return n.size() > 38 && n.substr(0, 19) == "UltraTears-builder-" && n.substr(n.size() - 19) == "-windows-x86_64.zip";
    }
    mini::string inst_local_builder() {   // a builder zip already here: the tests, a second try, or one put next to the installer
        if (const char *t = SDL_getenv("UIT_SETUP_TOOLS")) if (file_exists(t)) return t;
        mini::vector<mini::string> dirs = {join(join(inst.folder, "dev-local"), "downloads"), inst_parent(self_exe())};
        if (!inst.payload.empty()) dirs.push_back(inst_parent(inst.payload));
        for (auto &dd : dirs) {
            if (dd.empty()) continue;
            mini::vector<mini::string> fs = inst_list(dd, false);
            for (int k = (int)fs.size() - 1; k >= 0; k--)
                if (inst_builder_name(fs[k])) return join(dd, fs[k]);
        }
        return {};
    }
    struct DlCtx { App *a; double last; };
    static void SDLCALL inst_dl_cb(void *ud, int64_t done, int64_t total) {
        DlCtx *c = (DlCtx *)ud;
        double t = SDL_GetTicksNS() / 1e9;
        if (t - c->last < 0.2 && done != total) return;
        c->last = t;
        long long mb = done >> 20, tm = total > 0 ? (total + (1 << 19)) >> 20 : -1;
        c->a->inst_progress(2, "Getting ready", total > 0 ? 8.0f + 8.0f * (float)done / (float)total : 8.0f, mb, tm, "MB");
    }
    // step 2: the build tools: the upstream builder package (the same one tools/setup.py downloads), its deps\ and
    // licenses\ unpacked into the folder
    bool inst_get_tools(mini::string *err) {
        if (inst_tools_ready()) { inst_log("the build tools are already in the folder"); inst_progress(2, "Getting ready", 24, -1, -1, ""); return true; }
        mini::string zip = inst_local_builder();
        if (zip.empty()) {
            inst_progress(2, "Getting ready", 8, -1, -1, "");
            mini::string api = SDL_getenv("UIT_SETUP_API") ? SDL_getenv("UIT_SETUP_API") : "https://api.github.com/repos/kuraeow/ultratears/releases?per_page=20";
            mini::string text, name, url;
            inst_log("asking GitHub for the build tools: " + api);
            if (!http_get_text(api, &text, err)) { *err = "download: " + *err; return false; }
            // the first asset named UltraTears-builder-*-windows-x86_64.zip and its download address (newest release first)
            size_t p = 0;
            while ((p = text.find("\"name\":", p)) != mini::string::npos) {
                size_t q1 = text.find('"', p + 7), q2 = q1 == mini::string::npos ? q1 : text.find('"', q1 + 1);
                if (q2 == mini::string::npos) break;
                mini::string n = text.substr(q1 + 1, q2 - q1 - 1);
                p = q2;
                if (!inst_builder_name(n)) continue;
                size_t u = text.find("\"browser_download_url\":", p);   // the asset's own (it follows its name)
                size_t v1 = u == mini::string::npos ? u : text.find('"', u + 23), v2 = v1 == mini::string::npos ? v1 : text.find('"', v1 + 1);
                if (v2 == mini::string::npos) break;
                name = n, url = text.substr(v1 + 1, v2 - v1 - 1);
                break;
            }
            if (name.empty() || url.substr(0, 8) != "https://") { *err = "download: no build tools package was found on GitHub"; return false; }
            mini::string dl = join(join(inst.folder, "dev-local"), "downloads");
            SDL_CreateDirectory(dl.c_str());
            zip = join(dl, name);
            inst_log("downloading " + name + " from " + url);
            DlCtx ctx{this, 0};
            if (!http_get(url, zip + ".part", &inst.stop, inst_dl_cb, &ctx, err)) {
                SDL_RemovePath((zip + ".part").c_str());
                if (*err != "stopped") *err = "download: " + *err;
                return false;
            }
            SDL_RemovePath(zip.c_str());
            if (!SDL_RenamePath((zip + ".part").c_str(), zip.c_str())) { *err = "cannot write " + zip; return false; }
        } else inst_log("using the build tools package " + zip);
        Zip z;
        if (!zip_open(&z, zip, 0, -1, err)) {
            if (zip.find("dev-local") != mini::string::npos) SDL_RemovePath(zip.c_str());   // a damaged download: fetched again next time
            *err = "download: the build tools package is damaged (" + *err + ")";
            return false;
        }
        mini::vector<mini::pair<int, mini::string>> todo;   // (entry, path below the folder) for deps\ and licenses\ only
        for (int k = 0; k < (int)z.entries.size(); k++) {
            if (z.entries[k].dir) continue;
            mini::string rel = zip_safe_name(z.entries[k].name);
            auto parts = split(rel, '/');
            for (size_t i = 0; i < parts.size(); i++)
                if (parts[i] == "deps" || parts[i] == "licenses") {
                    mini::string r;
                    for (size_t j = i; j < parts.size(); j++) r += (r.empty() ? "" : "/") + parts[j];
                    todo.push_back({k, r});
                    break;
                }
        }
        long long n = 0;
        for (auto &t : todo) {
            if (inst.stop) { zip_close(&z); *err = "stopped"; return false; }
            mini::string data;
            if (!zip_read(&z, z.entries[t.first], &data, err) || !inst_write(join(inst.folder, inst_native(t.second)), data, err)) {
                zip_close(&z);
                if (zip.find("dev-local") != mini::string::npos && err->find("damaged") != mini::string::npos) SDL_RemovePath(zip.c_str());
                return false;
            }
            n++;
            if (n % 8 == 0 || n == (long long)todo.size()) inst_progress(2, "Getting ready", 16.0f + 8.0f * n / todo.size(), n, (long long)todo.size(), "files");
        }
        zip_close(&z);
        inst_log("unpacked " + num(n) + " files of the build tools");
        if (!inst_tools_ready()) { *err = "the build tools package does not have everything the build needs"; return false; }
        if (zip.find("dev-local") != mini::string::npos) SDL_RemovePath(zip.c_str());   // unpacked: the download's space back
        return true;
    }

    // ---------------------------------------------------------------- [UIT 0435] the newest release, online
    // Before anything is copied the installer asks GitHub for the newest release, by the launcher's own rules (tools/update.py:
    // the repository SuperSenad/Ultra-Instinct-Tears, the package UltraInstinctTears-src.zip, its SHA-256 from the release's
    // .sha256 file, GitHub's asset digest or the notes). If that release is newer than the package inside this exe, its
    // package is downloaded (progress in MB, Stop works), checked, and installed instead. Which releases count:
    //   - the Latest release, always;
    //   - pre-releases too, but only when the package inside this exe is itself newer than Latest (an installer made from a
    //     test build follows test builds; the installer on the download page, made at a release, never lands on a test build).
    // Anything that goes wrong (offline, GitHub's hourly limit, no checksum, a wrong checksum, a damaged zip) is only
    // logged: the package inside the exe is installed, as it always was. Only the log has the details.
    static constexpr const char *kInstRepoApi = "https://api.github.com/repos/SuperSenad/Ultra-Instinct-Tears";
    static constexpr const char *kInstNewestTitle = "Getting the newest version";
    struct NewDl { App *a; double last; int64_t hint; };
    static void SDLCALL inst_newest_cb(void *ud, int64_t done, int64_t total) {
        NewDl *c = (NewDl *)ud;
        double t = SDL_GetTicksNS() / 1e9;
        if (t - c->last < 0.2 && done != total) return;
        c->last = t;
        if (total <= 0) total = c->hint;
        long long mb = done >> 20, tm = total > 0 ? (total + (1 << 19)) >> 20 : -1;
        float f = total > 0 ? mini::min(1.0f, (float)done / (float)total) : 0.0f;
        c->a->inst_progress(1, kInstNewestTitle, 0.5f + 3.0f * f, mb, tm, "MB");
    }
    void inst_pk_embedded() { inst.pk_path = inst.payload, inst.pk_start = inst.pay_start, inst.pk_end = inst.pay_end, inst.pk_version = inst.emb_version, inst.pk_commit = inst.emb_commit; }
    mini::string inst_releases_api() const {
        if (!cmds.empty()) if (const char *t = SDL_getenv("UIT_SETUP_RELEASES")) return t;   // the tests (a --script run only)
        return kInstRepoApi;
    }
    // the release to install: the Latest; or, when this exe's own package is newer than Latest, the newest of all (pre-releases too)
    bool inst_pick_release(Json &rel_json, int *rel, mini::string *why) {
        const mini::string base = inst_releases_api();
        mini::string text, err;
        inst_log("asking GitHub for the newest release: " + base + "/releases/latest");
        if (!http_get_text(base + "/releases/latest", &text, &err)) { *why = "GitHub did not answer (" + err + ")"; return false; }
        Json lj;
        if (!lj.parse(text) || !lj.is_object(lj.root)) { *why = "GitHub's answer for the Latest release could not be read"; return false; }
        mini::string lv = tag_version(lj.text(lj.root, "tag_name"));
        VerKey emb = ver_key(inst.emb_version), lk = ver_key(lv);
        bool follow_pre = emb.ok && (!lk.ok || ver_cmp(emb, lk) > 0);
        if (!follow_pre) {
            if (!lk.ok) { *why = "the Latest release is tagged \"" + lj.text(lj.root, "tag_name") + "\", not vX.Y.Z"; return false; }
            rel_json = lj, *rel = lj.root;
            inst_log("Latest is " + lv + "; this installer carries " + inst.emb_version);
            return true;
        }
        inst_log("this installer carries " + inst.emb_version + ", newer than Latest" + (lk.ok ? " (" + lv + ")" : mini::string()) + ": looking at pre-releases too");
        if (!http_get_text(base + "/releases?per_page=30", &text, &err)) { *why = "GitHub did not answer for the release list (" + err + ")"; return false; }
        Json all;
        if (!all.parse(text) || !all.is_array(all.root)) { *why = "GitHub's release list could not be read"; return false; }
        int best = -1;
        VerKey bk;
        for (int id : all.v[all.root].kids) {
            if (!all.is_object(id) || all.flag(id, "draft")) continue;
            VerKey k = ver_key(tag_version(all.text(id, "tag_name")));
            if (k.ok && (best < 0 || ver_cmp(k, bk) > 0)) best = id, bk = k;
        }
        if (best < 0) { *why = "no release is tagged vX.Y.Z"; return false; }
        rel_json = all, *rel = best;
        inst_log("newest of all releases: " + all.text(best, "tag_name"));
        return true;
    }
    // 1 a newer package was downloaded and checked (payload now points at it), 0 not used (the reason is logged), -2 stopped
    int inst_try_newest() {
        inst_pk_embedded();
        inst.newest_zip.clear(), inst.used_newest = false;
        inst_progress(1, kInstNewestTitle, 0, -1, -1, "");
        mini::string why, err;
        Json rj;
        int rel = -1;
        if (!inst_pick_release(rj, &rel, &why)) { inst_log("using the installer's own copy: " + why); return inst.stop ? -2 : 0; }
        if (inst.stop) return -2;
        mini::string tag = rj.text(rel, "tag_name"), ver = tag_version(tag);
        VerKey nk = ver_key(ver), ek = ver_key(inst.emb_version);
        if (!nk.ok) { inst_log("using the installer's own copy: the release " + tag + " is not tagged vX.Y.Z"); return 0; }
        if (ek.ok && ver_cmp(nk, ek) <= 0) { inst_log("the installer's own copy (" + inst.emb_version + ") is already the newest (" + ver + ")"); return 0; }
        // the package among the release's files
        int assets = rj.get(rel, "assets"), pick = -1, zips = 0, only = -1;
        if (rj.is_array(assets))
            for (int a : rj.v[assets].kids) {
                mini::string n = rj.text(a, "name");
                if (lower_of(n) == "ultrainstincttears-src.zip") { pick = a; break; }
                if (src_zip_name(n)) zips++, only = a;
            }
        if (pick < 0 && zips == 1) pick = only;
        if (pick < 0) { inst_log("using the installer's own copy: the release " + tag + " has no UltraInstinctTears-src.zip"); return 0; }
        mini::string name = rj.text(pick, "name"), url = rj.text(pick, "browser_download_url");
        int64_t size = (int64_t)rj.number(pick, "size");
        if (url.empty() || (url.substr(0, 8) != "https://" && !(SDL_getenv("UIT_SETUP_TEST_HTTP") && url.substr(0, 7) == "http://"))) {
            inst_log("using the installer's own copy: the release has no usable download address"); return 0;
        }
        if (size <= 0 || size > (2ll << 30)) { inst_log("using the installer's own copy: the release lists a size of " + num(size) + " bytes for " + name); return 0; }
        // every SHA-256 the release publishes for it; one is required, all must agree
        mini::vector<mini::string> shas, where;
        mini::string dg = rj.text(pick, "digest");
        if (dg.size() == 71 && lower_of(dg.substr(0, 7)) == "sha256:") {
            mini::string h = lower_of(dg.substr(7));
            bool ok = true;
            for (char c : h) ok = ok && is_hex(c);
            if (ok) shas.push_back(h), where.push_back("GitHub's asset digest");
        }
        int side = -1;
        if (rj.is_array(assets))
            for (int a : rj.v[assets].kids) {
                mini::string n = lower_of(rj.text(a, "name"));
                if (n == lower_of(name) + ".sha256" || n == "sha256sums" || n == "sha256sums.txt") { side = a; break; }
            }
        if (side >= 0) {
            mini::string surl = rj.text(side, "browser_download_url"), st;
            if (surl.empty() || !http_get_text(surl, &st, &err)) { inst_log("using the installer's own copy: the checksum file could not be fetched (" + (surl.empty() ? mini::string("no address") : err) + ")"); return inst.stop ? -2 : 0; }
            mini::string h = sha_from_text(st, name, false);
            if (h.empty()) { inst_log("using the installer's own copy: " + rj.text(side, "name") + " has no SHA-256 for " + name); return 0; }
            shas.push_back(h), where.push_back(rj.text(side, "name"));
        }
        mini::string nh = sha_from_text(rj.text(rel, "body"), name, true);
        if (!nh.empty()) shas.push_back(nh), where.push_back("the release notes");
        if (shas.empty()) { inst_log("using the installer's own copy: the release " + tag + " publishes no SHA-256 for " + name + ", so it can't be checked"); return 0; }
        for (size_t k = 1; k < shas.size(); k++)
            if (shas[k] != shas[0]) { inst_log("using the installer's own copy: the release publishes different SHA-256 values for " + name + " (" + where[0] + " and " + where[k] + ")"); return 0; }
        if (inst.stop) return -2;
        // download, check, name
        mini::string dl = inst.repair ? join(inst.folder, "updates") : join(join(inst.folder, "dev-local"), "downloads");   // (updates\ is where tools/update.py install takes a package from)
        SDL_CreateDirectory(dl.c_str());
        mini::string zip = join(dl, inst.repair ? "UltraInstinctTears-src-setup.zip" : "UltraInstinctTears-src-newest.zip"), part = zip + ".part";
        inst_log("downloading " + name + " of " + tag + " (" + num(size) + " bytes) from " + url);
        NewDl ctx{this, 0, size};
        inst_progress(1, kInstNewestTitle, 0.5f, 0, (size + (1 << 19)) >> 20, "MB");
        if (!http_get(url, part, &inst.stop, inst_newest_cb, &ctx, &err)) {
            SDL_RemovePath(part.c_str());
            if (err == "stopped" || inst.stop) return -2;
            inst_log("using the installer's own copy: the download failed (" + err + ")");
            return 0;
        }
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(part.c_str(), &info) || (int64_t)info.size != size) {
            SDL_RemovePath(part.c_str());
            inst_log("using the installer's own copy: the download is " + num(SDL_GetPathInfo(part.c_str(), &info) ? (long long)info.size : -1) + " bytes, not the " + num(size) + " the release lists");
            return 0;
        }
        mini::string got = sha256_file(part, &inst.stop);
        if (inst.stop) { SDL_RemovePath(part.c_str()); return -2; }
        if (got != shas[0]) {
            SDL_RemovePath(part.c_str());
            inst_log("using the installer's own copy: the download's SHA-256 " + got.substr(0, 12) + "... is not the one in " + where[0] + " (" + shas[0].substr(0, 12) + "...)");
            return 0;
        }
        // a source package of that very version
        Zip z;
        mini::string zv, zc, vdata;
        bool opened = zip_open(&z, part, 0, -1, &err);
        if (opened) {
            for (auto &e : z.entries) if (e.name == "UltraInstinctTears/VERSION.txt" && zip_read(&z, e, &vdata, &err)) inst_parse_version(vdata, &zv, &zc);
            zip_close(&z);
        }
        if (!opened || zv.empty() || ver_cmp(ver_key(zv), nk) != 0) {
            SDL_RemovePath(part.c_str());
            inst_log(!opened ? "using the installer's own copy: the downloaded package can't be opened (" + err + ")"
                             : "using the installer's own copy: the release is tagged " + tag + " but its package says version \"" + zv + "\"");
            return 0;
        }
        SDL_RemovePath(zip.c_str());
        if (!SDL_RenamePath(part.c_str(), zip.c_str())) { SDL_RemovePath(part.c_str()); inst_log("using the installer's own copy: cannot write " + zip); return 0; }
        inst.pk_path = zip, inst.pk_start = 0, inst.pk_end = -1, inst.pk_version = zv, inst.pk_commit = zc, inst.newest_zip = zip, inst.used_newest = true;
        inst_log("installing the newest version " + zv + " (" + zc + ") instead of the installer's own " + inst.emb_version + "; SHA-256 " + got.substr(0, 12) + "... matches " + where[0]);
        return 1;
    }
    void inst_drop_newest() {   // the install has copied it: its space back
        if (!inst.newest_zip.empty()) SDL_RemovePath(inst.newest_zip.c_str());
        inst.newest_zip.clear();
    }
    static int SDLCALL inst_work_thread(void *ud) {
        App *a = (App *)ud;
        mini::string err;
        int r = 1;
        a->inst_log("Ultra Instinct Tears setup " + a->inst.version + " (" + a->inst.commit + "): " + a->inst.folder + ", game files " + a->inst.game);
        int nw = a->inst_try_newest();   // [UIT 0435] never fails the install: the installer's own copy is the fallback
        if (nw == -2) { r = -2; err = "stopped"; }
        else if (!a->inst.repair) {      // (an update over an install keeps the package for tools/update.py install)
            bool ok = a->inst_copy_files(&err);
            if (a->inst.stop) ok = false;
            if (!ok || !a->inst_get_tools(&err)) r = a->inst.stop ? -2 : -1;
        }
        if (r < 0 || !a->inst.repair) a->inst_drop_newest();
        if (r < 0) a->inst_log(r == -2 ? mini::string("stopped") : "did not finish: " + err);
        SDL_LockMutex(a->inst.mu);
        a->inst.w_result = r, a->inst.w_err = err;
        SDL_UnlockMutex(a->inst.mu);
        SDL_SetAtomicInt(&a->inst.work_done, 1);
        return 0;
    }

    // ---------------------------------------------------------------- starting and following the install
    void inst_busy_open(const mini::string &title) {
        modal = Modal();
        modal.kind = Modal::Message;
        modal.title = title, modal.busy = true, modal.pct = 0;
        modal.open_t = (float)now;
        busy = Busy();
        busy.t0 = busy.active_at = inst.t0 = now;
        busy.log = inst.log_path;
    }
    void inst_start() {
        inst.stop = 0, inst.cancelling = false;
        inst.end_title.clear(), inst.end_body.clear();
        if (!inst.retried) {
            inst.made_folder = !is_dir(inst.folder);
            inst.made_parents.clear();
            for (mini::string up = inst_parent(inst.folder); !up.empty() && !inst_is_root(up) && !is_dir(up); up = inst_parent(up)) inst.made_parents.push_back(up);
        }
        if (!SDL_CreateDirectory(inst.folder.c_str()) || !is_dir(inst.folder)) {
            inst_fail("The game couldn't be installed", "The folder " + inst.folder + " couldn't be made. Choose another folder and try again.", false);
            return;
        }
        dir = inst.folder;
        mini::string logs = join(dir, "logs");
        SDL_CreateDirectory(logs.c_str());
        inst.log_path = join(logs, "install-" + timestamp() + ".log");
        inst.page = IP_Work;
        if (inst.repair) {   // [UIT 0435] the newest package first (a short step on the same screen), then the update itself
            inst_parse_version(read_shared(join(dir, "VERSION.txt")), &inst.have_version, &inst.commit_had);
            inst_busy_open(inst_same_version(inst.commit_had) ? "Repairing Ultra Instinct Tears" : "Updating Ultra Instinct Tears");
            busy.plan = {"Getting the newest version"};
            busy.at = 1, busy.title = kInstNewestTitle;
            SDL_SetAtomicInt(&inst.work_done, 0);
            inst_progress(1, kInstNewestTitle, 0, -1, -1, "");
            inst.t0 = SDL_GetTicksNS() / 1e9;
            inst.work = SDL_CreateThread(inst_work_thread, "uit-setup", this);
            if (!inst.work) inst_repair_start();
            return;
        }
        mini::string mark = join(dir, kInstMark);
        if (!file_exists(mark)) { mini::string e; inst_write(mark, "This folder is being filled by the Ultra Instinct Tears installer.\n", &e); }
        inst_busy_open("Installing Ultra Instinct Tears");
        busy.plan = {"Copying the files", "Downloading what's needed", "Checking the game files", "Packing the game's data", "Translating the game code",
                     "Compiling the game", "Translating the shaders", "Preparing the textures", "Finishing up"};
        busy.at = 1, busy.title = "Getting ready";
        SDL_SetAtomicInt(&inst.work_done, 0);
        inst_progress(1, "Getting ready", 0, -1, -1, "");
        inst.t0 = SDL_GetTicksNS() / 1e9;
        inst.work = SDL_CreateThread(inst_work_thread, "uit-setup", this);
        if (!inst.work) inst_fail("The game couldn't be installed", "The installer couldn't start (" + mini::string(SDL_GetError()) + ").", true);
    }
    bool inst_native_busy() const { return inst.on && inst.work != nullptr; }
    void inst_poll() {
        inst_check_poll();
        if (inst.rm && SDL_GetAtomicInt(&inst.rm_done)) inst_rm_finish();
        if (!inst.work) return;
        SDL_LockMutex(inst.mu);
        int at = inst.w_at;
        mini::string title = inst.w_title, tail = inst.w_tail, unit = inst.w_unit;
        long long dn = inst.w_done, tot = inst.w_total;
        float pct = inst.w_pct;
        SDL_UnlockMutex(inst.mu);
        if (modal.busy) {
            busy.at = at, busy.title = title, busy.k = busy.n = 0;
            busy.done = dn, busy.total = tot, busy.unit = unit, busy.tail = tail, busy.line = last_of(tail, 1);
            modal.pct = pct;
            busy_activity(num(dn) + "|" + num(at) + "|" + tail.substr(tail.size() > 64 ? tail.size() - 64 : 0));
            busy_tick();
        }
        progress_chrome();
        if (!SDL_GetAtomicInt(&inst.work_done)) return;
        SDL_WaitThread(inst.work, nullptr);
        inst.work = nullptr;
        if (inst.w_result == -2 || inst.cancelling) { inst_remove(false); return; }
        if (inst.w_result < 0) {
            bool net = inst.w_err.substr(0, 9) == "download:";
            inst_fail("The game couldn't be installed",
                      net ? "The build tools couldn't be downloaded. Check your internet connection, then press Try again."
                          : "Something went wrong before it finished. View details shows what happened.", true);
            return;
        }
        if (inst.used_newest) inst.version = inst.pk_version, inst.commit = inst.pk_commit;   // what is being installed now
        if (inst.repair) { if (modal.busy) close_modal(true); inst_repair_start(); return; }
        // step 3 on: the build, on the same screen (tools/update.py firstbuild writes the progress file)
        inst.prog = join(join(dir, "logs"), "install-progress.json");
        SDL_RemovePath(inst.prog.c_str());
        task_prog = inst.prog;
        double t0 = busy.t0;
        mini::vector<mini::string> plan = busy.plan;
        mini::string args = " firstbuild --dump " + quote(inst.game) + " --progress " + quote(inst.prog) + (inst.dlss ? " --get-dlss" : "");
        if (!run_helper(T_Setup, "update.py", args, "Installing Ultra Instinct Tears", "Getting ready")) {
            inst_fail("The game couldn't be installed", "The build couldn't be started. View details shows what happened.", true);
            return;
        }
        busy.t0 = busy.active_at = t0, busy.plan = plan, busy.at = 3, busy.title = "Checking files", busy.k = 1, busy.n = 4;
        modal.pct = 24, busy.shown = 24;
    }
    // [the hook in poll_task] the build or the update / repair has ended
    void inst_task_done(int kind, int code) {
        mini::string t = read_text(task_prog), st = json_get(t, "state"), lg = json_get(t, "log");
        if (!lg.empty()) inst.end_log = join(join(dir, "logs"), lg);
        else inst.end_log = task_out;
        if (kind == T_Install && !inst.repair_zip.empty()) { SDL_RemovePath(inst.repair_zip.c_str()); inst.repair_zip.clear(); }
        if (inst.cancelling) { inst_remove(false); return; }
        if (code == 0 && st == "done") {
            SDL_RemovePath(join(dir, kInstMark).c_str());
            mini::string pref = inst_pref();
            if (!pref.empty()) { SDL_CreateDirectory(pref.c_str()); mini::string e; inst_write(join(pref, "install.txt"), dir + "\n", &e); }
            inst.page = IP_Done, inst.want_primary = true;
            inst_log("installed in " + dir);
            return;
        }
        if (kind == T_Install) {
            inst_fail(inst_same_version(inst.commit_had) ? "The game couldn't be repaired" : "The update couldn't be installed",
                      st == "rolled_back" ? "Your game is back to how it was. View details shows what happened."
                                          : "Something went wrong before it finished. View details shows what happened.", false);
            return;
        }
        inst_fail("The game couldn't be installed", "Something went wrong while building the game. View details shows what happened.", true);
    }
    void inst_fail(const mini::string &title, const mini::string &body, bool fresh) {
        if (modal.busy) close_modal(true);
        inst.page = IP_Failed, inst.want_primary = true;
        inst.end_title = title, inst.end_body = body;
        if (inst.end_log.empty()) inst.end_log = inst.log_path;
        inst.can_report = file_exists(join(join(dir, "tools"), "report.py")) && (file_exists(join(join(join(dir, "deps"), "python"), "python.exe"))
#ifndef _WIN32
                                                                               || true
#endif
                                                                               );
        inst.fresh = fresh && !inst.repair;
        if (inst.fresh) inst.end_body += " Try again picks up where it stopped. Close removes everything the installer put on your PC.";
        if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) { SDL_FlashWindow(win, SDL_FLASH_UNTIL_FOCUSED); notify_sound(false); }
        if (awake) { keep_awake(false); awake = false; }
        SDL_SetWindowProgressState(win, SDL_PROGRESS_STATE_ERROR);
        bar_state = SDL_PROGRESS_STATE_ERROR;
    }
    // the update / repair of an install already in the folder: the launcher's own update (backed up, put back on a failure)
    void inst_repair_start() {
        mini::string ups = join(dir, "updates");
        SDL_CreateDirectory(ups.c_str());
        inst.repair_zip = join(ups, "UltraInstinctTears-src-setup.zip");
        mini::string err, have_c;
        inst_parse_version(read_shared(join(dir, "VERSION.txt")), &inst.have_version, &inst.commit_had);
        bool ok = true;
        if (inst.used_newest && file_exists(inst.newest_zip) && inst.newest_zip == inst.repair_zip) inst_log("updating from the newest package, already in " + ups);
        else {
            SDL_IOStream *in = SDL_IOFromFile(inst.payload.c_str(), "rb");
            ok = in != nullptr;
            if (ok) {
                int64_t end = inst.pay_end < 0 ? SDL_GetIOSize(in) : inst.pay_end;
                mini::string data;
                data.resize((size_t)(end - inst.pay_start));
                ok = SDL_SeekIO(in, inst.pay_start, SDL_IO_SEEK_SET) == inst.pay_start && SDL_ReadIO(in, &data[0], data.size()) == data.size();
                SDL_CloseIO(in);
                ok = ok && inst_write(inst.repair_zip, data, &err);
            }
        }
        inst.newest_zip.clear();
        if (!ok) { inst_fail("The update couldn't be installed", "The installer's own files couldn't be read. Download the installer again.", false); return; }
        inst.prog = join(join(dir, "logs"), "update-progress.json");
        SDL_RemovePath(inst.prog.c_str());
        task_prog = inst.prog;
        bool same = inst_same_version(inst.commit_had);
        if (!run_helper(T_Install, "update.py", " install " + quote(inst.repair_zip) + " --force --progress " + quote(inst.prog),
                        same ? "Repairing Ultra Instinct Tears" : "Updating Ultra Instinct Tears", "Getting ready"))
            inst_fail("The update couldn't be installed", "It couldn't be started. Nothing in your game was changed.", false);
    }

    // ---------------------------------------------------------------- stopping, and removing what was put on the PC
    void inst_ask_cancel() {
        if (!inst.fresh || inst.repair || inst.cancelling) { flash("This closes by itself when it's done"); return; }
        inst.ask = 1, inst.ask_focus = 1;
    }
    void inst_cancel_now() {
        inst.ask = 0;
        inst.cancelling = true;
        inst_log("stopped by the player");
        if (inst.work) { inst.stop = 1; return; }       // the thread ends at its next file; inst_poll then removes
        if (task_kind == T_Setup) { stop_tree(&task); return; }   // poll_task sees it end: inst_task_done removes
        inst_remove(false);
    }
    struct RmCtx { App *a; mini::string folder; bool keep_folder; mini::vector<mini::string> parents; };
    RmCtx rm_ctx;   // the removal thread's own copy (set before it starts)
    static int SDLCALL inst_rm_thread(void *ud) {
        RmCtx *c = (RmCtx *)ud;
        App *a = c->a;
        bool ok = false;
        mini::string err;
        for (int tries = 0; tries < 10 && !ok; tries++) {   // files of the ended build can stay locked for a moment
            if (tries) SDL_Delay(500);
            ok = remove_tree(c->folder, &err);
            if (ok && c->keep_folder) SDL_CreateDirectory(c->folder.c_str());
        }
        if (ok && !c->keep_folder)
            for (auto &up : c->parents) if (inst_empty_dir(up)) SDL_RemovePath(up.c_str());   // made for the install, still empty
        SDL_LockMutex(a->inst.mu);
        a->inst.rm_ok = ok, a->inst.rm_err = err;
        SDL_UnlockMutex(a->inst.mu);
        SDL_SetAtomicInt(&a->inst.rm_done, 1);
        return 0;
    }
    // Only a folder carrying this installer's mark, never a drive, never one holding the game files.
    bool inst_removable() const {
        return inst.fresh && !inst.repair && !inst.folder.empty() && !inst_is_root(inst.folder) && file_exists(join(inst.folder, kInstMark)) &&
               (inst.game.empty() || !inst_inside(inst.game, inst.folder));
    }
    void inst_remove(bool quit_after) {
        if (modal.busy) close_modal(true);
        task_kind = T_None;
        inst.quit_after_rm = quit_after;
        // a copy of the log first: View details still works once the folder is gone
        mini::string pref = inst_pref();
        mini::string src = !inst.end_log.empty() && file_exists(inst.end_log) ? inst.end_log : inst.log_path;
        if (!pref.empty() && file_exists(src)) {
            SDL_CreateDirectory(pref.c_str());
            mini::string copy = join(pref, "last-install.log");
            mini::string body = read_shared(inst.log_path);
            if (src != inst.log_path) body += "\n--- " + src + "\n" + read_shared(src);
            mini::string e;
            if (inst_write(copy, body, &e)) inst.end_log = copy;
        }
        if (!inst_removable()) { inst_rm_done_page(true, ""); return; }
        inst.removing = true;
        inst.page = inst.cancelling ? IP_Stopped : IP_Failed; inst.want_primary = true;
        SDL_SetAtomicInt(&inst.rm_done, 0);
        rm_ctx.a = this, rm_ctx.folder = inst.folder, rm_ctx.keep_folder = !inst.made_folder, rm_ctx.parents = inst.made_parents;
        RmCtx *c = &rm_ctx;
        dir = inst_parent(inst.payload.empty() ? self_exe() : inst.payload);   // nothing of ours stays open in the folder
        inst.rm = SDL_CreateThread(inst_rm_thread, "uit-remove", c);
        if (!inst.rm) { inst_rm_done_page(false, SDL_GetError()); }
    }
    void inst_rm_finish() {
        SDL_WaitThread(inst.rm, nullptr);
        inst.rm = nullptr;
        inst_rm_done_page(inst.rm_ok, inst.rm_err);
    }
    void inst_rm_done_page(bool ok, const mini::string &err) {
        inst.removing = false;
        if (inst.quit_after_rm && ok) { quit = true; return; }
        inst.page = IP_Stopped, inst.want_primary = true;
        inst.end_title = inst.cancelling ? "Installation stopped" : "Installation removed";
        inst.end_body = ok ? "Nothing was left on your PC. Your game files weren't touched."
                           : "Some files couldn't be removed (" + err + "). You can delete the folder " + inst.folder + " yourself.";
        if (ok) inst.fresh = true, inst.made_folder = false, inst.retried = false;
        SDL_SetWindowProgressState(win, SDL_PROGRESS_STATE_NONE);
        bar_state = SDL_PROGRESS_STATE_NONE;
        if (awake) { keep_awake(false); awake = false; }
    }

    // ---------------------------------------------------------------- the end: Play, the launcher, the shortcut
    mini::string inst_launcher() const { return join(inst.folder, "UltraInstinctTearsLauncher.exe"); }
    void inst_shortcuts() {
        mini::string target = inst_launcher(), err;
        if (!file_exists(target)) return;
        mini::string start = start_menu_folder();
        if (!start.empty()) make_shortcut(join(start, "Ultra Instinct Tears.lnk"), target, inst.folder, "Ultra Instinct Tears", &err);
        if (inst.shortcut) {
            mini::string desk = desktop_folder();
            if (!desk.empty() && !make_shortcut(join(desk, "Ultra Instinct Tears.lnk"), target, inst.folder, "Ultra Instinct Tears", &err))
                inst_log("the desktop shortcut couldn't be made: " + err);
        }
    }
    void inst_finish(int how) {   // 0 close, 1 open the launcher, 2 play
        inst_shortcuts();
        if (how) {
            mini::string err;
            if (!file_exists(inst_launcher())) { message("The launcher isn't there", "UltraInstinctTearsLauncher.exe wasn't found in " + inst.folder + "."); return; }
            if (!start_detached(inst_launcher(), how == 2 ? "--play" : "", inst.folder, &err)) { message("The launcher couldn't be started", err); return; }
            inst.started = how == 2 ? "play" : "launcher";
        }
        quit = true;
    }
    void inst_request_quit() {
        if (inst.page == IP_Work && (inst.work || task_kind != T_None)) {
            if (inst.fresh && !inst.repair) inst_ask_cancel();
            else request_quit_busy();
            return;
        }
        if (inst.removing) return;
        if (inst.page == IP_Failed && inst.fresh && inst_removable()) { inst_remove(true); return; }
        if (inst.page == IP_Done) inst_shortcuts();
        quit = true;
    }
    void request_quit_busy() {   // an update / repair over an install: as the launcher (it is backed up and undone on a failure)
        if (now - quit_ask > 6 && cmds.empty()) { quit_ask = now; flash("Still working. Close again to close the installer anyway"); return; }
        quit = true;
    }

    // ---------------------------------------------------------------- choosing folders: the dialog, typing, dropping
    enum { kInstSetFolder = 0, kInstSetGame = 1 };
    static constexpr const char *kInstKeys[2] = {"UIT_SETUP_FOLDER", "UIT_SETUP_GAME"};
    void inst_settings() {   // two text "settings" so the launcher's own typing dialog (and its on-screen keyboard) can be used
        const char *names[2] = {"Install folder", "Game files folder"};
        const char *hints[2] = {"Type the folder to install into, for example C:\\Games\\UltraInstinctTears",
                                "Type the folder that holds your exefs and romfs folders"};
        for (int k = 0; k < 2; k++) {
            Setting st;
            st.key = kInstKeys[k], st.name = names[k], st.type = Type::Text, st.hint = hints[k], st.applies = Applies::Launcher;
            S.push_back(st);
            by_key[st.key] = (int)S.size() - 1;
        }
    }
    void inst_type(int which) {
        int i = idx(kInstKeys[which]);
        if (i < 0) return;
        cfg.set(S[i].key, mini::optional<mini::string>(which == kInstSetFolder ? inst.folder : inst.game));
        (which == kInstSetFolder ? inst.typed_folder : inst.typed_game) = which == kInstSetFolder ? inst.folder : inst.game;
        edit(i);
    }
    void inst_typed_poll() {   // a typed path is applied once the dialog closes with Save
        if (modal.kind != Modal::None) return;
        for (int k = 0; k < 2; k++) {
            auto v = cfg.get(kInstKeys[k]);
            mini::string t = v ? *v : mini::string();
            mini::string &last = k == kInstSetFolder ? inst.typed_folder : inst.typed_game;
            if (t == last) continue;
            last = t;
            if (k == kInstSetFolder) inst_set_folder(t); else inst_set_game(t);
        }
    }
    void inst_browse(int which) {
        if (!pick_mu) pick_mu = SDL_CreateMutex();
        pick_kind = which == kInstSetFolder ? P_InstFolder : P_InstGame;
        mini::string at = which == kInstSetFolder ? inst_parent(inst.folder) : inst.game;
        if (!at.empty() && !is_dir(at)) at.clear();
        SDL_ShowOpenFolderDialog(picked_cb, this, win, at.empty() ? nullptr : at.c_str(), false);
    }
    void inst_picked(PickKind k, const mini::string &f) {
        if (k == P_InstFolder) inst_set_folder(f);
        else inst_set_game(f);
    }
    bool inst_drop(const mini::vector<mini::string> &paths) {   // a folder dropped on the window: the page's own folder
        if (paths.empty()) return false;
        if (inst.page == IP_Folder) { inst_set_folder(is_dir(paths[0]) ? paths[0] : inst_parent(paths[0])); return true; }
        if (inst.page == IP_Game) { inst_set_game(paths[0]); return true; }
        flash("Drop a folder on the Install folder or Game files page");
        return true;
    }

    // ---------------------------------------------------------------- moving between the pages
    void inst_go(InstPage p) {
        inst.page = p;
        inst.want_primary = true;
        if (p == IP_Folder && inst.folder.empty()) inst_set_folder(inst.remembered.empty() ? inst_default_folder() : inst.remembered);
        if (p == IP_Folder && !inst.folder.empty()) inst_check_folder();
        if (p == IP_Ready) { GpuInfo g = gpu; inst.nvidia = g.nvidia; }
    }
    void inst_back() {
        switch (inst.page) {
        case IP_Folder: inst_go(IP_Welcome); break;
        case IP_Game: inst_go(IP_Folder); break;
        case IP_Ready: inst_go(inst.repair ? IP_Folder : IP_Game); break;
        default: break;
        }
    }
    void inst_next() {
        switch (inst.page) {
        case IP_Welcome: inst_go(IP_Folder); break;
        case IP_Folder:
            if (inst.folder_ok != 1) { flash(inst.folder_note); break; }
            inst_go(inst.repair ? IP_Ready : IP_Game);
            break;
        case IP_Game:
            if (inst.game_ok == 2) { flash("Still checking your game files"); break; }
            if (inst.game_ok != 1) { flash(inst.game.empty() ? mini::string("Choose the folder with your game files first") : inst.game_note); break; }
            inst_go(IP_Ready);
            break;
        case IP_Ready: inst_start(); break;
        default: break;
        }
    }
    void inst_use_remembered(bool open_launcher) {
        inst_set_folder(inst.remembered);
        if (open_launcher) {
            mini::string err;
            if (!start_detached(inst_launcher(), "", inst.folder, &err)) { message("The launcher couldn't be started", err); return; }
            inst.started = "launcher";
            quit = true;
            return;
        }
        inst_go(IP_Ready);
    }
    void inst_try_again() {
        inst.retried = true;
        inst.end_log.clear();
        if (inst.repair) inst_check_folder();
        inst_start();
    }
    void inst_view_details() {
        mini::string l = !inst.end_log.empty() && file_exists(inst.end_log) ? inst.end_log : inst.log_path;
        if (file_exists(l)) open_path(l); else flash("There are no details to show");
    }

    // ---------------------------------------------------------------- input
    bool inst_handle(const Action &a) {   // true: used here (the installer's pages and its stop question)
        if (inst.ask) {
            switch (a.a) {
            case A_Left: case A_Up: inst.ask_focus = 0; break;
            case A_Right: case A_Down: inst.ask_focus = 1; break;
            case A_Accept: if (inst.ask_focus == 0) inst_cancel_now(); else inst.ask = 0; break;
            case A_Back: inst.ask = 0; break;
            default: break;
            }
            return true;
        }
        if (modal.kind != Modal::None) return false;
        int n = (int)inst.ctls.size();
        switch (a.a) {
        case A_Up: case A_Left: if (n) inst.focus = mini::max(0, inst.focus - 1); break;
        case A_Down: case A_Right: if (n) inst.focus = mini::min(n - 1, inst.focus + 1); break;
        case A_Home: inst.focus = 0; break;
        case A_End: inst.focus = mini::max(0, n - 1); break;
        case A_Accept:
            if (inst.focus >= 0 && inst.focus < n && inst.ctls[inst.focus].fn) { auto fn = inst.ctls[inst.focus].fn; fn(); }
            break;
        case A_Back:
            if (inst.page == IP_Welcome) flash("Close the window to leave the installer");
            else inst_back();
            break;
        case A_Play: if (inst.page == IP_Done) inst_finish(2); break;
        default: break;
        }
        return true;
    }

    // ---------------------------------------------------------------- drawing
    void inst_top_bar(float W) {
        float h = d.px(76), cy = h / 2;
        d.rect({0, 0}, {W, h}, pal::bg);
        d.rect({0, h - 1}, {W, h}, pal::line);
        float x = d.px(40), fs = d.fs(15), sp = d.px(3.2f);
        ImVec2 ws = d.measure(d.semi, fs, "U");
        if (logo) {
            float ls = d.px(34);
            d.dl->AddImage((ImTextureID)(intptr_t)logo, {x, cy - ls / 2}, {x + ls, cy + ls / 2});
            x += ls + d.px(14);
        }
        d.text_spaced(d.semi, fs, {x, cy - ws.y / 2}, pal::text, "ULTRA INSTINCT TEARS", sp);
        x += d.spaced_width(d.semi, fs, "ULTRA INSTINCT TEARS", sp) + d.px(12);
        d.rect({x, cy - d.px(8)}, {x + 1, cy + d.px(8)}, pal::line);
        d.text(d.reg, fs, {x + d.px(12), cy - ws.y / 2}, pal::dim, "Setup");
        // where the player is: the steps as the launcher's tabs, not clickable
        mini::vector<mini::string> steps = {"Welcome", "Install folder"};
        if (!inst.repair) steps.push_back("Game files");
        steps.push_back(inst.repair ? "Update" : "Install");
        int cur = inst.page == IP_Welcome ? 0 : inst.page == IP_Folder ? 1 : inst.page == IP_Game ? 2 : (int)steps.size() - 1;
        if (inst.repair && inst.page == IP_Ready) cur = 2;
        float tfs = d.fs(15), tpad = d.px(16), total = 0;
        for (auto &t : steps) total += d.measure(d.semi, tfs, t).x + tpad * 2;
        float tx = W - d.px(40) - total;
        ImVec2 ts = d.measure(d.semi, tfs, "Ag");
        for (int k = 0; k < (int)steps.size(); k++) {
            float w = d.measure(d.semi, tfs, steps[k]).x + tpad * 2;
            ImU32 c = k == cur ? pal::text : k < cur ? pal::body : pal::faint;
            if (k < cur) d.check({tx + tpad - d.px(10), cy}, d.px(8), pal::teal);
            d.text(d.semi, tfs, {tx + tpad + (k < cur ? d.px(4) : 0), cy - ts.y / 2}, c, steps[k]);
            if (k == cur) d.rect({tx + tpad, h - d.px(3)}, {tx + w - tpad, h - 1}, pal::teal, d.px(1));
            tx += w;
        }
    }
    mini::vector<Hint> inst_hints() {
        if (modal.kind != Modal::None || inst.ask) return inst.ask ? mini::vector<Hint>{{G_Change, "Choose", A_Right}, {G_Accept, "Select", A_Accept}, {G_Back, "Keep installing", A_Back}} : hints();
        mini::vector<Hint> h = {{G_Move, "Move", A_Down}, {G_Accept, "Select", A_Accept}};
        if (inst.page == IP_Folder || inst.page == IP_Game || inst.page == IP_Ready) h.push_back({G_Back, "Back", A_Back});
        if (inst.page == IP_Done) h.push_back({G_Play, "Play", A_Play});
        return h;
    }
    void inst_footer(float W, float H) {
        float h = d.px(58), y = H - h, cy = y + h / 2, x = d.px(40), fs = d.fs(14);
        d.rect({0, y}, {W, H}, pal::bg);
        d.rect({0, y}, {W, y + 1}, pal::line);
        ImVec2 ts = d.measure(d.reg, fs, "Ag");
        mini::string msg = "Your game files are never uploaded or changed.";
        ImU32 col = pal::faint;
        if (now - toast_at < 3.5 && !toast.empty()) msg = toast, col = pal::body;
        else if (!inst.version.empty()) msg = "Version " + inst.version + "  \xC2\xB7  " + msg;
        auto hs = inst_hints();
        float hint_w = 0;
        for (auto &k : hs) hint_w += glyph(k.g, 0, 0, false) + d.px(8) + d.measure(d.reg, fs, k.label).x + d.px(24);
        d.text_fit(d.reg, fs, {x, cy - ts.y / 2}, col, msg, W - hint_w - x - d.px(60));
        float hx = W - d.px(40) - hint_w + d.px(24);
        for (auto &k : hs) {
            float x0 = hx;
            hx += glyph(k.g, hx, cy) + d.px(8);
            float lw = d.measure(d.reg, fs, k.label).x;
            bool hov = hovered({x0, y}, {hx + lw, H});
            d.text(d.reg, fs, {hx, cy - ts.y / 2}, hov ? pal::text : pal::dim, k.label);
            Act act = k.act;
            hit({x0 - d.px(6), y}, {hx + lw + d.px(6), H}, -1, [this, act] {
                if (act == A_Right || act == A_Down) return;   // "Move" / "Choose" are not buttons
                Action a{act};
                handle(a);
            });
            hx += lw + d.px(24);
        }
    }
    // A button; style 0 plain (teal when focused, as the launcher's dialogs), 1 the main one (gold, as Play). Returns its width.
    float inst_button(ImVec2 a, float h, const mini::string &t, int style, mini::function<void()> fn, bool primary = false, bool enabled = true) {
        int id = (int)inst.ctls.size();
        inst.ctls.push_back({fn, primary, t});
        bool foc = inst.focus == id && modal.kind == Modal::None && !inst.ask;
        ImVec2 tsz = d.measure(d.semi, d.fs(16), t);
        float w = mini::max(d.px(132), tsz.x + d.px(52));
        ImVec2 b = {a.x + w, a.y + h};
        bool hov = hovered(a, b) && modal.kind == Modal::None && !inst.ask;
        if (hov && mouse_moved) inst.focus = id;
        ImU32 fill, ink;
        if (style == 1) {
            fill = enabled ? (hov ? mix(pal::gold, pal::text, 0.12f) : pal::gold) : mix(pal::gold, pal::panel, 0.6f);
            ink = pal::ink;
            if (foc) d.outline({a.x - d.px(4), a.y - d.px(4)}, {b.x + d.px(4), b.y + d.px(4)}, pal::teal, d.px(12), d.px(2));
        } else {
            fill = foc ? pal::teal : hov ? pal::focus : pal::raised;
            ink = foc ? pal::ink : enabled ? pal::body : pal::faint;
            if (hov && !foc) d.outline(a, b, alpha(pal::teal, 0.6f), d.px(8));
        }
        d.rect(a, b, fill, d.px(style == 1 ? 10 : 8));
        d.text(d.semi, d.fs(16), {a.x + (w - tsz.x) / 2, a.y + (h - tsz.y) / 2}, ink, t);
        if (modal.kind == Modal::None && !inst.ask) hit(a, b, -1, [this, id] { inst.focus = id; if (inst.ctls.size() > (size_t)id && inst.ctls[id].fn) { auto f = inst.ctls[id].fn; f(); } });
        return w;
    }
    // buttons in a row, right-aligned at x1 (left to right as given); the first one with primary set is focused on a new page
    struct IBtn { mini::string label; int style; mini::function<void()> fn; bool primary; bool enabled; };
    void inst_buttons(float x1, float y, const mini::vector<IBtn> &bs, float x0 = -1) {
        float h = d.px(52), total = 0;
        for (auto &b : bs) total += mini::max(d.px(132), d.measure(d.semi, d.fs(16), b.label).x + d.px(52)) + d.px(12);
        float x = x0 >= 0 ? x0 : x1 - total + d.px(12);
        for (auto &b : bs) x += inst_button({x, y}, h, b.label, b.style, b.fn, b.primary, b.enabled) + d.px(12);
    }
    // one status line with its icon: 1 ok (teal check), -1 a problem (gold), 2 working (a turning arc), 0 neutral
    float inst_status(float x, float y, float w, int state, const mini::string &t) {
        if (t.empty()) return 0;
        inst.shown += " | " + t;
        float ic = d.px(11), cy = y + d.px(11);
        ImVec2 c = {x + ic, cy};
        if (state == 1) { d.dl->AddCircleFilled(c, ic, alpha(pal::teal, 0.22f), 20); d.check(c, d.px(11), pal::teal); }
        else if (state == -1) { d.dl->AddCircleFilled(c, ic, pal::goldSoft, 20); d.cross(c, d.px(8), pal::gold); }
        else if (state == 2) {
            float a0 = snap ? 0.0f : (float)mini::fmod(now * 5.0, 6.2831853);
            d.dl->PathArcTo(c, ic - d.px(1.5f), a0, a0 + 4.2f, 24);
            d.dl->PathStroke(pal::teal, 0, d.px(2.2f));
        } else d.dl->AddCircle(c, d.px(5), pal::faint, 16, d.px(1.5f));
        float th = d.text(d.reg, d.fs(16), {x + ic * 2 + d.px(12), y}, state == -1 ? pal::gold : state == 1 ? pal::body : pal::dim, t, w - ic * 2 - d.px(12));
        return mini::max(th, ic * 2) + d.px(6);
    }
    // the folder box: a caption, the path, Browse and Type a path
    float inst_path_box(float x, float y, float w, const char *caption, const mini::string &path, const char *empty, int which) {
        float h = d.px(92);
        inst.shown += mini::string(" | ") + caption + ": " + (path.empty() ? mini::string(empty) : path);
        d.rect({x, y}, {x + w, y + h}, pal::panel, d.px(12));
        d.outline({x, y}, {x + w, y + h}, pal::line, d.px(12));
        float bw = 0;
        mini::string b1 = "Browse\xE2\x80\xA6", b2 = "Type a path";
        for (auto &t : {b1, b2}) bw += mini::max(d.px(132), d.measure(d.semi, d.fs(16), t).x + d.px(52)) + d.px(12);
        float tx = x + d.px(24), tw = w - d.px(48) - bw;
        d.text_spaced(d.semi, d.fs(12), {tx, y + d.px(20)}, pal::faint, caption, d.px(1.6f));
        d.text_fit(path.empty() ? d.reg : d.semi, d.fs(18), {tx, y + d.px(44)}, path.empty() ? pal::faint : pal::text, path.empty() ? mini::string(empty) : path, tw, true);
        float bx = x + w - d.px(24) - bw + d.px(12), by = y + (h - d.px(52)) / 2;
        bx += inst_button({bx, by}, d.px(52), b1, 0, [this, which] { inst_browse(which); }) + d.px(12);
        inst_button({bx, by}, d.px(52), b2, 0, [this, which] { inst_type(which); });
        return h;
    }
    float inst_row(float x, float y, float w, const mini::string &name, const mini::string &value, bool fit_end = true) {
        float rh = d.px(46), cy = y + rh / 2;
        inst.shown += " | " + name + ": " + value;
        ImVec2 ts = d.measure(d.reg, d.fs(16), "Ag");
        d.text(d.reg, d.fs(16), {x, cy - ts.y / 2}, pal::dim, name);
        float nw = d.measure(d.reg, d.fs(16), name).x + d.px(40);
        ImVec2 vs = d.measure(d.semi, d.fs(16), value);
        float vw = mini::min(vs.x, w - nw);
        d.text_fit(d.semi, d.fs(16), {x + w - vw, cy - ts.y / 2}, pal::text, value, vw, fit_end);
        d.rect({x, y + rh - 1}, {x + w, y + rh}, alpha(pal::line, 0.7f));
        return rh;
    }
    float inst_toggle(float x, float y, float w, const mini::string &name, const mini::string &sub, bool *on) {
        float rh = sub.empty() ? d.px(56) : d.px(70), cy = y + rh / 2;
        int id = (int)inst.ctls.size();
        inst.ctls.push_back({[on] { *on = !*on; }, false, name});
        bool foc = inst.focus == id && modal.kind == Modal::None && !inst.ask;
        ImVec2 a = {x - d.px(16), y}, b = {x + w + d.px(16), y + rh};
        bool hov = hovered(a, b) && modal.kind == Modal::None && !inst.ask;
        if (hov && mouse_moved) inst.focus = id;
        d.rect(a, b, foc ? pal::focus : hov ? mix(pal::panel, pal::focus, 0.5f) : pal::panel, d.px(10));
        if (foc) d.rect({a.x, a.y + d.px(14)}, {a.x + d.px(3), b.y - d.px(14)}, pal::teal, d.px(1.5f));
        float th = d.measure(d.semi, d.fs(17), "Ag").y, sh = sub.empty() ? 0 : d.measure(d.reg, d.fs(14), "Ag").y + d.px(3);
        float ty = cy - (th + sh) / 2;
        d.text(d.semi, d.fs(17), {x + d.px(8), ty}, foc ? pal::text : pal::body, name);
        if (!sub.empty()) d.text_fit(d.reg, d.fs(14), {x + d.px(8), ty + th + d.px(3)}, pal::dim, sub, w - d.px(100));
        float sw = d.px(46), shh = d.px(26), sx = x + w - sw - d.px(4);
        d.rect({sx, cy - shh / 2}, {sx + sw, cy + shh / 2}, *on ? pal::teal : pal::raised, shh / 2);
        float kx = *on ? sx + sw - shh / 2 : sx + shh / 2;
        d.dl->AddCircleFilled({kx, cy}, shh / 2 - d.px(3), *on ? pal::ink : pal::dim, 20);
        if (modal.kind == Modal::None && !inst.ask) hit(a, b, -1, [this, id, on] { inst.focus = id; *on = !*on; });
        return rh;
    }

    // the pages
    void inst_page(float W, float H) {
        float top = d.px(76), bottom = H - d.px(58);
        float cw = mini::min(W - d.px(120), d.px(820)), x = (W - cw) / 2;
        float y = top + mini::max(d.px(36), (bottom - top) * 0.08f);
        float by = bottom - d.px(40) - d.px(52);   // the page's buttons
        mini::string title, body;
        auto head = [&](const mini::string &t, const mini::string &b) {
            title = t, body = b;
            y += d.text(d.semi, d.fs(36), {x, y}, pal::text, t, cw) + d.px(14);
            if (!b.empty()) y += d.text(d.reg, d.fs(18), {x, y}, pal::dim, b, cw) + d.px(30);
        };
        switch (inst.page) {
        case IP_Welcome: {
            if (!inst.remembered.empty()) {
                mini::string v, c;
                inst_parse_version(read_shared(join(inst.remembered, "VERSION.txt")), &v, &c);
                bool same = inst_same_version(c);
                head("Welcome back", "Ultra Instinct Tears is already installed in " + inst.remembered + (v.empty() ? mini::string() : " (version " + v + ")") + ". " +
                     (same ? "This installer has the same version. It can repair it, keeping your saves and settings."
                           : "This installer has version " + inst.version + ". It can update it, keeping your saves and settings."));
                inst_buttons(x + cw, by, {{"Install somewhere else", 0, [this] { inst.remembered.clear(); inst.folder.clear(); inst_go(IP_Folder); }, false, true},
                                          {"Open the launcher", 0, [this] { inst_use_remembered(true); }, false, true},
                                          {same ? "Repair" : "Update", 1, [this] { inst_use_remembered(false); }, true, true}});
                break;
            }
            head("Welcome", "This sets up Ultra Instinct Tears, a PC version of The Legend of Zelda: Tears of the Kingdom (version 1.4.3) that's "
                            "built on your PC from your own copy of the game.");
            d.text_spaced(d.semi, d.fs(12), {x, y}, pal::faint, "YOU'LL NEED", d.px(1.6f));
            y += d.px(30);
            int64_t ram = (int64_t)SDL_GetSystemRAM();
            y += inst_status(x, y, cw, 0, "Your own game files: the exefs and romfs folders of Tears of the Kingdom, dumped from your Switch with update 1.4.3.") + d.px(8);
            y += inst_status(x, y, cw, 0, "About " + gb(kInstNeed) + " of free space.") + d.px(8);
            y += inst_status(x, y, cw, 0, "An internet connection for the first part (about 150 MB).") + d.px(8);
            if (ram > 0 && ram < 15000) y += inst_status(x, y, cw, -1, "This PC has " + num((ram + 512) / 1024) + " GB of memory. 16 GB or more is recommended.") + d.px(8);
            y += d.px(18);
            d.text(d.reg, d.fs(15), {x, y}, pal::faint, "No game files are downloaded or shared. Everything made from your game stays on this PC.", cw);
            inst_buttons(x + cw, by, {{"Quit", 0, [this] { quit = true; }, false, true}, {"Get started", 1, [this] { inst_next(); }, true, true}});
            break;
        }
        case IP_Folder: {
            head("Where should it go?", "Pick a folder for the game. It needs about " + gb(kInstNeed) + " of space.");
            y += inst_path_box(x, y, cw, "INSTALL FOLDER", inst.folder, "No folder chosen yet", kInstSetFolder) + d.px(18);
            y += inst_status(x + d.px(4), y, cw - d.px(8), inst.folder_ok, inst.folder_note);
            inst_buttons(x + cw, by, {{"Back", 0, [this] { inst_back(); }, false, true}, {"Next", 1, [this] { inst_next(); }, true, inst.folder_ok == 1}});
            break;
        }
        case IP_Game: {
            head("Where are your game files?", "Choose the folder that holds the exefs and romfs folders you dumped from your own Switch, "
                                               "with update 1.4.3 applied.");
            y += inst_path_box(x, y, cw, "GAME FILES", inst.game, "No folder chosen yet", kInstSetGame) + d.px(18);
            y += inst_status(x + d.px(4), y, cw - d.px(8), inst.game.empty() ? 0 : inst.game_ok,
                             inst.game.empty() ? mini::string("Your game files stay where they are. Nothing is changed or uploaded.") : inst.game_note);
            inst_buttons(x + cw, by, {{"Back", 0, [this] { inst_back(); }, false, true}, {"Next", 1, [this] { inst_next(); }, true, inst.game_ok == 1}});
            break;
        }
        case IP_Ready: {
            bool same = inst.repair && inst_same_version(read_commit(inst.folder));
            head(inst.repair ? (same ? "Ready to repair" : "Ready to update") : "Ready to install",
                 inst.repair ? "Your saves and settings are kept, and the game is put back as it was if anything goes wrong."
                             : "Building the game takes a while. You can keep using your PC while it works.");
            y += inst_row(x, y, cw, "Install folder", inst.folder);
            if (!inst.repair) y += inst_row(x, y, cw, "Game files", inst.game);
            if (!inst.repair) y += inst_row(x, y, cw, "Space needed", "About " + gb(kInstNeed) + (inst.free >= 0 ? " (" + gb(inst.free) + " free)" : mini::string()));
            else y += inst_row(x, y, cw, "Version", (inst.have_version.empty() ? mini::string("?") : inst.have_version) + "  \xE2\x86\x92  " + inst.version, false);
            y += d.px(20);
            if (inst.nvidia && !inst.repair)
                y += inst_toggle(x + d.px(16), y, cw - d.px(32), "Also get DLSS for your NVIDIA card",
                                 "Downloads about 280 MB from NVIDIA. NVIDIA's license applies.", &inst.dlss) + d.px(10);
            inst_buttons(x + cw, by, {{"Back", 0, [this] { inst_back(); }, false, true},
                                      {inst.repair ? (same ? "Repair" : "Update") : "Install", 1, [this] { inst_next(); }, true, true}});
            break;
        }
        case IP_Work: {
            head(inst.repair ? "Updating" : "Installing", "");
            break;
        }
        case IP_Done: {
            float r = d.px(30);
            ImVec2 c = {x + r, y + r};
            d.dl->AddCircleFilled(c, r, alpha(pal::teal, 0.2f), 40);
            d.check(c, d.px(28), pal::teal);
            y += r * 2 + d.px(26);
            head(inst.repair ? "Ultra Instinct Tears is up to date" : "Ultra Instinct Tears is installed",
                 "It's in " + inst.folder + ". " + (inst.repair ? "Your saves and settings were kept. " : "Your game files weren't changed. ") +
                 "You can change settings any time in the launcher.");
            y += inst_toggle(x + d.px(16), y, cw - d.px(32), "Add a shortcut to the desktop", "", &inst.shortcut);
            inst_buttons(x + cw, by, {{"Close", 0, [this] { inst_finish(0); }, false, true}, {"Open launcher", 0, [this] { inst_finish(1); }, false, true},
                                      {"Play", 1, [this] { inst_finish(2); }, true, true}});
            break;
        }
        case IP_Failed: case IP_Stopped: {
            if (inst.removing) {
                head(inst.cancelling ? "Stopping" : "Removing", "");
                y += inst_status(x, y, cw, 2, "Removing what the installer put on your PC\xE2\x80\xA6");
                break;
            }
            head(inst.end_title, inst.end_body);
            mini::vector<IBtn> bs;
            if (inst.page == IP_Failed) {
                bs.push_back({"Close", 0, [this] { inst_request_quit(); }, false, true});
                bs.push_back({"View details", 0, [this] { inst_view_details(); }, false, true});
                if (inst.can_report) bs.push_back({side_state == 1 ? "Making a report\xE2\x80\xA6" : side_state == 2 ? "Show the report" : "Report a problem", 0, [this] {
                    if (side_state == 2) { mini::string z = side_zip; open_path(inst_parent(z)); } else side_report_start();
                }, false, true});
                bs.push_back({"Try again", 1, [this] { inst_try_again(); }, true, true});
            } else {
                bs.push_back({"Close", 0, [this] { quit = true; }, false, true});
                if (file_exists(inst.end_log)) bs.push_back({"View details", 0, [this] { inst_view_details(); }, false, true});
                bs.push_back({"Start over", 1, [this] { inst.cancelling = false; inst_go(IP_Welcome); }, true, true});
            }
            inst_buttons(x + cw, by, bs);
            break;
        }
        }
        inst.cmd_note = title + " | " + body + inst.shown;
    }
    static mini::string read_commit(const mini::string &folder) {
        mini::string v, c;
        inst_parse_version(read_shared(join(folder, "VERSION.txt")), &v, &c);
        return c;
    }
    void inst_ask_draw(float W, float H) {   // "Stop installing?" over the install screen
        d.rect({0, 0}, {W, H}, alpha(IM_COL32(6, 9, 10, 255), 0.6f));
        float w = d.px(600), pad = d.px(36), iw = w - pad * 2;
        mini::string t = "Stop installing?", b = "Everything the installer has put on your PC so far will be removed. Your game files aren't touched.";
        float h = pad + d.measure(d.semi, d.fs(24), t, iw).y + d.px(14) + d.measure(d.reg, d.fs(16), b, iw).y + d.px(28) + d.px(50) + pad;
        float x0 = (W - w) / 2, y0 = (H - h) / 2;
        d.rect({x0, y0}, {x0 + w, y0 + h}, pal::panel, d.px(14));
        d.outline({x0, y0}, {x0 + w, y0 + h}, pal::line, d.px(14));
        float y = y0 + pad;
        y += d.text(d.semi, d.fs(24), {x0 + pad, y}, pal::text, t, iw) + d.px(14);
        d.text(d.reg, d.fs(16), {x0 + pad, y}, pal::body, b, iw);
        const char *labels[2] = {"Stop and remove", "Keep installing"};
        float bx = x0 + w - pad, bh = d.px(50);
        float ws[2];
        for (int k = 0; k < 2; k++) ws[k] = mini::max(d.px(132), d.measure(d.semi, d.fs(16), labels[k]).x + d.px(48));
        bx -= ws[0] + ws[1] + d.px(10);
        for (int k = 0; k < 2; k++) {
            ImVec2 a = {bx, y0 + h - pad - bh}, bb = {bx + ws[k], y0 + h - pad};
            bool foc = inst.ask_focus == k, hov = hovered(a, bb);
            if (hov && mouse_moved) inst.ask_focus = k;
            d.rect(a, bb, foc ? pal::teal : hov ? pal::focus : pal::raised, d.px(8));
            ImVec2 ts = d.measure(d.semi, d.fs(16), labels[k]);
            d.text(d.semi, d.fs(16), {a.x + (ws[k] - ts.x) / 2, a.y + (bh - ts.y) / 2}, foc ? pal::ink : k == 0 ? pal::gold : pal::body, labels[k]);
            hit(a, bb, -1, [this, k] { if (k == 0) inst_cancel_now(); else inst.ask = 0; });
            bx += ws[k] + d.px(10);
        }
    }
    void inst_frame(float W, float H) {
        inst_typed_poll();
        inst.ctls.clear();
        inst.shown.clear();
        d.rect({0, 0}, {W, H}, pal::bg);
        inst_page(W, H);
        if (inst.want_primary) {
            for (int k = 0; k < (int)inst.ctls.size(); k++) if (inst.ctls[k].primary) { inst.focus = k; break; }
            inst.want_primary = false;
        }
        if (inst.focus >= (int)inst.ctls.size()) inst.focus = mini::max(0, (int)inst.ctls.size() - 1);
        inst_top_bar(W);
        inst_footer(W, H);
        if (modal.kind != Modal::None) draw_modal(W, H);
        if (inst.ask) inst_ask_draw(W, H);
        mouse_moved = false;
    }

    // ---------------------------------------------------------------- scripted runs (tools/tests/test_installer.py)
    // ipage:NAME (welcome folder game ready work done failed stopped), ihas:TEXT (the page's title, text and notes contain
    // TEXT), inot:TEXT, ifolder:PATH / igame:PATH (as if chosen; igame waits for the check), inext, iback, ipress:LABEL
    // (the page's button with that label), iwait:S (until the install has ended, S seconds at most), isleep:S (S seconds
    // of real time with the install running), icancel (the stop question), istarted:WHAT (play / launcher / none),
    // iexists:PATH / igone:PATH, ifocus:LABEL (the focused button)
    static const char *inst_page_name(InstPage p) {
        static const char *n[] = {"welcome", "folder", "game", "ready", "work", "done", "failed", "stopped"};
        return n[p];
    }
    void inst_pump(double secs, bool until_end) {
        double until = SDL_GetTicksNS() / 1e9 + secs;
        while (SDL_GetTicksNS() / 1e9 < until) {
            if (until_end && inst.page != IP_Work && !inst.removing && !inst.chk && !inst.work && task_kind == T_None) break;
            SDL_Delay(20);
            double t = SDL_GetTicksNS() / 1e9;
            dt = t - now, now = t;
            inst_poll();
            poll_task();
        }
    }
    bool inst_script(const mini::string &c, const mini::string &arg) {
        auto ok = [&](bool good, const mini::string &have) {
            SDL_Log("script: check %s %s %s (have %s)", c.c_str(), arg.c_str(), good ? "ok" : "FAILED", have.c_str());
        };
        if (c == "ipage") ok(arg == inst_page_name(inst.page), inst_page_name(inst.page));
        else if (c == "ihas") ok(inst.cmd_note.find(arg) != mini::string::npos, inst.cmd_note);
        else if (c == "inot") ok(inst.cmd_note.find(arg) == mini::string::npos, inst.cmd_note);
        else if (c == "ifolder") inst_set_folder(arg);
        else if (c == "igame") { inst_set_game(arg); for (int k = 0; k < 6000 && inst.chk; k++) { SDL_Delay(10); inst_check_poll(); } }
        else if (c == "inext") inst_next();
        else if (c == "iback") inst_back();
        else if (c == "ipress") {
            bool found = false;
            for (int k = 0; k < (int)inst.ctls.size(); k++)
                if (inst.ctls[k].label == arg && inst.ctls[k].fn) { inst.focus = k; auto f = inst.ctls[k].fn; f(); found = true; break; }
            if (!found) { mini::string l; for (auto &x : inst.ctls) l += x.label + ","; ok(false, l); }
        } else if (c == "ifocus" && inst.ask) ok(arg == (inst.ask_focus == 0 ? "Stop and remove" : "Keep installing"), inst.ask_focus == 0 ? "Stop and remove" : "Keep installing");
        else if (c == "ifocus") ok(inst.focus < (int)inst.ctls.size() && inst.ctls[inst.focus].label == arg, inst.focus < (int)inst.ctls.size() ? inst.ctls[inst.focus].label : "-");
        else if (c == "iwait") { inst_pump(arg.empty() ? 120 : atof(arg.c_str()), true); }
        else if (c == "isleep") inst_pump(atof(arg.c_str()), false);
        else if (c == "icancel") inst_ask_cancel();
        else if (c == "istarted") ok((inst.started.empty() ? mini::string("none") : inst.started) == arg, inst.started);
        else if (c == "iexists") ok(file_exists(arg) || is_dir(arg), "-");
        else if (c == "igone") ok(!file_exists(arg) && !is_dir(arg), "-");
        else if (c == "inote") ok(inst.end_body.find(arg) != mini::string::npos || inst.end_title.find(arg) != mini::string::npos, inst.end_title + " / " + inst.end_body);
        else return false;
        return true;
    }

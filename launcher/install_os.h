// Ultra Instinct Tears setup: the OS calls only the installer needs (free space, drives, a download, shortcuts, ending
// a build and everything it started, removing a folder). [UIT 0433] Windows is the target; the Linux versions exist for
// the tests (tools/tests/test_installer.py) and screenshots.
#pragma once
#include <stdint.h>
#include "mini.h"
#include "platform.h"

namespace ut {

mini::string self_exe();                                   // this program's own file
int64_t free_bytes(const mini::string &path);              // free space where path is (or its nearest existing parent); -1 unknown
mini::vector<mini::string> fixed_drives();                 // "C:\\", "D:\\" ... (Linux: the home folder)
mini::string home_folder();                                // the user's profile folder
mini::string desktop_folder();
mini::string start_menu_folder();                          // Start menu > Programs
bool is_dir(const mini::string &path);
// Downloads url to path (https only, redirects followed). progress(done, total) from the download's thread; total 0 when
// unknown. *stop set to non-zero ends it (false, error "stopped"). User agent: the installer's name.
bool http_get(const mini::string &url, const mini::string &path, volatile int *stop,
              void (*progress)(void *ud, int64_t done, int64_t total), void *ud, mini::string *error);
// The same for a small answer kept in memory (the GitHub release list).
bool http_get_text(const mini::string &url, mini::string *out, mini::string *error);
// A shortcut (Windows .lnk) at link to target, started in workdir.
bool make_shortcut(const mini::string &link, const mini::string &target, const mini::string &workdir,
                   const mini::string &description, mini::string *error);
// Ends a helper and every process it started (the build's compilers too), and waits a little for them to go.
void stop_tree(Game *proc);
// Removes a folder and everything in it. False if anything is left (the reason in *error).
bool remove_tree(const mini::string &path, mini::string *error);
// Starts a program (the installed launcher) on its own, not tied to this one; args already quoted.
bool start_detached(const mini::string &exe, const mini::string &args, const mini::string &dir, mini::string *error);

}  // namespace ut

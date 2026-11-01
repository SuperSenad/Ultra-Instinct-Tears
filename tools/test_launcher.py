#!/usr/bin/env python3
# Drives the launcher's --script mode through every button and dialog and checks it neither crashes nor writes a
# wrong ultrainstincttears.cfg. Meant for a sanitizer build (see tools/test_mini.cpp for the build line) but works with any
# Linux launcher:  python3 tools/test_launcher.py build/launcher/ultrainstincttears-launcher
# Needs no game files; the folder gets empty stand-ins for UltraInstinctTears.exe and the DLSS DLLs.
import os, subprocess, sys, tempfile

USER_CFG = """# Ultra Instinct Tears settings (edited by hand)
TOTK_UPSCALER=dlss
# DLSS at 2160p
TOTK_DLSS_MODE=quality

# shaders
TOTK_SHADER_PREWARM=all
TOTK_VK_ROBUST=1
TOTK_VK_CHECKPOINTS=1
TOTK_FSR=1
TOTK_FPS=60
"""

# (name, dlls?, cfg text or None, script, comments that must survive). ok/cancel/alt fail when no dialog is open,
# so a press that only flashes "already ..." must not be followed by one ("each" leaves every setting at default).
CASES = [
    ("recommended+apply", True, USER_CFG, "tab:0;down:2;accept;accept;check:TOTK_VK_ROBUST=-;check:TOTK_VK_CHECKPOINTS=-;"
     "check:TOTK_SHADER_PREWARM=-;check:TOTK_FULLSCREEN=1;check:TOTK_FPS=60", ["# shaders", "# DLSS at 2160p"]),
    ("recommended+apply, no DLLs", False, USER_CFG, "press:recommend;ok;check:TOTK_VK_ROBUST=-;check:TOTK_FPS=60", ["# shaders"]),
    ("recommended+cancel", True, USER_CFG, "press:recommend;cancel;check:TOTK_VK_ROBUST=1;check:TOTK_SHADER_PREWARM=all", None),
    ("recommended twice", True, USER_CFG, "press:recommend;ok;press:recommend;press:recommend;each;press:recommend;ok", ["# shaders"]),
    ("reset all", True, USER_CFG, "press:reset;ok;check:TOTK_VK_ROBUST=-;check:TOTK_FPS=-;check:TOTK_UPSCALER=-", ["# shaders"]),
    ("reset all+cancel", True, USER_CFG, "press:reset;cancel;check:TOTK_FPS=60", None),
    ("every setting", True, USER_CFG, "each;save;reload;each;press:recommend;ok;press:reset;ok;save;reload", ["# shaders"]),
    ("every setting, no DLLs", False, USER_CFG, "each;save;reload;press:recommend;ok;press:reset;ok", ["# shaders"]),
    ("no cfg file", True, None, "press:recommend;ok;check:TOTK_FPS=60;check:TOTK_FULLSCREEN=1;press:reset;ok;check:TOTK_FPS=-;each;reload", None),
    ("empty cfg", False, "", "each;press:recommend;ok;save;reload;press:reset;ok", None),
    ("CRLF cfg", True, USER_CFG.replace("\n", "\r\n"), "press:recommend;ok;check:TOTK_VK_ROBUST=-;check:TOTK_FPS=60", ["# shaders"]),
    ("other tabs", True, USER_CFG, "tab:1;down:3;accept;tab:2;accept;tab:0;press:recommend;cancel;press:reset;cancel", None),
    # the settings file from before the rename (ultratears.cfg): copied to the new name, the old one left as it was
    ("old cfg name", True, USER_CFG, "check:TOTK_FPS=60;check:TOTK_VK_ROBUST=1;press:recommend;ok;check:TOTK_VK_ROBUST=-", ["# shaders"]),
    ("old and new cfg", True, USER_CFG, "check:TOTK_FPS=30;check:TOTK_VK_ROBUST=-", None),
    # button bindings (Game > Controller buttons / Keyboard keys): press-to-bind, swaps, rejects, clear, cancel, reset group
    ("bind key", True, USER_CFG, "bind:TOTK_KEY_A;presskey:P;check:TOTK_KEY_A=P;bind:TOTK_KEY_A;presskey:J;check:TOTK_KEY_A=-", ["# shaders"]),
    ("bind key swap", True, USER_CFG, "bind:TOTK_KEY_X;presskey:U;check:TOTK_KEY_X=U;check:TOTK_KEY_Y=I;"
     "bind:TOTK_KEY_A;presskey:K;check:TOTK_KEY_A=K;check:TOTK_KEY_B=Space", None),
    ("bind key rejects", True, USER_CFG, "bind:TOTK_KEY_A;presskey:W;presskey:Up;presskey:F13;check:TOTK_KEY_A=-;presskey:Escape;check:TOTK_KEY_A=-", None),
    ("bind clear", True, USER_CFG, "bind:TOTK_KEY_ZR;presskey:Delete;check:TOTK_KEY_ZR=none;save;reload;check:TOTK_KEY_ZR=none;"
     "bind:TOTK_PAD_Y;presskey:Delete;check:TOTK_PAD_Y=none", None),
    ("bind pad swap", True, USER_CFG, "padtype:xbox;bind:TOTK_PAD_A;presspad:south;check:TOTK_PAD_A=south;check:TOTK_PAD_B=east;"
     "bind:TOTK_PAD_X;presspad:righttrigger;check:TOTK_PAD_X=righttrigger;check:TOTK_PAD_ZR=north", None),
    ("bind pad cancel", True, USER_CFG, "padtype:ps;bind:TOTK_PAD_L;presskey:Escape;check:TOTK_PAD_L=-;bind:TOTK_PAD_L;presspad:leftshoulder;check:TOTK_PAD_L=-", None),
    ("bind reset group", True, USER_CFG, "bind:TOTK_PAD_A;presspad:south;bind:TOTK_PAD_DUP;presspad:dpdown;check:TOTK_PAD_DDOWN=dpup;"
     "jump:TOTK_PAD_DRIGHT;down;accept;ok;check:TOTK_PAD_A=-;check:TOTK_PAD_B=-;check:TOTK_PAD_DUP=-;check:TOTK_PAD_DDOWN=-;check:TOTK_FPS=60", ["# shaders"]),
    # Performance > Stutter (the old Stutter tab) and the shadow-verify diagnostic in Advanced
    ("stutter section", True, USER_CFG, "jump:TOTK_SHADER_PREWARM;reset;check:TOTK_SHADER_PREWARM=-;tab:2;end;back;back;tab:4;tab:5;tab:0", ["# shaders"]),
    ("shadow verify", True, USER_CFG, "jump:TOTK_VK_SHADOW_VERIFY;accept;check:TOTK_VK_SHADOW_VERIFY=60;jump:TOTK_VK_SHADOW_VERIFY_MIB;reset;check:TOTK_VK_SHADOW_VERIFY_MIB=-", None),
    ("cpu experiments", True, USER_CFG, "jump:TOTK_VK_POOL_FASTPATH;check:TOTK_VK_POOL_FASTPATH=-;accept;check:TOTK_VK_POOL_FASTPATH=0;accept;check:TOTK_VK_POOL_FASTPATH=-;jump:TOTK_VK_PCORES;accept;check:TOTK_VK_PCORES=1", None),
    ("async submit", True, USER_CFG, "jump:TOTK_VK_ASYNC_SUBMIT;check:TOTK_VK_ASYNC_SUBMIT=-;accept;check:TOTK_VK_ASYNC_SUBMIT=0;"
     "accept;check:TOTK_VK_ASYNC_SUBMIT=-", None),
    ("thread mmcss", True, USER_CFG, "jump:TOTK_THREAD_MMCSS;check:TOTK_THREAD_MMCSS=-;accept;check:TOTK_THREAD_MMCSS=1;accept;check:TOTK_THREAD_MMCSS=-", None),   # [UIT native-1031]
    ("copy token async", True, USER_CFG, "jump:TOTK_COPY_TOKEN_ASYNC;check:TOTK_COPY_TOKEN_ASYNC=-;accept;check:TOTK_COPY_TOKEN_ASYNC=1;accept;check:TOTK_COPY_TOKEN_ASYNC=-", None),   # [UIT native-1031]
    ("memory log", True, USER_CFG, "jump:TOTK_VK_MEMLOG;check:TOTK_VK_MEMLOG=-;accept;check:TOTK_VK_MEMLOG=1;accept;check:TOTK_VK_MEMLOG=-", None),   # [UIT native-1031]
    ("shader early", True, USER_CFG, "jump:TOTK_SHADER_EARLY;check:TOTK_SHADER_EARLY=-;accept;check:TOTK_SHADER_EARLY=1;accept;check:TOTK_SHADER_EARLY=-", None),   # [UIT native-1031]
    ("shader bin bg", True, USER_CFG, "jump:TOTK_SHADER_BINARY_CACHE;check:TOTK_SHADER_BINARY_CACHE=-;jump:TOTK_SHADER_BIN_BG;check:TOTK_SHADER_BIN_BG=-;accept;check:TOTK_SHADER_BIN_BG=0;accept;check:TOTK_SHADER_BIN_BG=-", None),   # [UIT native-1031]
    ("dlss mv probe", True, USER_CFG, "jump:TOTK_DLSS_MV_PROBE;check:TOTK_DLSS_MV_PROBE=-;accept;check:TOTK_DLSS_MV_PROBE=1;"
     "accept;check:TOTK_DLSS_MV_PROBE=-;check:TOTK_DLSS_DEBUG_MV=-;check:TOTK_UPSCALER=dlss", None),
    # 0065-0069: menu background fix (on by default), output resolution / aspect, filtering, present, frame step
    ("menu bg fix", True, USER_CFG, "jump:TOTK_MENU_BG_FIX;check:TOTK_MENU_BG_FIX=-;accept;check:TOTK_MENU_BG_FIX=0;accept;check:TOTK_MENU_BG_FIX=-", None),
    ("output res", True, USER_CFG, "jump:TOTK_OUTPUT_RES;check:TOTK_OUTPUT_RES=-;accept;check:TOTK_OUTPUT_RES=display;accept;check:TOTK_OUTPUT_RES=1440p;reset;check:TOTK_OUTPUT_RES=-", None),
    ("aspect", True, USER_CFG, "jump:TOTK_ASPECT;accept;check:TOTK_ASPECT=21x9;jump:TOTK_ASPECT_HUD;accept;check:TOTK_ASPECT_HUD=center", None),
    ("aniso present", True, USER_CFG, "jump:TOTK_ANISO;accept;check:TOTK_ANISO=2;jump:TOTK_PRESENT_MODE;accept;check:TOTK_PRESENT_MODE=mailbox;"
     "jump:TOTK_TRIPLE_BUFFER;accept;check:TOTK_TRIPLE_BUFFER=on", None),
    ("frame step", True, USER_CFG, "jump:TOTK_FRAME_STEP;check:TOTK_FRAME_STEP=-;accept;check:TOTK_FRAME_STEP=fixed;check:TOTK_FPS=60", None),
    # [UIT 0070] Graphics > Detail and pop-in: greyed out (accept does nothing) unless data/generated/lift-features.ini
    # says the build-time scan found that code; Game's own FXAA and the shadow-size check work in any build
    ("lift none", True, USER_CFG, "jump:TOTK_QUALITY_REDUCTION;accept;check:TOTK_QUALITY_REDUCTION=-;jump:TOTK_LOD_IMPROVE;accept;"
     "check:TOTK_LOD_IMPROVE=-;jump:TOTK_SHADOW_RES;accept;check:TOTK_SHADOW_RES=-;jump:TOTK_GAME_FXAA;accept;check:TOTK_GAME_FXAA=off;"
     "jump:TOTK_LIFT_PROBE;check:TOTK_LIFT_PROBE=-;accept;check:TOTK_LIFT_PROBE=0;each", None),
    ("lift found", True, USER_CFG, "jump:TOTK_QUALITY_REDUCTION;accept;check:TOTK_QUALITY_REDUCTION=off;jump:TOTK_LOD_IMPROVE;accept;"
     "check:TOTK_LOD_IMPROVE=1;jump:TOTK_SHADOW_RES;accept;check:TOTK_SHADOW_RES=512;reset;check:TOTK_SHADOW_RES=-;each", None),
    ("lift some", True, USER_CFG, "jump:TOTK_QUALITY_REDUCTION;accept;check:TOTK_QUALITY_REDUCTION=off;jump:TOTK_LOD_IMPROVE;accept;"
     "check:TOTK_LOD_IMPROVE=-;jump:TOTK_SHADOW_RES;accept;check:TOTK_SHADOW_RES=-", None),
    ("hitch options", True, USER_CFG, "jump:TOTK_SHADER_BINARY_CACHE;check:TOTK_SHADER_BINARY_CACHE=-;accept;check:TOTK_SHADER_BINARY_CACHE=1;accept;check:TOTK_SHADER_BINARY_CACHE=-;jump:TOTK_VK_PRESYNC_SUBS;check:TOTK_VK_PRESYNC_SUBS=-", None),
    ("merge3", True, USER_CFG, "jump:TOTK_VK_WB_MERGE3;check:TOTK_VK_WB_MERGE3=-;accept;check:TOTK_VK_WB_MERGE3=0;reset;check:TOTK_VK_WB_MERGE3=-", None),
    # About > Install update / Report a problem: the real tools/update.py and report.py, a stand-in tools/setup.py
    ("update none", True, USER_CFG, "press:update;waittask;title:No update package found;cancel;title:", None),
    ("update install", True, USER_CFG, "press:update;waittask;title:Install this update?;ok;waittask;title:Update installed;ok;check:TOTK_FPS=60", None),
    ("update rollback", True, USER_CFG, "press:update;waittask;title:Install this update?;ok;waittask;title:The update failed and was undone;ok", None),
    ("update pick", True, USER_CFG, "press:update;waittask;title:No update package found;pickzip:{PKG};waittask;title:Install this update?;cancel", None),
    # [UIT 0281] GitHub releases from a mock api.github.com (tools/tests/mock_github.py; every update case uses it, the
    # cases above see the private repository's 404): what's new, download + checks, then the same install
    ("update online", True, USER_CFG, "press:update;waittask;title:Update available: v1.0.0;bodyhas:Faster shaders;bodyhas:SHA-256;"
     "rowhas:New version=1.0.0;rowhas:Download=UltraInstinctTears-src.zip;ok;waittask;title:Update installed;ok;check:TOTK_FPS=60", None),
    ("update online 404", True, USER_CFG, "press:update;waittask;title:No update package found;bodyhas:GitHub: could not check;bodyhas:private;cancel", None),
    ("update online ratelimit", True, USER_CFG, "press:update;waittask;title:Install this update?;rowhas:GitHub=try again after;cancel", None),
    ("update online zip", True, USER_CFG, "press:update;waittask;title:Update available: v1.0.0;alt;title:Install this update?;ok;waittask;title:Update installed;ok", None),
    ("update online uptodate", True, USER_CFG, "press:update;waittask;title:Install this update?;rowhas:GitHub=up to date;cancel", None),
    ("update online off", True, USER_CFG + "TOTK_LAUNCHER_UPDATE_ONLINE=0\n", "press:update;waittask;title:No update package found;cancel", None),
    ("update online badsha", True, USER_CFG, "press:update;waittask;title:Update available: v1.0.0;ok;waittask;title:The download failed;bodyhas:Nothing was installed;ok", None),
    ("update online prerelease", True, USER_CFG + "TOTK_LAUNCHER_UPDATE_PRERELEASE=1\n", "press:update;waittask;title:Update available: v1.1.0-rc1;rowhas:New version=pre-release;cancel", None),
    ("update online busy play", True, USER_CFG, "press:update;press:play;waittask;title:Update available: v1.0.0;cancel", None),
    # [UIT 0320] Advanced > Updates and reports > Check for updates when the launcher starts (startupcheck = what the launcher
    # does a few seconds after it opens): a notice only (dot on About, the bottom line), never a dialog, never a download
    ("update start newer", True, USER_CFG, "startupcheck;waitcheck;checkucheck:done;checkbadge:1;"
     "checkflash:Update available: v1.0.0 - About > Install update;nodialog;tab:6;shot:{OUT}/about.png;"
     "press:update;waittask;title:Update available: v1.0.0;cancel;checkbadge:1", None),
    ("update start same", True, USER_CFG, "startupcheck;waitcheck;checkucheck:done;checkbadge:0;checknoflash:Update available;nodialog", None),
    ("update start offline", True, USER_CFG, "startupcheck;waitcheck;checkucheck:done;checkbadge:0;checknoflash:Update available;nodialog", None),
    ("update start ratelimit", True, USER_CFG, "startupcheck;waitcheck;checkucheck:done;checkbadge:0;nodialog", None),
    ("update start game running", True, USER_CFG, "gamerunning:1;startupcheck;wait:30;checkucheck:armed;checkbadge:0;nodialog", None),
    ("update start game starts", True, USER_CFG, "startupcheck;wait:6;checkucheck:asking;gamerunning:1;wait:2;checkucheck:armed;"
     "checkbadge:0;nodialog", None),
    ("update start after game", True, USER_CFG, "gamerunning:1;startupcheck;wait:10;checkucheck:armed;gamerunning:0;waitcheck;"
     "checkucheck:done;checkbadge:1;nodialog", None),
    ("update start off", True, USER_CFG + "TOTK_LAUNCHER_UPDATE_ON_START=0\n", "startupcheck;wait:10;checkucheck:off;checkbadge:0;"
     "jump:TOTK_LAUNCHER_UPDATE_ON_START;accept;check:TOTK_LAUNCHER_UPDATE_ON_START=-;checkapplies:TOTK_LAUNCHER_UPDATE_ON_START=launcher", None),
    ("update start online off", True, USER_CFG + "TOTK_LAUNCHER_UPDATE_ONLINE=0\n", "startupcheck;wait:10;checkucheck:off;checkbadge:0", None),
    ("update start relaunch", True, USER_CFG, "relaunch;startupcheck;wait:10;checkucheck:off;checkbadge:0", None),
    ("update start prerelease", True, USER_CFG + "TOTK_LAUNCHER_UPDATE_PRERELEASE=1\n", "startupcheck;waitcheck;checkbadge:1;"
     "checkflash:Update available: v1.1.0-rc1", None),
    ("update start search", True, USER_CFG, "tab:5;search:launcher starts;checkrows:TOTK_LAUNCHER_UPDATE_ON_START,TOTK_LAUNCHER_UPDATE_ONLINE;"
     "jump:TOTK_LAUNCHER_UPDATE_ON_START;accept;check:TOTK_LAUNCHER_UPDATE_ON_START=0;reset;check:TOTK_LAUNCHER_UPDATE_ON_START=-", None),
    ("report", True, USER_CFG + "TOTK_LAUNCHER_REPORT_DIR={OUT}\n", "press:report;waittask;title:Problem report saved;ok", None),
    # [UIT 0149] About > Faster game code (PGO): a stand-in tools/setup.py answers --pgo-guide --pgo-tsv from data/pgo-state.txt
    ("pgo normal", True, USER_CFG, "press:pgo;waittask;title:Faster game code (PGO);ok;waittask;title:Profiling version built;ok;check:TOTK_FPS=60", None),
    ("pgo normal close", True, USER_CFG, "press:pgo;waittask;title:Faster game code (PGO);cancel;title:", None),
    ("pgo profiling", True, USER_CFG, "press:pgo;waittask;title:Faster game code (PGO);ok;waittask;title:Fast version built;ok", None),
    ("pgo profiling none", True, USER_CFG, "press:pgo;waittask;title:Faster game code (PGO);alt;waittask;title:Back on the normal build;ok", None),
    ("pgo fast", True, USER_CFG, "press:pgo;waittask;title:Faster game code (PGO);ok;title:", None),
    ("pgo fail", True, USER_CFG, "press:pgo;waittask;title:Faster game code (PGO);ok;waittask;title:The build failed;ok", None),
    # Mods tab (0059): the real tools/mods.py on fixture mods Beta (top) and Alpha (info.json "Alpha Pack"), no game files
    ("mods order", True, USER_CFG, "tab:4;checkmods:Beta+,Alpha+;focusmod:Alpha;accept;checkmods:Beta+,Alpha-;modup;checkmods:Alpha-,Beta+;"
     "grab;down;accept;checkmods:Beta+,Alpha-;press:alloff;checkmods:Beta-,Alpha-;press:allon;checkmods:Beta+,Alpha+;"
     "focusmod:Alpha;left;checkmods:Beta+,Alpha-;right;checkmods:Beta+,Alpha+;kbm;focusmod:Beta;moddown;checkmods:Alpha+,Beta+", None),
    # [UIT 0062] Disable Mods / Enable Mods: all off in one press, then back to exactly the set that was on (not all),
    # again and again; by keyboard (Enter), controller (A) and mouse (a click on the row, the first one in the tab)
    ("mods disable", True, USER_CFG,
     "tab:4;focusmod:Alpha;accept;checkmods:Beta+,Alpha-;press:alloff;checkmods:Beta-,Alpha-;checkbefore:Beta;"
     "press:alloff;checkmods:Beta+,Alpha-;checkbefore:-;press:alloff;press:alloff;checkmods:Beta+,Alpha-;"
     "kbm;focusmod:Beta;up:4;accept;checkmods:Beta-,Alpha-;accept;checkmods:Beta+,Alpha-;"
     "pad;focusmod:Beta;up:4;accept;checkmods:Beta-,Alpha-;accept;checkmods:Beta+,Alpha-;"
     "mouse:375,219;click;checkmods:Beta-,Alpha-;click;checkmods:Beta+,Alpha-;"
     "press:alloff;focusmod:Alpha;accept;checkmods:Beta-,Alpha+;press:alloff;checkmods:Beta-,Alpha-;checkbefore:Alpha;"
     "press:allon;checkmods:Beta+,Alpha+;checkbefore:-", None),
    # [UIT 0175] Disable Mods turns the built-in cheats off too, remembers them ("::KEY" lines) and Enable Mods puts them back;
    # with no mods installed the button is there for the cheats alone; cheats remembered while every mod was off leave the mods off
    ("mods disable cheats", True, USER_CFG + "TOTK_CHEAT_DURABILITY=1\n",
     "tab:4;checkcheats:TOTK_CHEAT_DURABILITY/-;press:alloff;checkmods:Beta-,Alpha-;checkcheats:-/TOTK_CHEAT_DURABILITY;"
     "checkbefore:Beta,Alpha;check:TOTK_CHEAT_DURABILITY=-;press:alloff;checkmods:Beta+,Alpha+;checkcheats:TOTK_CHEAT_DURABILITY/-;"
     "check:TOTK_CHEAT_DURABILITY=1;checkbefore:-;press:alloff;press:allon;checkcheats:-/TOTK_CHEAT_DURABILITY;press:alloff;"
     "checkmods:Beta-,Alpha-;checkcheats:-/TOTK_CHEAT_DURABILITY;checkbefore:Beta,Alpha;press:alloff;"
     "checkcheats:TOTK_CHEAT_DURABILITY/-;checkmods:Beta+,Alpha+;checkbefore:-", None),   # All on between: the cheat is still remembered
    ("mods none cheats", True, USER_CFG + "TOTK_CHEAT_AMIIBO=1\nTOTK_CHEAT_DURABILITY=1\n",
     "tab:4;checkmods:;press:alloff;checkcheats:-/TOTK_CHEAT_DURABILITY,TOTK_CHEAT_AMIIBO;check:TOTK_CHEAT_AMIIBO=-;"
     "press:alloff;checkcheats:TOTK_CHEAT_DURABILITY,TOTK_CHEAT_AMIIBO/-;check:TOTK_CHEAT_AMIIBO=1;check:TOTK_CHEAT_DURABILITY=1", None),
    ("mods cheats only", True, USER_CFG + "TOTK_CHEAT_AMIIBO=1\n",
     "tab:4;focusmod:Alpha;accept;focusmod:Beta;accept;checkmods:Beta-,Alpha-;press:alloff;checkcheats:-/TOTK_CHEAT_AMIIBO;"
     "press:alloff;checkmods:Beta-,Alpha-;checkcheats:TOTK_CHEAT_AMIIBO/-;checkbefore:-", None),
    ("mods pad", True, USER_CFG, "pad;tab:4;focusmod:Alpha;grab;up;accept;checkmods:Alpha+,Beta+;focusmod:Beta;grab;up;tab:0;checkmods:Beta+,Alpha+", None),
    ("mods pad far", True, USER_CFG, "tab:4;pickmod:{GAMMA};waittask;ok;pad;focusmod:Alpha;grab;up;up;accept;checkmods:Alpha+,Gamma+,Beta+", None),   # the grab stays on the mod it picked up
    ("mods install", True, USER_CFG, "tab:4;pickmod:{GAMMA};waittask;title:Mod installed;ok;checkmods:Gamma+,Beta+,Alpha+", None),
    ("mods install two", True, USER_CFG, "tab:4;pickmod:{GAMMA}|{BROKEN};waittask;title:1 installed, 1 not;ok;checkmods:Gamma+,Beta+,Alpha+", None),
    ("mods broken", True, USER_CFG, "pickmod:{BROKEN};waittask;title:Could not install;ok;checkmods:Beta+,Alpha+", None),
    ("mods drop", True, USER_CFG, "drop:{EPS};waittask;title:Mod installed;ok;checkmods:Eps+,Beta+,Alpha+", None),
    ("mods inbox", True, USER_CFG, "checkinbox:1;tab:4;press:inbox;waittask;title:Mod installed;ok;checkinbox:0;checkmods:Delta+,Beta+,Alpha+", None),
    ("mods inbox empty", True, USER_CFG, "tab:4;press:inbox;waittask;title:The inbox is empty;ok", None),
    ("mods remove", True, USER_CFG, "tab:4;focusmod:Alpha;reset;title:Uninstall Alpha Pack?;ok;waittask;checkmods:Beta+", None),
    ("mods off", True, USER_CFG + "TOTK_MODS=0\n", "tab:4;checkmods:Beta+,Alpha+;jump:TOTK_MODS;accept;check:TOTK_MODS=-", None),
    ("mods none", True, USER_CFG, "tab:4;checkmods:;down:9;tab:0", None),
    # setup.py packs the game's RomFS into romfs.bin + runtime-index.bin: "new to the game" comes from that list, any case
    ("mods game index", True, USER_CFG + "TOTK_ROMFS=data/romfs\n", "tab:4;checknew:Alpha=1;checknew:Beta=0;checknew:Loose=0", None),
    ("mods loose file", True, USER_CFG + "TOTK_ROMFS=data/romfs\n",
     "tab:4;drop:{LOOSE};waittask;title:Game file placed;ok;checkmods:Common (LayoutArchive)+,Beta+,Alpha+;"
     "checknew:Common (LayoutArchive)=0;checkplaced:Common (LayoutArchive)=UI/LayoutArchive/Common.Product.110.Nin_NX_NVN.blarc.zs;"
     "checkplaced:Beta=;pickmod:{PCHTXT};waittask;title:Could not install;ok", None),
    # Undo in the confirmation: controller (left to Undo, A), then keyboard; the list and load order are as before
    ("mods loose undo", True, USER_CFG + "TOTK_ROMFS=data/romfs\n",
     "tab:4;focusmod:Alpha;accept;checkmods:Beta+,Alpha-;pad;drop:{LOOSE};waittask;title:Game file placed;left;accept;waittask;"
     "checkmods:Beta+,Alpha-;checkplaced:Common (LayoutArchive)=-;kbm;drop:{LOOSE};waittask;title:Game file placed;left;accept;waittask;"
     "checkmods:Beta+,Alpha-;drop:{LOOSE};waittask;ok;checkmods:Common (LayoutArchive)+,Beta+,Alpha-", None),
    # [UIT 0251] search: filter and rank, fold state kept, keyboard / controller / tab behaviour, Mods tab
    ("search filter", True, USER_CFG,
     "tab:2;search:fps;checkfocus:TOTK_FPS;search:TOTK-FPS;checkfocus:TOTK_FPS;search:Target RATE frame;checkfocus:TOTK_FPS;checkcount:4;"
     "search:tex grace;checkrows:TOTK_TEX_GRACE_MS;search:zzqx;checkrows:-;checkcount:0;search:;checkquery:-;checkcount:0;"
     "tab:1;search:sharpening;checkfocus:TOTK_FSR_SHARPNESS;right;check:TOTK_FSR_SHARPNESS=0.3", None),
    ("search folds", True, USER_CFG,
     "tab:2;checkopen:performance/Stutter/Textures+=0;search:grace;checkrows:TOTK_TEX_GRACE_MS;checkopen:performance/Stutter/Textures+=0;"
     "search:;checkopen:performance/Stutter/Textures+=0;checkfocus:group:performance/Stutter/Textures+;"
     "jump:TOTK_TEX_GRACE_MS;checkopen:performance/Stutter/Textures+=1;search:fps;search:;checkopen:performance/Stutter/Textures+=1;"
     "checkfocus:TOTK_FPS", None),   # clearing keeps the setting the search landed on
    ("search keyboard", True, USER_CFG,
     "tab:2;key:ctrl+f;checkfocus:search;text:tex grace;checkrows:TOTK_TEX_GRACE_MS;key:Down;checkfocus:TOTK_TEX_GRACE_MS;"
     "key:Up;checkfocus:search;key:Return;checkfocus:TOTK_TEX_GRACE_MS;key:Escape;checkfocus:search;checkquery:tex grace;"
     "key:Escape;checkquery:-;checkfocus:search;key:Escape;checkfocus:group:performance/Stutter/Textures+;"
     "key:/;checkfocus:search;checkquery:-;text:frame rate;key:ctrl+Backspace;checkquery:frame ;key:Backspace;checkquery:frame;"
     "key:Tab;checktab:3;checkquery:-;key:shift+Tab;checkquery:frame;checkfocus:TOTK_FPS;tab:0;key:/;checktab:2;checkfocus:search", None),
    ("search pad", True, USER_CFG,
     "tab:2;pad;padbtn:view;checkfocus:osk;padbtn:down;padbtn:right;padbtn:right;padbtn:right;padbtn:a;padbtn:up;padbtn:right;padbtn:right;padbtn:right;padbtn:right;padbtn:right;padbtn:right;padbtn:a;padbtn:down;padbtn:left;padbtn:left;padbtn:left;padbtn:left;padbtn:left;padbtn:left;padbtn:left;padbtn:left;padbtn:a;"
     "checkquery:fps;padbtn:x;checkquery:fp;padbtn:a;checkquery:fps;padbtn:lb;checktab:2;padbtn:start;checkfocus:TOTK_FPS;"
     "padbtn:right;check:TOTK_FPS=120;padbtn:b;checkfocus:search;padbtn:a;checkfocus:osk;padbtn:b;checkfocus:search;"
     "padbtn:b;checkquery:-;padbtn:b;checkfocus:TOTK_FPS;padbtn:up;padbtn:up;padbtn:rb;checktab:3", None),
    ("search other tabs", True, USER_CFG,
     "tab:1;search:clock;checkrows:-;checkfocus:button:Show 4 matches in Performance;accept;checktab:2;checkquery:clock;"
     "checkfocus:TOTK_REALTIME;tab:1;checkquery:clock", None),
    ("mods search", True, USER_CFG,
     "tab:4;checkmods:Beta+,Alpha+;search:beta;checkrows:mod:Beta;focusmod:Beta;moddown;grab;checkmods:Beta+,Alpha+;accept;"
     "checkmods:Beta-,Alpha+;search:;checkquery:-;modup;checkmods:Beta-,Alpha+", None),
    ("bind hand-edited cfg", True, USER_CFG + "TOTK_KEY_B=K,Space,Bogus\nTOTK_PAD_A=nonsense\n", "each;check:TOTK_KEY_B=K,Space,Bogus;check:TOTK_PAD_A=nonsense", None),
    # [UIT 0270] the first-open question (which face button confirms) and both layouts
    ("confirm ask pad standard", True, USER_CFG,
     "pad;padtype:xbox;firstrun;title:Which button should confirm?;padbtn:south;nodialog;check:TOTK_UI_CONFIRM=standard;"
     "checkbuttons:A/B;firstrun", None),
    ("confirm ask pad game", True, USER_CFG,
     "pad;padtype:xbox;firstrun;padbtn:east;nodialog;check:TOTK_UI_CONFIRM=game;checkbuttons:B/A;padtype:nintendo;checkbuttons:A/B;"
     "padtype:ps;checkbuttons:Circle/Cross", None),
    ("confirm ask keyboard", True, USER_CFG,
     "firstrun;title:Which button should confirm?;accept;nodialog;check:TOTK_UI_CONFIRM=standard", None),
    ("confirm ask keyboard right", True, USER_CFG,
     "firstrun;key:Right;accept;check:TOTK_UI_CONFIRM=game", None),
    ("confirm ask buttons", True, USER_CFG, "firstrun;ok;check:TOTK_UI_CONFIRM=game;firstrun;nodialog", None),
    ("confirm ask escape", True, USER_CFG, "firstrun;key:Escape;nodialog;check:TOTK_UI_CONFIRM=-;firstrun;title:Which button should confirm?;alt;check:TOTK_UI_CONFIRM=standard", None),
    ("confirm setting", True, USER_CFG + "TOTK_UI_CONFIRM=game\n",
     "firstrun;nodialog;pad;padtype:xbox;checkbuttons:B/A;jump:TOTK_UI_CONFIRM;left;check:TOTK_UI_CONFIRM=standard;checkbuttons:A/B;"
     "padtype:nintendo;checkbuttons:B/A", None),
    # the search and its on-screen keyboard with the game's layout: east types / opens, south goes back
    ("search pad game layout", True, USER_CFG + "TOTK_UI_CONFIRM=game\n",
     "tab:2;pad;padbtn:view;checkfocus:osk;padbtn:down;padbtn:right;padbtn:right;padbtn:right;padbtn:b;padbtn:up;padbtn:right;padbtn:right;padbtn:right;padbtn:right;padbtn:right;padbtn:right;padbtn:b;padbtn:down;padbtn:left;padbtn:left;padbtn:left;padbtn:left;padbtn:left;padbtn:left;padbtn:left;padbtn:left;padbtn:b;"
     "checkquery:fps;padbtn:x;checkquery:fp;padbtn:b;checkquery:fps;padbtn:lb;checktab:2;padbtn:start;checkfocus:TOTK_FPS;"
     "padbtn:right;check:TOTK_FPS=120;padbtn:a;checkfocus:search;padbtn:b;checkfocus:osk;padbtn:a;checkfocus:search;"
     "padbtn:a;checkquery:-;padbtn:a;checkfocus:TOTK_FPS;padbtn:up;padbtn:up;padbtn:rb;checktab:3", None),
    # [UIT 0304] when a change takes effect: restart / live / setup / launcher, and the note for a change made while the game
    # runs (gamerunning:1 makes the launcher behave as while the game runs; nothing is started)
    ("applies tags", True, USER_CFG, "checkapplies:TOTK_FPS=live;checkapplies:TOTK_BG_FPS=live;checkapplies:TOTK_FSR=live;"
     "checkapplies:TOTK_PAD_A=live;checkapplies:TOTK_FREE_CORES=restart;checkapplies:TOTK_DLSS_MODE=restart;checkapplies:exefs=restart;"
     "checkapplies:TOTK_LIFT_REGCACHE=setup;checkapplies:TOTK_LAUNCHER_PYTHON=launcher;tab:2;search:frame rate in the background;checkrows:TOTK_BG_FPS", None),
    ("change while the game runs", True, USER_CFG, "gamerunning:1;tab:2;jump:TOTK_FREE_CORES;right;checkflash:Takes effect after restarting the game;"
     "check:TOTK_FREE_CORES=1;jump:TOTK_BG_FPS;right;checkflash:the in-game overlay changes it now;check:TOTK_BG_FPS=60;"
     "play;checkflash:already running;gamerunning:0;jump:TOTK_FREE_CORES;right;checknoflash:restarting", ["# shaders"]),
]
OLD_CFG = "ultratears.cfg"

def mods_setup(name, d):
    """For the mods cases: tools/mods.py, mods Beta and Alpha, and the sources to install. Returns (placeholders, check)."""
    import shutil, zipfile
    here = os.path.dirname(os.path.abspath(__file__))
    os.makedirs(os.path.join(d, "tools"))
    shutil.copy(os.path.join(here, "mods.py"), os.path.join(d, "tools", "mods.py"))
    def put(rel, data):
        p = os.path.join(d, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(data)
    if name not in ("mods none", "mods none cheats"):
        put("mods/Alpha/romfs/UI/shared.bin", b"alpha---")
        put("mods/Alpha/romfs/UI/alpha.txt", b"a")
        put("mods/Alpha/info.json", b'{"name": "Alpha Pack", "version": "1.2", "author": "Tester", "description": "A fixture mod."}')
        put("mods/Beta/romfs/UI/shared.bin", b"beta----")
        put("mods/Beta/exefs/patch.ips", b"PATCHEOF")
        put("mods/load-order.txt", b"+Beta\n+Alpha\n")
    src = os.path.join(os.path.dirname(d), "picked")
    os.makedirs(src)
    ph = {"GAMMA": os.path.join(src, "Gamma.zip"), "BROKEN": os.path.join(src, "Broken.zip"), "EPS": os.path.join(src, "Eps")}
    with zipfile.ZipFile(ph["GAMMA"], "w") as z:
        z.writestr("Gamma/romfs/Actor/g.bin", "gamma")
    open(ph["BROKEN"], "wb").write(b"PK\x03\x04 this is not a whole zip")
    os.makedirs(os.path.join(ph["EPS"], "romfs", "UI"))
    open(os.path.join(ph["EPS"], "romfs", "UI", "e.bin"), "wb").write(b"e")
    if name in ("mods game index", "mods loose file", "mods loose undo"):
        import struct
        gp = sorted(["UI/Shared.bin", "UI/LayoutArchive/Common.Product.110.Nin_NX_NVN.blarc.zs", "Actor/x.bin"])
        st, en = bytearray(), bytearray()
        for g in gp:
            b = g.encode() + b"\0"
            en += struct.pack("<QQII", 0, 1, len(st), len(b)); st += b
        put("data/romfs/runtime-index.bin", b"TOTKRFS1" + struct.pack("<QQ", len(gp), 24 + len(en)) + en + st)
        put("data/romfs/romfs.bin", b"\0")
        if name == "mods game index":
            put("mods/Loose/romfs/ui/layoutarchive/common.product.110.nin_nx_nvn.blarc.zs", b"z")
        ph["LOOSE"] = os.path.join(src, "Common.Product.110.Nin_NX_NVN.blarc.zs")
        open(ph["LOOSE"], "wb").write(b"z")
        ph["PCHTXT"] = os.path.join(src, "60fps.pchtxt")
        open(ph["PCHTXT"], "w").write("@nsobid-x")
    if name == "mods inbox":
        os.makedirs(os.path.join(d, "mods-inbox"))
        with zipfile.ZipFile(os.path.join(d, "mods-inbox", "Delta.zip"), "w") as z:
            z.writestr("romfs/UI/d.bin", "delta")

    def check():
        p = []
        lo = os.path.join(d, "mods", "load-order.txt")
        if name == "mods remove" and os.path.exists(os.path.join(d, "mods", "Alpha")):
            p.append("mods/Alpha still there")
        if name in ("mods install", "mods drop", "mods inbox") and not os.path.exists(lo):
            p.append("no load-order.txt")
        if name == "mods drop" and not os.path.isdir(ph["EPS"]):
            p.append("the dropped folder was moved (it must be copied)")
        if name == "mods inbox" and not os.path.exists(os.path.join(d, "mods-inbox", "installed", "Delta.zip")):
            p.append("Delta.zip not moved to mods-inbox/installed")
        if name == "mods order" and "+Alpha\n+Beta\n" not in open(lo).read():
            p.append("load-order.txt not saved as Alpha, Beta")
        return p
    return ph, check   # the name before the rename to Ultra Instinct Tears

def helper_setup(name, d):
    """For the update/report cases: copies the helpers in, makes a package zip. Returns (placeholders, check)."""
    import shutil, zipfile
    here = os.path.dirname(os.path.abspath(__file__))
    os.makedirs(os.path.join(d, "tools"))
    for f in ("update.py", "report.py", "progress_file.py"):
        shutil.copy(os.path.join(here, f), os.path.join(d, "tools", f))
    os.makedirs(os.path.join(d, "data"))
    open(os.path.join(d, "data", "keep.bin"), "wb").write(b"user data")
    open(os.path.join(d, "src.c"), "w").write("old\n")
    ph = {"OUT": os.path.join(os.path.dirname(d), "out"), "PKG": ""}
    os.makedirs(ph["OUT"])
    if name.startswith("pgo"):   # [UIT 0149]
        state = {"pgo normal": "normal 0", "pgo normal close": "normal 0", "pgo profiling": "profiling 2",
                 "pgo profiling none": "profiling 0", "pgo fast": "fast 1", "pgo fail": "normal 0"}[name]
        open(os.path.join(d, "data", "pgo-state.txt"), "w").write(state)
        open(os.path.join(d, "tools", "setup.py"), "w").write(
            "import sys, os\n"
            "st = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'data', 'pgo-state.txt')).read().split()\n"
            "if '--pgo-guide' in sys.argv:\n"
            "    print('Profile-guided optimization (PGO): ...')\n"
            "    if '--pgo-tsv' in sys.argv: print('pgo-state\\t%%s\\t%%s' %% (st[0], st[1]))\n"
            "    sys.exit(0)\n"
            "want = {'normal': '--pgo-profile', 'profiling': '--pgo' if st[1] != '0' else '--no-pgo'}.get(st[0], '?')\n"
            "print('[1/1] build ' + ' '.join(sys.argv[1:]), flush=True)\n"
            "sys.exit(0 if want in sys.argv and %r != 'pgo fail' else 1)\n" % name)
        return ph, lambda: []
    if name.startswith("update"):   # [UIT 0281] the mock GitHub: 404 (the private repository) unless the case says otherwise
        SRV.releases, SRV.assets, SRV.requests, SRV.mode = [], {}, [], "404"
        if (name.startswith("update online") or name.startswith("update start")) and name not in ("update online 404",):
            SRV.mode = "ratelimit" if name in ("update online ratelimit", "update start ratelimit") else \
                "stall" if name == "update start game starts" else "ok"
            p = os.path.join(os.path.dirname(d), "rel.zip")
            with zipfile.ZipFile(p, "w") as z:
                z.writestr("UltraInstinctTears/VERSION.txt", "Ultra Instinct Tears 1.0.0 (commit def5678)\n")
                z.writestr("UltraInstinctTears/src.c", "new\n")
                z.writestr("UltraInstinctTears/src/x.c", "int x;\n")
                z.writestr("UltraInstinctTears/data/keep.bin", "must not land")
                z.writestr("UltraInstinctTears/tools/setup.py", "print('[1/1] build', flush=True)\n")
            SRV.add_release("v1.0.0", open(p, "rb").read(), body="## What's new\n- **Faster shaders**\n- Fixed the map",
                            sha="wrong" if name == "update online badsha" else "sidecar")
            if name in ("update online prerelease", "update start prerelease"):
                SRV.add_release("v1.1.0-rc1", open(p, "rb").read().replace(b"1.0.0 (commit", b"1.1.0-rc1 (commit"), prerelease=True, sha=None)
        if name in ("update online uptodate", "update start same"):
            open(os.path.join(d, "VERSION.txt"), "w").write("Ultra Instinct Tears 1.0.0 (commit def5678)\n")
    online_only = name in ("update online", "update online 404", "update online badsha", "update online off", "update online prerelease",
                           "update online busy play") or name.startswith("update start")
    if not name.startswith("update") or name == "update none" or online_only:
        def check_online():
            p = []
            src = open(os.path.join(d, "src.c")).read() if os.path.exists(os.path.join(d, "src.c")) else ""
            if name == "update online" and (src != "new\n" or open(os.path.join(d, "data", "keep.bin"), "rb").read() != b"user data"):
                p.append("the downloaded release was not installed (src.c %r) or data/ changed" % src)
            if name in ("update online 404", "update online badsha", "update online off", "update online prerelease", "update online busy play") and src != "old\n":
                p.append("src.c changed")
            if name == "update online badsha" and os.path.isdir(os.path.join(d, "updates", "downloads")) and \
                    any(f.endswith(".zip") for f in os.listdir(os.path.join(d, "updates", "downloads"))):
                p.append("a zip with the wrong SHA-256 was kept")
            if name == "update online off" and SRV.requests:
                p.append("Check GitHub for updates off, but GitHub was asked: %s" % SRV.requests)
            if name.startswith("update start"):   # [UIT 0320] the start-up check: never a download, never an install
                if src != "old\n":
                    p.append("src.c changed")
                if any("/dl/" in r or "/assets/" in r for r in SRV.requests):
                    p.append("the start-up check downloaded something: %s" % SRV.requests)
                asked = any("/releases" in r for r in SRV.requests)
                if name in ("update start off", "update start online off", "update start relaunch", "update start game running", "update start search") and asked:
                    p.append("GitHub was asked although it must not be: %s" % SRV.requests)
                if name in ("update start newer", "update start same", "update start ratelimit", "update start after game", "update start prerelease") and not asked:
                    p.append("GitHub was not asked")
                lg = os.path.join(d, "logs", "update-check.txt")
                if name == "update start game starts" and (not os.path.exists(lg) or "stopped: the game started" not in open(lg).read()):
                    p.append("game starts: no stopped line in logs/update-check.txt")
                if name == "update start offline" and (not os.path.exists(lg) or "online\terror" not in open(lg).read()):
                    p.append("offline: no error line in logs/update-check.txt")
            if name == "update online" and not any("/releases/latest" in r for r in SRV.requests):
                p.append("GitHub was not asked")
            return p
        if name.startswith("update"):
            return ph, check_online
        return ph, lambda: [] if name != "report" or any(f.endswith(".zip") for f in os.listdir(ph["OUT"])) else ["no report zip"]
    where = os.path.join(os.path.dirname(d), "picked" if name == "update pick" else "")
    os.makedirs(where, exist_ok=True)
    pkg = os.path.join(where, "UltraInstinctTears-src-test.zip")
    code = 1 if name == "update rollback" else 0   # update online zip / uptodate / ratelimit: this local zip too
    with zipfile.ZipFile(pkg, "w") as z:
        z.writestr("UltraInstinctTears/VERSION.txt", "Ultra Instinct Tears 0.0.0-test (commit abc1234)\n")
        z.writestr("UltraInstinctTears/src.c", "new\n")
        z.writestr("UltraInstinctTears/src/x.c", "int x;\n")
        z.writestr("UltraInstinctTears/data/keep.bin", "must not land")
        z.writestr("UltraInstinctTears/tools/setup.py", "import sys\nprint('[1/2] lift', flush=True)\nprint('[2/2] build', flush=True)\nsys.exit(%d)\n" % code)
    ph["PKG"] = pkg

    def check():
        p = []
        if open(os.path.join(d, "data", "keep.bin"), "rb").read() != b"user data":
            p.append("data/ was changed")
        want = "old\n" if code or name in ("update pick", "update online ratelimit", "update online uptodate") else "new\n"
        if open(os.path.join(d, "src.c")).read() != want:
            p.append("src.c is not " + want.strip())
        return p
    return ph, check

SRV = None   # [UIT 0281] the mock api.github.com every update case talks to: no case ever reaches the real GitHub

def main():
    global SRV
    if len(sys.argv) < 2:
        sys.exit("usage: test_launcher.py LAUNCHER")
    exe = os.path.abspath(sys.argv[1])
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests"))
    from mock_github import MockGitHub
    SRV = MockGitHub().start()
    env = dict(os.environ, SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy",
               ASAN_OPTIONS="detect_leaks=0:abort_on_error=0", UBSAN_OPTIONS="print_stacktrace=1",
               UIT_UPDATE_API=SRV.api, UIT_UPDATE_TIMEOUT="3")
    env.pop("UIT_GITHUB_TOKEN", None)
    bad = 0
    for name, dlls, cfg, script, keep in CASES:
        d = tempfile.mkdtemp(prefix="ut-launcher-test-")
        check = None
        if name.startswith("update") or name == "report" or name.startswith("mods") or name.startswith("pgo"):
            d = os.path.join(d, "game")
            os.makedirs(d)
            ph, check = (mods_setup if name.startswith("mods") else helper_setup)(name, d)
            for k, v in ph.items():
                script = script.replace("{" + k + "}", v)
                cfg = cfg.replace("{" + k + "}", v)
        if name in ("lift found", "lift some"):   # [UIT 0070] what tools/lifter/scan.py writes
            os.makedirs(os.path.join(d, "data", "generated"))
            open(os.path.join(d, "data", "generated", "lift-features.ini"), "w").write(
                "build=277178b7dba1b6d4\nquality=1\nquality_note=0x1\nfxaa=1\nfxaa_note=0x2\n" +
                ("lod=1\nlod_note=0x3\nshadow=1\nshadow_note=0x4 (confirmed in game)\nshadow_probes=0\n" if name == "lift found" else
                 "lod=0\nlod_note=build id differs\nshadow=0\nshadow_note=3 candidates\nshadow_probes=3\n"))
        for f in ["UltraInstinctTears.exe"] + (["nvngx_dlss.dll", "sl.interposer.dll"] if dlls else []):
            open(os.path.join(d, f), "wb").close()
        path = os.path.join(d, "ultrainstincttears.cfg")
        old = os.path.join(d, OLD_CFG)
        if name == "old cfg name":
            open(old, "w", newline="").write(cfg)
        elif name == "old and new cfg":
            open(old, "w", newline="").write(cfg)
            open(path, "w", newline="").write("TOTK_FPS=30\n")
        elif cfg is not None:
            open(path, "w", newline="").write(cfg)
        # [UIT 0320] offline: a port nothing listens on (connection refused at once)
        cenv = dict(env, UIT_UPDATE_API="http://127.0.0.1:9") if name == "update start offline" else env
        r = subprocess.run([exe, "--dir", d, "--size", "1280x720", "--script", script + ";save;quit"],
                           env=cenv, capture_output=True, text=True, timeout=120)
        out = r.stdout + r.stderr
        problems = []
        if r.returncode != 0:
            problems.append("exit code %d" % r.returncode)
        problems += [l for l in out.splitlines() if "FAILED" in l or "Sanitizer" in l or "runtime error" in l]
        if check:
            problems += check()
        if name.startswith("old"):
            if open(old, newline="").read() != cfg:
                problems.append(OLD_CFG + " was changed (it must stay as the backup)")
            if ("copied" in out) != (name == "old cfg name"):
                problems.append("migration log line " + ("missing" if name == "old cfg name" else "unexpected"))
        if keep:
            text = open(path, newline="").read() if os.path.exists(path) else ""
            problems += ["comment lost: " + k for k in keep if k not in text]
            if cfg and "\r\n" in cfg and "\n" in text.replace("\r\n", ""):
                problems.append("line endings mixed")
        print(("ok    " if not problems else "FAIL  ") + name)
        for p in problems:
            print("        " + p)
        if problems:
            bad += 1
            print("\n".join("      | " + l for l in out.splitlines()[-40:]))
    print("%d of %d launcher cases failed" % (bad, len(CASES)) if bad else "all %d launcher cases passed" % len(CASES))
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()

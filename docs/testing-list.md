# Testing list for this build (batch-1031-final + sweep 0200–0208 + CPU 0190–0193)

What's in it: everything you've had since `250f698` (floating ground 0158, horse naming, amiibo and cheats, overlay and
Controls rebinding), plus the new sweep fixes (0200–0208) and the new CPU options (0190–0193, **off** until you try
them in H). Do the groups in order. If something goes wrong in a group, use About > Report a problem
right away (see I), then carry on with the next group.

**About FPS numbers:** this build still runs with troubleshooting on: crash reports with function names, the
shadow-size check and the per-frame log lines. Any Advanced > Troubleshooting or Developer switch you turned on earlier
(GPU crash checkpoints, Bounds-checked GPU reads, Vulkan validation layer, anything under Developer: logging) is also
still on. So FPS here is **not** what a clean build would get. Only compare numbers taken at the same spot with the same
settings; don't compare them with other emulators.

## A. Install and first launch
1. Launcher: About > Install update, and pick the new `UltraInstinctTears-src.zip`. Its `VERSION.txt` gives the commit
   (see the report). Your saves, settings and game data stay.
2. **This rebuild takes longer than usual.** The game-code converter changed, so setup converts all of the game's code
   again (often 10–30 minutes). Keep the launcher open until it says it is done.
3. Use **Play with log** for every run in this list.
4. Expected: the game starts and gets to the title screen and your save with no crash.

## B. Floating ground (0158)
1. Check that Performance > Stutter > Textures > Split triangle strips where the game splits them is **On** (the
   default). Textures is a folded group: open it.
2. Load at a spot where the slabs used to show: Lookout Landing, Hyrule Field, or a lake or river shore. Look around
   and walk over those places. Expected: no slabs in the air or over water, and the ground you see is the ground you walk on.
3. Log: `[UIT 0158] the game sets primitive restart on, index 0xffff` and `[UIT 0158] strips and fans restart at the
   all-ones index`. If the first line is missing, note it; the fix still applies.
4. Optional A/B: turn that switch Off, press Play, go to the same spot (the slabs should be back), then turn it On again.

## C. Horse naming
1. Register a horse at a stable. Expected: the naming keyboard opens over the game, with no crash.
2. Type a name on the keyboard, or use the controller grid (A types, + is OK), then confirm. Expected: the horse keeps
   the name. Log: `[swkbd] answered (...)`.
3. Open it again and cancel (Esc, or − on the controller). Expected: the game goes back, with no crash.

## D. Amiibo and Limitless amiibo scanning
1. In the game, turn amiibo on (+ > System > Options). Hold L, choose amiibo, let go of L.
2. Open the overlay (F1, or hold L3 + R3 for 1 second) and go to the Amiibo tab. After your own files (Game > Amiibo >
   Amiibo folder), the list shows built-in figures as "Name · built in (no file)". Pick one, e.g. Link. Expected:
   something drops.
3. **Scan twice, cheat off:** make sure Game > Cheats > Limitless amiibo scanning is **Off**, then scan the same
   built-in figure again. Expected: the game says it can't be used again today. diag/amiibo.txt: `... it answered
   "already used today"; Limitless amiibo scanning is off: refused`.
4. **Cheat on:** turn Limitless amiibo scanning **On** (in the overlay's Cheats tab, it takes effect at once) and scan
   the same figure again. Expected: it drops again. diag/amiibo.txt: `... is on: let through`.
5. **Time on the reader (new):** Game > Amiibo > Time on the reader (default 2500 ms). If the game ever misses a scan,
   set it to 5000 and try again. The log at start shows `TOTK_AMIIBO_READER_MS=...` when it is set.
6. Whatever happens, diag/amiibo.txt (in the game folder) logs every step. Send a report if a scan does nothing.

## E. Infinite durability
1. Game > Cheats > Infinite item durability **On** (or in the overlay's Cheats tab). Hit things with a weapon.
   Expected: it doesn't wear out.
2. Turn it **Off**. Expected: weapons wear again.
3. If the switch is greyed out, the build's scan didn't find that code. Note the reason it shows, and send a report.

## F. Overlay and Controls rebinding
1. F1 (Game > In-game overlay > Overlay key) and holding L3 + R3 for 1 second (Game > In-game overlay > Overlay
   controller combo and Controller hold time) both open and close the overlay. Q / E or LB / RB change tabs.
   Expected: the game ignores input while the overlay is open.
2. Controls tab: rebind one controller button and one keyboard key, try them in the game, then Reset controller and
   Reset keyboard. The launcher shows the same bindings under Game > Controller buttons and Game > Keyboard keys
   (both are folded groups: open them).
3. Change one live setting (e.g. Volume), close the overlay and check it took effect. Then close the game and check
   the launcher shows the same value.
4. Optional: Game > In-game overlay > Frame rate counter **On** shows FPS (you need it for H).

## G. New sweep fixes (on by default; just watch for these)
All of these are under Advanced > Compatibility switches, a folded group (open it). Leave them at their defaults
for the first runs.
1. **Stale textures after area loads** (0201, New pools count as written: On). Fast travel 3–4 times between
   different regions, and go in and out of a shrine or cave. Expected: no textures from the previous area on
   the ground or on objects, and no textures that stay blurry or wrong. If you see one, take a screenshot, then turn
   the switch Off for one run at the same spot to compare.
2. **Lines** (0203, Game's line width: On). The game draws some lines 4.5 pixels wide; it used to be 1. We don't know
   which effect they belong to. Note anything that now looks thicker or different. Compare Off and On only if
   something looks wrong. Log: `SetLineWidth 4.5: recorded`.
3. **The image view count** (0200, Image view cache fix: On). In the log, `per frame: image views created ...`
   should be small and should mostly stop after the first seconds in one area. Before, a view was made and thrown
   away every draw.
4. Also on, nothing to look for unless something breaks: Free one-use samplers (0207) and Depth bias by polygon
   mode (0204, only changes objects drawn as lines or points). If decals, shadows or texture filtering look wrong,
   turn that one Off for a run and compare.
5. Optional A/B: Flat shading from the last vertex (0205, **Off** by default). Turn it On for one session on
   the same route. Report any surface whose colour changes or that looks faceted, both On and Off. If nothing
   changes, leave it Off.
6. **Play as smooth as Play with log, and no console window** (0259). Before, Play opened a black console window
   behind the game and wrote the game's log into it one character at a time, which slowed the game and made it
   hitch; clicking in that window could freeze the game until a key was pressed. Now:
   - Play: no console window. Play a few minutes on a busy route; it should feel the same as Play with log (or
     better). Nothing is written to logs\ (as before).
   - Play with log: no console window either; logs\ultrainstincttears-<date>-<time>.log is written as before.
   - Double-click UltraInstinctTears.exe itself (no launcher): no console window; `lasrecomp.log` appears beside the
     exe, and About > Report a problem adds it to the zip.
   - If the game stops with an error, its message box still appears (and only one).

## H. CPU speed test (separate step, do this last)
This changes the game's code, so do it only after A–G look fine.
1. **Baseline (options off, as installed):** Game > In-game overlay > Frame rate counter On. Route: Lookout Landing
   around noon (many people), then glide toward Hyrule Field. Pick 3 fixed spots and stand still 10 seconds at each.
   Note the FPS at each spot. Keep the frame rate target, upscaler, resolution and window mode the same for every run.
2. **Turn the options on** (all under Performance > CPU, a folded group: open it):
   - Faster game code translation: **On**
   - Flags from the compared values: **On**
   - Game registers kept apart from game memory: **On**
   - Write back only the registers a function changes: **On** (the default)
   - Registers kept apart in every function: **Off** (the default)
   - Leave everything else in that group as it is.
3. **Re-convert:** in the launcher, About > Install update, choose the same `UltraInstinctTears-src-….zip` you
   installed, then **Install anyway**. That runs `tools/setup.py`, which translates the game's code again because the
   CPU+ translation options changed (it keeps a stamp of them and of the translator; with nothing changed it skips that
   step). Keep the launcher open until it says it is done. (Command line instead, from the game folder:
   `deps\python\python.exe tools\setup.py`.) This takes a few minutes or more. Its log (logs\update-*.log) says
   `restrict: N function definitions take Cpu *__restrict c` and
   `memory read again at N loop headers, M tail calls and 0 function entries`.
4. **Measure:** Play with log, same route, same 3 spots. Note the FPS at each spot.
5. **Retrain PGO** (the code changed, so an old profile no longer fits). **After any re-convert (step 3) the PGO
   fast build must be redone:** first About > Faster game code (PGO) > **Back to the normal build**, then Steps 1-3:
   About > Faster game code (PGO) > Step 1:
   build the profiling version. Step 2: press Play and play 10 minutes or more the way you normally do. Step 3:
   build the fast version. Then measure the same 3 spots again.
6. **Compare** steps 1, 4 and 5 (on vs off, at the same spots).
7. **If it freezes** (the picture stops while the sound keeps playing, or a loading screen lasts more than a minute):
   close the game (Alt+F4, or Task Manager), start the launcher and press About > Report a problem. Then:
   - Set Game registers kept apart from game memory **Off**, reinstall the same zip again (About > Install update > Install anyway, as in step 3) and retest.
   - If it still freezes, also set Faster game code translation **Off** and reinstall the same zip again (as in step 3). That is back to the
     old code. Send a report either way.
   - Same for crashes, or for people or physics acting oddly.
8. Optional (12900K): Performance > CPU > Core placement (hybrid CPUs): "Game and graphics on performance cores,
   background on efficiency cores". No setup run is needed. Compare the low points on the same route.

## J. Settings search (1.0.0, launcher only)
Every settings tab (Graphics, Performance, Game, Mods, Advanced) has a search field at the top right of the list.
1. Keyboard: on Performance press **Ctrl+F** (or **/**) and type `fps`. "Frame rate target" should be first, with the
   matched letters marked, and "N matches · Also: Graphics … Game …" under the title. Try `rate frame` (any order),
   `p-cores` / `pcores`, a cfg key such as `TOTK_TEX_GRACE_MS`, and `zzqx` (should say "No settings match").
2. **Down** or **Enter** goes to the first result. Change it with Left/Right, then **Up** on the first result to return to
   the field. **Esc** on a result returns to the field, **Esc** clears, and **Esc** again leaves. The focus should stay on
   the setting you changed, and folded groups (Performance > Stutter > Textures, for example) should be folded again.
3. Mouse: click the field and type. The **(x)** clears. Click a result. Click "Also: Game 2" to jump to that tab's matches.
4. Xbox controller: press **View** (the two-windows button left of the Xbox button). The keyboard opens in the right
   panel. Use the **D-pad / left stick** to move, **A** to type, **X** to delete, **Y** for a space, and **Menu** for Done
   (jumps to the results). **B** closes the keyboard. On the field, **A** reopens the keyboard, **B** clears and **B**
   again leaves. On a result, **B** goes back to the search. **LB / RB** still change tabs. The bottom bar shows the buttons.
5. Each tab keeps its own words: search on Graphics, switch to Game and back, and the words should still be there.
   Mods tab: search a mod's name; it should only list that mod, and the mod order cannot be moved while searching.
6. Nothing else should change: navigate, change and reset settings with the search empty exactly as before.

## K. Updates from GitHub (1.0.0, launcher only)
The launcher checks GitHub when you press **About > Install update** and (since 1.0.1, see S) once in the background
when it starts. Never while the game runs, so it cannot cost FPS. Settings are in **Advanced > Updates and reports**: **Check GitHub for updates** (On),
**Include pre-releases** (Off), **GitHub repository for updates** (empty = the official one).
1. With the game closed, press **About > Install update**. While the repository is private, the dialog should say
   "GitHub: could not check: no published release found … may still be private" and then offer your local zips as before
   (or "No update package found" if there are none). Nothing should freeze; at worst it gives up after 30 s.
2. Turn **Advanced > Updates and reports > Check GitHub for updates** Off and press **About > Install update** again:
   only local zips, no GitHub line.
3. Offline: unplug or disable the network, turn the setting back On and try again. You should see "could not reach
   GitHub … install from a zip instead", and the local zip install should still work.
4. While the game is running, **About > Install update** should refuse ("Close the game first"). While the check or
   download is running, **Play** should refuse too ("Looking for an update" / "An update is being downloaded: play when it is done").
5. Once there is a public release (or with a test token, see RELEASING.md), **About > Install update** should show
   "Update available: v…" with the release notes, Installed now / New version / Download rows. **Install** downloads with a
   progress bar, then the same install and rebuild as a zip. **Later** closes the dialog, and **Use a zip instead**
   picks a local zip.

## L. Confirm button, Guide for the overlay, one launcher (1.0.0, 0270-0272)
1. First open: delete the `TOTK_UI_CONFIRM` line from `ultrainstincttears.cfg` (next to UltraInstinctTears.exe) and
   open the launcher. "Which button should confirm?" shows two panels, **Standard** (bottom button) and **Follow the
   game** (right button), with the pad's buttons drawn. Try mouse (click a button), keyboard (Left / Right, Enter) and
   controller (press the button you want to confirm with: bottom = Standard, right = Follow the game). Esc closes it
   without saving, and it asks again next time.
2. Standard: the bottom button (A / Cross) selects, the right one (B / Circle) goes back, in lists, dialogs, the search
   keyboard, and the bottom bar hints. Switch to **Game > Controller > Confirm button (menus): Follow the game**:
   the right button selects and the bottom goes back at once, and the hints swap. The game's own buttons are unchanged.
3. Overlay: Play, open it, check confirm / back / the footer hints follow the same choice (repeat with the other choice;
   the overlay picks it up at the next Play).
4. **Game > In-game overlay > Overlay controller combo: Guide / PS / Home button (hold)**: hold it ~1 s in the game, the
   overlay opens; a short press does nothing. With Game Bar's controller button **on** (Settings > Gaming > Xbox Game
   Bar), note whether Game Bar opens too (expected), then turn it off and check again. Try **L3 + R3 or the Guide / PS /
   Home button**: both open it. With Steam running, note if Steam's overlay takes Guide.
5. One launcher: with the launcher open, start UltraInstinctTearsLauncher.exe again (double-click). No second window;
   the open one comes to the front. Minimize it and try again: it is restored. Press Play (it minimizes), start the
   launcher again: it comes back, showing the game as running, and the game keeps running.
6. Update relaunch: About > Install update with a newer zip, then **Play** (it was **Restart launcher** before 0378):
   the new launcher opens (not just the old one coming forward) and starts the game, and only one window is left.

## M. Less waiting inside a frame (1.0.0, 0254-0258)
All under Performance > CPU (a folded group: open it). All On by default except where noted; each one can be turned
Off for a run to compare. Use Play with log, Lookout Landing at noon, Advanced > Developer: profiling > Frame time
breakdown On.
1. **Look only at GPU drawings over a buffer read's memory** (0254), **Copy GPU results the game reads back with the
   frame that draws them** (0255), **Queue the game's small texture writes instead of waiting for the queue** (0256).
   In the log, `[critical path] queue drains by cause` should show far less for "GPU copies for game threads" and
   "WriteTexels". Watch for a texture, light or exposure that looks out of date or flickers: set that one Off and send
   the log.
2. **Bring the game's memory up to date chunk by chunk so the GPU starts sooner** (0258; Off by default since 0312,
   after the 2026-11-01 A/B: no earlier first work, more lock waits, slower frames). To re-test, A/B: one run On, one Off,
   same route. Compare `[UIT 0258] recording threads: per submission the first work reaches the GPU X ms` (lower is
   better) and the FPS. If "later from draws" in the frame line climbs a lot, try **How many chunks ahead memory pools
   are brought up to date: 3**. Watch for a flicker or an object that is a frame out of date.
3. **Let thread interruption find the game's code function by function** (0257). Only does something with **Interrupt
   long-running game threads** On (Off by default, leave it so for normal play). The log now says whether it started
   (`asynchronous preemption: N lifted functions in M exact ranges`) and which thread wait is in use (`thread waits:
   WaitOnAddress ...` or `condition variables ...`).

## N. Stick deadzones and trigger press point (1.0.0, 0290-0291)
1. **Game > Controller > Left stick deadzone** and **Right stick deadzone** show "Off (as before)" (0 %), and
   **Game > More controller settings > Stick outer deadzone** "Off (as before)", **Trigger press point** 50 %. Search
   `deadzone` finds all three deadzones (and Gyro deadzone). Change them with the controller (Left / Right) too.
2. In the game, open the overlay, Game tab: the same four rows. Set Left stick deadzone to 30 %: small pushes no longer
   move Link, a bigger push starts him walking slowly (no sudden jump), a full push still runs. Same for the camera with
   Right stick deadzone. Back to 0: exactly as before.
3. Stick outer deadzone 20 %: full run is reached a little before the edge. Trigger press point 10 %: a light touch on
   ZL / ZR counts; 90 %: only a full pull does. Back to 50 %.
4. With saving on, close and reopen the launcher: it shows what the overlay set. The log has
   `[UIT 0290] sticks: deadzone left X%, right Y%, outer Z%; triggers press at T%`.

## O. Quit game and Restart game from the overlay (1.0.1, 0292-0295)
1. Play. Open the overlay (F1, or hold L3 + R3), go to the Overlay tab (LB / RB or Q / E). The last two rows are
   **Restart game** [Restart…] and **Quit game** [Quit…]. The panel shows 8 rows, so the list scrolls down to them
   (D-pad / arrows or the mouse wheel) with a thin bar on the right; at 1920x1080 the whole panel stays on screen (0321).
2. **Quit…** with the controller: the dialog "Quit to desktop? Progress since your last save will be lost." shows, with
   **Cancel** focused. Confirm right away: it cancels, the game goes on. Open it again, back button: cancelled. Again,
   D-pad Right to **Quit**, confirm: the game closes within a few seconds. Expected: the launcher comes back with "The
   game closed normally", no error dialog. Try once with the other Confirm button (menus) setting too.
3. Same with the keyboard (Enter, Left / Right, Esc) and the mouse (click Quit; a click outside the dialog or a right
   click cancels).
4. **Play with log**, change a live setting in the overlay (e.g. Frame rate counter On, saving on), then **Restart…** >
   **Restart**. Expected: the game closes and starts again by itself, logging to a new file in `logs\` (the old log
   ends with `[totk] restart from the overlay`), the setting is still as you set it. With plain **Play** it comes back
   as Play (no log).
5. Optional: start `UltraInstinctTears.exe` directly (no launcher), Restart: it starts itself again.
6. The log never shows `closing took over 12 s` on a normal quit; if it does, send the log.

## P. Frame-time graph, benchmark, background frame rate (1.0.1, 0301-0303)
1. **Game > In-game overlay > Frame-time graph** On (or the overlay's Overlay tab): a small graph appears under the FPS
   counter. Walk around: bars stay low and even; a stutter shows as a tall red bar. Off: it is gone.
2. Press **Shift+F8** (or overlay, Overlay tab > Start benchmark): "Benchmarking 60 s" counts down in the top right.
   After a minute it shows the average FPS and lows and where it was saved. Open `benchmarks\` next to
   `UltraInstinctTears.exe`: a .txt report and a .csv. Shift+F8 again during a run stops it early (the report says so).
   **Game > In-game overlay > Benchmark length** changes the length (10-300 s).
3. **Performance > Frame rate > Frame rate in the background** shows 30 FPS (default). At the 60 FPS target, Alt+Tab
   to another window: GPU use drops, the music keeps playing normally. Click back into the game: smooth again at once.
   Try Off, 15 and 60 from the overlay's Performance tab. At the 30 FPS target, 15 slows the game and its sound to half
   speed while away (expected; 30 does nothing there).

## Q. Settings that need a restart (1.0.1, 0304)
1. In the launcher, Performance tab: most settings have a small **restart** tag after the name, settings the overlay
   can change have none. Searching shows the same tags. The details of a setting say "Takes effect when the game
   starts", or "Also in the in-game overlay". Settings used only by setup say **setup**.
2. Press Play, then switch back to the launcher while the game runs: the top shows "Game running", and changing
   a setting shows "Takes effect after restarting the game" at the bottom (for an overlay setting: "... (the in-game
   overlay changes it now)"). Close the game and restart it: the change is in effect.
3. While the game is in front, the launcher uses next to no CPU or GPU (it only redraws while it is the front
   window).

## R. No dips when the game patches a big texture (1.0.1, 0310-0311)
1. **Performance > CPU > Upload only the written rows of changed textures** and **Performance > CPU > Write only
   the changed region of a texture the game patches** are On by default (they sit in the folded advanced CPU group;
   search `texture` finds both).
2. Play with log where the 1.0.0 dips were (open the map, set or move a marker, then run and glide around the area
   for a minute or two). Frame time should stay flat: no 0.3-0.7 s bursts of 33-45 ms frames.
3. The log has `[UIT 0310] texture ... only the written rows uploaded again, X KiB of Y KiB` (X far below Y) and
   `[UIT 0311] WriteTexels of texture ... only the region laid out`.
4. Look at the ground and the map where the game changes things (marks, cut grass, map reveal): nothing out of date or
   blocky. If something is, set the first setting Off, then the second, and send a log for each.

## S. Check for updates when the launcher starts (1.0.1, 0320)
**Advanced > Updates and reports > Check for updates when the launcher starts** is On by default. It only runs when
**Advanced > Updates and reports > Check GitHub for updates** is On too.
1. Close the game and open the launcher. Nothing pops up and the launcher stays smooth. After a few seconds, if GitHub
   has a newer release, a small gold dot shows on the **About** tab and the bottom line says
   "Update available: v1.0.x - About > Install update". **About > Install update** also says "v1.0.x is out on GitHub".
   Up to date or offline: no dot, no message. Either way `logs\update-check.txt` has the answer.
2. Press **Play** within a few seconds of opening the launcher: the check stops (`logs\update-check.txt` ends with
   "stopped: Play was pressed") and runs again a few seconds after the game closes.
3. With **Include pre-releases** On, the line can name a pre-release; Off, only full releases.
4. Turn the setting Off and reopen the launcher: no `update-check.txt` is written and GitHub is not asked. Same with
   **Check GitHub for updates** Off.
5. After an update installs and the launcher restarts itself, it does not check again on that start.
6. Search `launcher starts` finds the setting. It has no **restart** tag (it only affects the launcher).

## T. A calm update screen that never looks stuck (1.0.4, 0430-0431, 0378, 0418)
The launcher that is open during an update is the old one: the new screen shows from the update after the one that
brings it. The update that brings it already writes one short line that older launchers show, e.g. "Preparing the
game - 42% - 5:34 elapsed" ("Almost done - 97% - 12:10 elapsed" near the end), with the clock moving every second.
1. **About > Install update** (reinstalling the same zip with **Install anyway** works too). The window says
   **Installing the update**, one plain status line ("Checking files (1 of 4)", "Preparing the game (2 of 4)",
   "Building the game (3 of 4)", then "Almost done" in the last stage or from 95%), a bar with the percent at its
   right, "Elapsed 5:34" under it, and "You can keep using your PC. Please keep this window open." A light runs over
   the bar all the time, even when the percent stays put.
2. [0418] On the left, the steps in plain words, ticked off as they finish, the current one marked: Checking the
   update, Copying the new files, Checking the game files, Packing the game's data,
   Translating the game code, Compiling the game, Translating the shaders, Preparing the textures, Finishing up
   ([0440] "Backing up your game" comes after Checking the update only when you answered "Back up, then update" to
   "Back up the game first?", which each update asks after Install; About > Rebuild the game: Getting ready, then the same seven). Under the elapsed time, the
   count of the step when it has one ("1,200 of 4,512 files"), and a small meter with "CPU 87%" that changes every
   second. Nothing names a tool, a process, a file type or a `TOTK_...` option, there are no thread counts, and there is
   no time-left estimate anywhere, only the time so far.
3. **Show details** (closed at first) opens the last lines of the build as they come, with **Open log**. With a
   controller: left / right and A.
4. Only after 3 minutes with no new output AND no CPU use by the update does a gold note say "This is taking longer
   than usual. It may still finish; you can leave it running.", with **View details** and **Report a problem**;
   nothing is stopped. A long quiet step that is still working never shows it.
5. The window title and the taskbar button show the percent. The PC does not go to sleep during the update. Pressing the
   window's close button once only warns; a second press within a few seconds closes it.
6. When it ends while the launcher is in the background, the taskbar button flashes and Windows plays its notification
   sound. A finished update says **Update installed** ("Your settings and saves are kept.") with **Play** (the new
   launcher opens and starts the game) and **Close**. A failed one turns the taskbar button red until the message is
   closed and says **The update couldn't be installed** / "Your game is back to how it was.", with **View details**
   (the update's log) and **Report a problem**. A failed download says **The update couldn't be downloaded** /
   "Nothing in your game was changed."
7. Faster game code (PGO) builds show the same screen (the same stages, the percent from setup.py's steps).

## U. Rebuild the game from the launcher (1.0.4, 0389)
Settings that only a rebuild puts into the game are tagged **rebuild** in their row (Performance > CPU+ (folded):
the translation and compile switches such as Optimize the big functions with unresolved jumps; Advanced >
Compatibility switches+ (folded) > Row lookup cache). The first update or rebuild with this version records what the
game was built with; before that there is no notice.
1. Change one of them, e.g. Performance > CPU+ (folded) > Optimize the big functions with unresolved jumps: Off (on by default since 0447). The
   bottom line flashes "This change needs the game to be rebuilt: About > Rebuild the game", then stays on (gold)
   "Some changes need the game to be rebuilt. Rebuild now: About > Rebuild the game" (clicking it opens the rebuild),
   and a gold dot shows after About. Set it back: "Back to how the game was built: no rebuild needed for this one",
   and the line and the dot go.
2. **Play** with a change waiting asks **Some changes need the game to be rebuilt** ("Optimize the big functions with
   unresolved jumps only takes effect after the game is rebuilt. ..."), with **Rebuild now**, **Play without them**
   and **Cancel**. Play without them starts the game and does not ask again for the same changes.
3. **About > Rebuild the game** ("Applies settings that need a rebuild. Your saves and settings are kept.", or which
   settings are waiting) asks **Rebuild the game?** with the waiting settings listed, then shows the update screen
   titled **Rebuilding the game** (section T). Nothing is downloaded and no zip is needed.
4. Done: **Game rebuilt** / "Your settings and saves are kept." with **Play** and **Close**; the notice is gone.
   Failed (or closed half way and rebuilt again): **The game couldn't be rebuilt** / "Your game is back to how it
   was." with **View details** and **Report a problem**; the notice stays.

## V. Button prompts that match the controller (1.1.0 test build 2, 0452-0453)
Launcher **Game > Controller > Button prompts**. Restart the game after every change. Check the log
(UltraInstinctTears.log) for lines starting `[totk] button prompts:`; send them back whatever you see.
1. **Auto (match my controller)** (the default): start with an Xbox pad connected (Xbox look), quit, connect a
   DualSense, start again (PS5 look). Unplug every controller and start: the last look stays. Log: `Auto: ...`.
   Never had a controller connected: the Switch look, as before.
2. **Xbox**, default Controller buttons: talk to someone / pick something up. The action prompt shows **B** where it
   showed A (the button you press on an Xbox pad), the corner button guide too. In a menu, the hints along the bottom
   (text prompts) show A/B/X/Y swapped the same way, and LB, RB, LT, RT, Menu (three bars) and View (two squares) instead
   of L, R, ZL, ZR, + and -. The jump hint (Switch X) shows **Y**.
3. **PlayStation 4** and **PlayStation 5**: the same places show Circle for confirm, Cross for back, Triangle for jump,
   Square, L1, R1, L2, R2, Options, and Share (PS4) or Create (PS5). The letter next to the action prompt stays A/B/X/Y.
4. **Switch (the game's own)**: everything looks exactly as before and the log has no button prompts lines.
5. Change a binding (Controller buttons, e.g. Switch A on the bottom button) and the prompt follows the button.
6. Anything that stays Switch-looking or looks broken (a missing or garbled icon, wrong size, text overlapping): a
   screenshot plus the log lines. First start after a change takes a few seconds longer (the fonts are prepared once).

## W. The mouse turns the camera and aims the bow (1.1.0 test build 2, 0451, 0454)
Launcher **Game > Mouse** (Mouse camera on by default). Play with the mouse and keyboard; a controller may stay connected.
1. Move the mouse slowly, then quickly over the same distance: the camera turns about as far both times, at 30 and at
   60 FPS. The pointer is hidden while you play; F1 or Alt+Tab gives it back, clicking into the game takes it again.
2. Left click attacks, right click draws the bow. While the bow is drawn the aim follows the mouse exactly, with no top
   speed and no drift (needs Motion controls on and the game's Aim with motion controls on, both the default). Let go
   to fire. With Motion controls off the bow still aims, like the camera.
3. Ultrahand, Fuse, Ascend, Recall and throwing aim with the mouse too.
4. Overlay (F1), Game tab: Mouse sensitivity, Bow aiming sensitivity and Invert up and down change at once.
5. **Game > Keyboard keys**: open the Y row and press the middle mouse button: the middle button now attacks in game.
   Reset under the group puts U and the left button back. Any mouse button can press any Switch button.
6. If moving the mouse with the bow drawn turns the aim the wrong way or tilts it: **Game > Mouse+ > Bow aiming style**,
   try Flat or Upright, and say which one works.

## X. Buttons that open the overlay (1.1.0 test build 2, 0455)
Overlay tab, first rows: **Buttons that open this menu** and **Choose the buttons yourself**. The keyboard key (F1) always works.
1. Controller connected. Pick **Minus + Plus (View + Menu)** with the arrows. The footer says "Hold ... to keep it - L3 + R3 works for N s".
   L3 + R3 still opens and closes the overlay now. Nothing is in ultrainstincttears.cfg yet (no TOTK_OVERLAY_PAD line).
2. Hold the new buttons for a second (inside the overlay or with it closed): the overlay opens or stays open, and the cfg gets
   TOTK_OVERLAY_PAD=back+start within a second. L3 + R3 no longer opens it. The launcher shows the same choice (Game > In-game overlay).
3. Pick **Guide / PS / Home button (hold)** and do not touch the pad: after 30 s the footer says "Not used in time: back to ..." (Guide not seen)
   and the old buttons still work. If Windows' Game Bar opens on the Guide press, that is why it can be missed (launcher text explains).
   On an Xbox pad the log says "[overlay] Xbox Guide button: read through XInput".
4. **Choose the buttons yourself**: hold e.g. LB + RB + Minus together, let go. It becomes a trial like step 1 and is kept once used.
   A, B, X, Y or the D-pad are refused with a note; so is one game button alone; Esc or a click cancels; 8 s without a press cancels.
5. Reset: delete the TOTK_OVERLAY_PAD line (or pick L3 + R3 (both stick clicks) and use it once).
6. The hint texts ("F1 or hold ... to close", the Keys row) show the buttons in use and the controller's names (Xbox button, PS button, Home button).

## I. What to send back
- After any group that goes wrong, and once at the end: launcher **About > Report a problem**. It packs the logs
  from Play with log, the crash report if there is one, and diag/amiibo.txt.
- With it, a line per group: A–G (and J, K, L, M, N, O, P, Q, R, S, V, W, X) pass or fail plus what you saw, screenshots for B and G1 if anything looks off,
  and for H the FPS at the 3 spots for steps 1, 4 and 5, plus whether it froze.

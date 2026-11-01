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

## J. Settings search (next update, launcher only)
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

## K. Updates from GitHub (next update, launcher only)
The launcher checks GitHub only when you press **About > Install update**, never at start-up and never while the game
runs, so it cannot cost FPS. Settings are in **Advanced > Updates and reports**: **Check GitHub for updates** (On),
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

## L. Confirm button, Guide for the overlay, one launcher (next update, 0270-0272)
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
6. Update relaunch: About > Install update with a newer zip, then **Restart launcher**: the new launcher opens (not just
   the old one coming forward), and only one window is left.

## M. Less waiting inside a frame (next update, 0254-0258)
All under Performance > CPU (a folded group: open it). All On by default except where noted; each one can be turned
Off for a run to compare. Use Play with log, Lookout Landing at noon, Advanced > Developer: profiling > Frame time
breakdown On.
1. **Look only at GPU drawings over a buffer read's memory** (0254), **Copy GPU results the game reads back with the
   frame that draws them** (0255), **Queue the game's small texture writes instead of waiting for the queue** (0256).
   In the log, `[critical path] queue drains by cause` should show far less for "GPU copies for game threads" and
   "WriteTexels". Watch for a texture, light or exposure that looks out of date or flickers: set that one Off and send
   the log.
2. **Bring the game's memory up to date chunk by chunk so the GPU starts sooner** (0258). A/B: one run On, one Off,
   same route. Compare `[UIT 0258] recording threads: per submission the first work reaches the GPU X ms` (lower is
   better) and the FPS. If "later from draws" in the frame line climbs a lot, try **How many chunks ahead memory pools
   are brought up to date: 3**. Watch for a flicker or an object that is a frame out of date.
3. **Let thread interruption find the game's code function by function** (0257). Only does something with **Interrupt
   long-running game threads** On (Off by default, leave it so for normal play). The log now says whether it started
   (`asynchronous preemption: N lifted functions in M exact ranges`) and which thread wait is in use (`thread waits:
   WaitOnAddress ...` or `condition variables ...`).

## N. Stick deadzones and trigger press point (next update, 0290-0291)
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

## I. What to send back
- After any group that goes wrong, and once at the end: launcher **About > Report a problem**. It packs the logs
  from Play with log, the crash report if there is one, and diag/amiibo.txt.
- With it, a line per group: A–G (and J, K, L, M, N) pass or fail plus what you saw, screenshots for B and G1 if anything looks off,
  and for H the FPS at the 3 spots for steps 1, 4 and 5, plus whether it froze.

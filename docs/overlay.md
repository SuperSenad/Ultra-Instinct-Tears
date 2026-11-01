# In-game overlay (UIT 0170-0177)

A small settings panel drawn over the game. It changes only settings that really take effect while the game runs,
and saves them to `ultrainstincttears.cfg` in the launcher's format, so the launcher shows the same values afterwards.

Launcher: **Game > In-game overlay > In-game overlay: on** (default). The same group has the key, the controller
combo, the hold time and the frame rate counter. The About tab's "In game" keys line shows the overlay key and combo.

## Opening and using it

| | Keyboard and mouse | Controller |
|---|---|---|
| Open / close | **F1** (Game > In-game overlay > Overlay key: F2-F6, F12, Insert, Home, End, Page Up, Page Down, Pause) | hold **L3 + R3** for **1 s** (Overlay controller combo / Controller hold time, 300-3000 ms); optionally the **Guide / PS / Home** button, or either |
| Move | arrows or W A S D, mouse hover, wheel | D-pad or left stick (repeats when held) |
| Change / confirm | Enter, Space, left click; Left / Right step values | the bottom face button (A, Cross; B on a Switch pad), or the right one with Confirm button = Follow the game; D-pad Left / Right |
| Tabs | Q / E, Page Up / Page Down, Tab / Shift+Tab, click a tab | LB / RB |
| Close | Esc, Backspace, right click, the hotkey | the other one of those two (B / Circle with Standard), or the combo again |
| Default (Controls tab) | Delete | the left face button (X, Square; Y on a Switch pad) |

Buttons count by position (UIT 0270), the same as in the launcher: **Game > Controller > Confirm button (menus)**
(`TOTK_UI_CONFIRM`, asked the first time the launcher opens) picks Standard (bottom confirms, right goes back) or Follow
the game (right confirms, bottom goes back, as in TotK). The footer hints name the buttons as printed on the connected
pad. Before 0270 the overlay went by the printed letter, so a Switch pad confirmed with A (right); with Standard it
now confirms with B (bottom), and with Follow the game it is A again. The game's own buttons do not change.

F7-F11 keep working while it is open (mark, FSR, sharpening, DLSS, fullscreen). F1-F6 and F12, Insert, Home, End,
Page Up / Down and Pause are not game keys (README "Controls"), so the hotkey cannot collide with the default layout; if
you bound the same key to a Switch button under Game > Keyboard keys, the overlay takes it.

## Controls tab (UIT 0174): controller buttons and keyboard keys

Tabs: Display, Performance, Game, **Controls**, Amiibo, Cheats, Overlay. One row per Switch button (A ... D-pad Right, with what it does
in the game), a **Controller** column (named like the connected pad: Xbox, PlayStation or Nintendo labels) and a
**Keyboard** column. The same 32 settings as **Game > Controller buttons > (button)** and **Game > Keyboard keys >
(button)** in the launcher, saved to the same `TOTK_PAD_*` / `TOTK_KEY_*` lines (removed for the default, `none` when
unbound), and in effect at once.

- Pick a cell (D-pad / arrows, Left / Right for the column, or hover it), then A / Enter / click: "Press a button..." or
  "Press a key..." with an 8 s countdown. The button or key you press next is the new binding; nothing of it reaches
  the game. The button that started it (A, a click) must be let go first, so it is never bound by accident.
- Cancel: Esc, a click or right click (and B when waiting for a key); no press in 8 s also cancels. Unbind: Delete (or
  X when waiting for a key). While waiting for a controller button, every button is a candidate (B included), so use
  Esc or let it time out.
- Already used by another Switch button: as in the launcher, the other button takes this one's old binding (a swap),
  and the message says so ("Switch A: A  ·  Switch B now uses B").
- Default: Delete / X on a cell, or its "Reset" chip (shown on a changed cell, marked with a gold dot); the last row
  has **Reset controller** and **Reset keyboard**. If a swap moved the default to another button, the warning says
  which ("Switch Y uses it too").
- Refused: W A S D and the arrows (they move the sticks), keys the game cannot read, F7-F11 (their own jobs), the
  overlay hotkey (it closes the overlay), and a one-button overlay combo (Share / Capture, the touchpad). L3 + R3 can
  be bound (each one alone still reaches the game, as before).
- With no controller connected the controller column says so and does not change. Saving off: changes last for this
  session only, as the other tabs.

Live path: the window thread rebuilds the lookup tables in a spare buffer and publishes them with one atomic pointer
store (controller) / a pointer swap under the existing input lock (keyboard: one store, the copy is made outside it).
The input path gains no lock and no work; with the overlay closed nothing of this runs.

### Why L3 + R3 held for a second

TotK uses each stick click on its own (left: crouch / sneak; right: the scope) but never both together, and the
Switch layout an Xbox pad follows (`input_map.h`: button by position) maps them to the same two clicks. Both together
for a full second does not happen in play. A ring fills around a "Keep holding to open the overlay" hint while it is
held; any other button pressed (or one stick click let go) cancels it, and the combo buttons stay hidden from the game
until they are let go. Alternatives in the launcher: Share / Capture (misc1), the PlayStation touchpad press, the Guide / PS / Home button
(0271), L3 + R3 **or** the Guide button (two combos separated by a comma, `leftstick+rightstick,guide`: either one,
held alone for the hold time, opens it), or none.
The first stick click does reach the game until the second joins it (the game would crouch for that moment).

### The Guide / PS / Home button (UIT 0271)

TotK has no use for it (the Switch's Home button never reaches a game), so nothing reaches the game while it is held.
Windows and other programs may want it too, and the overlay cannot stop them; SDL still sees the press:
- **Xbox Game Bar**: Settings > Gaming > Xbox Game Bar > "Open Xbox Game Bar using this button on a controller"
  (on newer Windows 11: "Allow your controller to open Game Bar"). On, a press opens Game Bar at once, over the game,
  before the 1 s hold opens the overlay. Turn it off to use Guide for the overlay, or keep L3 + R3.
- **Steam** (Big Picture / Steam Input) opens its own overlay with Guide; **DS4Windows** may take the PS button.
- Some Bluetooth Xbox pads on older drivers do not report Guide at all; L3 + R3 is still there with "L3 + R3 or Guide".
The launcher's choice text says the same.

While the overlay is open the game receives no keyboard input and a connected controller at rest (gyro reads zero).
Everything held is cleared when it opens and again when it closes, and the button that closed it is hidden from the
game until it is let go. The game is not paused (TotK has no pause the host can trigger safely; open the game's own
menu first if you need it stopped).

## Amiibo tab (UIT 0175): amiibo from files on the computer

The game sees one always-connected NFC reader (`src/host/nfp.c`, native `nn::nfp`); the tab puts an amiibo file on it.

- **Choose a file...** opens the system file dialog (SDL3 `SDL_ShowOpenFileDialog`, the game window as parent; filters
  `*.bin;*.json;*.nfc`), starting in the amiibo folder. In fullscreen the dialog can open behind the game: Alt+Tab to it.
- Below it, the **amiibo folder** as a list (D-pad / arrows / wheel, A / Enter / click scans): every `.bin`,
  `.json` and `.nfc` file, sorted, each shown as "Name · file" (or "file · why it can't be used"). The folder is read only when
  the tab is shown (and re-read each time it is reopened, or with Delete / X). Folder: **Game > Amiibo > Amiibo
  folder** in the launcher (`TOTK_AMIIBO_DIR`, default `amiibo` next to the exe).
- [UIT 0181] After the files, the **built-in figures**: every Legend of Zelda series amiibo (Link, Zelda, the
  Champions, the Tears of the Kingdom figures, the Super Smash Bros. ones...), shown as "Name · built in (no file)".
  They need no file at all: the game reads only an amiibo's ID and serial number (TagInfo, ModelInfo), so a figure
  made from the public ID (names table, `src/host/amiibo_names.h`) with a fixed serial number derived from it is what a
  dump would give. The same figure is the same amiibo every time, so the game's once-a-day limit applies to it (and
  **Game > Cheats > Limitless amiibo scanning** lifts it).
- Every step is logged to the run's log and to `diag/amiibo.txt` (UIT 0180): the folder read, each scan and why one
  was refused, the game's nfp calls and their answers, the figure arriving and being taken off, and the cheat site
  ("once a day" check seen / let through).
- **Scan again** re-presents the last amiibo. The status line says whether the game is asking for one right now, and
  the last scan's result; the last amiibo's name is shown under it.
- Errors, in words: wrong size (not a 532 / 540 / 572-byte dump or JSON), not an amiibo (no amiibo header / ID / serial
  check bytes), JSON without an `AmiiboId`, and "the game isn't asking for an amiibo right now" (the figure then
  waits on the reader for 20 s, so scanning first and opening the game's amiibo menu next also works).

Accepted files: raw NTAG215 dumps (540 bytes; 532 without the password/config page; 572 with the signature) and
Ryujinx-style JSON (`"AmiiboId": "hex16"`, optional `"Name"`) and Flipper Zero `.nfc` text dumps (their `Page N:`
lines are the raw tag pages; UIT 0181). **No Nintendo keys are used, needed or shipped**:
nothing is decrypted. Only the plaintext parts of a dump are read (the 7-byte tag serial and the amiibo ID at 0x54).
The game's application area for each figure is kept in `<profile>/amiibo/<serial>-<amiibo id>.bin` instead of on the
"tag", so a dump file is never written.

In game (TotK): first **+ > System > Options > amiibo: on**; then hold **L** (the ability wheel), pick **amiibo**,
release L, aim and press the button to use it. The game now searches: the overlay says "The game is asking for an
amiibo: scan one now"; scan; the figure stays on the reader 2.5 s, then is removed (as lifting it off the pad), and the
drop / chest appears. The game allows each figure once per in-game day unless **Limitless amiibo scanning** is on.

Flow: `StartDetection` -> device state Searching -> a scan -> Found + the activate event (`hle_signal_system_event_alive`)
-> `GetTagInfo` / `Mount` / `GetModelInfo` / `GetCommonInfo` / `GetRegisterInfo` / `OpenApplicationArea` /
`CreateApplicationArea` / `GetApplicationArea` / `SetApplicationArea` + `Flush` -> after 2.5 s Removed + the
deactivate event. Results are the documented nfp codes (module 115): device not found, invalid state, not mounted, no application area, wrong
access ID, area already exists. With nothing ever scanned, the per-frame work is one flag test.

## Cheats tab (UIT 0175)

| Overlay row | cfg key | Launcher path | What it does |
|---|---|---|---|
| Infinite item durability | TOTK_CHEAT_DURABILITY | Game > Cheats > Infinite item durability (default off) | a hit takes no wear off a weapon, bow or shield, and a worn item reads as full |
| Limitless amiibo scanning | TOTK_CHEAT_AMIIBO | Game > Cheats > Limitless amiibo scanning (default off) | the once-a-day check always answers "not used today" |

Both are native patches in the lifted code (`tools/lifter/patches.py`, optional switched sites), each guarded by one
relaxed atomic load (`src/host/cheats.c`), so they turn on and off live; off runs the game's original instructions.
A build where the lifter did not find the expected instruction words (another game version) keeps the original code,
and the row says "Not in this build". **Mods > Disable Mods** turns them off with the mods (and Enable Mods back on;
they are remembered as `::TOTK_CHEAT_...` lines in `mods/.uit-mods-before-disable.txt`).

Provenance: the addresses and instruction words (TotK 1.4.3, build ID 277178B7DBA1B6D4) are from the entries in
TKMM's `Cheats.json` (MIT; credits @Arch9SK7 / MyNXCheats). Only addresses and words are used; no code was copied.

| Cheat | Site (main+) | Original | Patched |
|---|---|---|---|
| Durability | 0x21eca9c | `msub w8, w10, w9, w8` (0x1b09a148) | `msub w8, wzr, w9, w8` (0x1b09a3e8): wear per hit 0 |
| Durability | 0x282e58, 0x2829f4 | `ldr w1, [x8, #8]` (0xb9400901) | `ldr w1, [x8, #0x18]; str w1, [x8, #8]`: current = max |
| Amiibo | 0x1f87e08 | `tbnz w0, #0, main+0x1f87e44` (0x370001e0) | while on: the branch the first check of the session (a new serial number) took, every time (UIT 0210; `TOTK_CHEAT_AMIIBO_BRANCH=nop` = the public cheat's nop) |

UIT 0210: with Limitless amiibo scanning on, each scan also reaches the game with a new random serial number (GetTagInfo; 04 + 6 bytes): the game keeps its once-a-day record per serial number. The amiibo ID and the application-area file are unchanged.

Known limit: durability lost other than by hits (a weapon burning up in fire, Fuse breaking off) is not covered.

## Text entry: the game's software keyboard (UIT 0177)

When the game asks for text (naming a horse at a stable), it calls `nn::swkbd::ShowKeyboard`, which on a console
starts the keyboard applet. This host has no applet (that was the crash in `CreateLibraryApplet` /
`StartLibraryAppletImmediately`), so both `ShowKeyboard` overloads are answered natively (`src/host/swkbd_host.c`):
the request the game built (`ShowKeyboardArg`: header, sub and guide texts, initial text, min / max length, disabled
characters, keyboard mode, UTF-16 or UTF-8 output; the layout is switchbrew's `KeyboardConfig`, the filters follow the
MIT reference, `src/host/swkbd.c`) is shown as a dialog on the overlay, and the answer is written into the game's
`String`. OK returns 0, cancel returns `ResultCanceled` (0x29f) and an empty string.

| Input | Keys |
|---|---|
| Keyboard | type (IME compositions show underlined), Enter OK (Shift+Enter a new line where allowed), Esc cancel, Backspace / Delete, Left / Right, Home / End, Ctrl+V paste |
| Controller | D-pad / stick move on the grid, A type the key, B delete, X space, Y next page (letters, symbols, accents), L3 shift, L / R move the cursor, + OK, - cancel |
| Mouse | click a key, click the text to put the cursor at the end |

- The text is checked as it is typed: characters the game disabled (a horse name: no `@ % \`), letters in a number
  field, over the maximum. The dialog says why a key did nothing; OK needs at least the minimum (and never empty).
- The game's thread waits in `ShowKeyboard` like a Horizon wait; the window, the overlay and the present keep running
  (the last game picture is shown again under the dialog). The controller and keys do not reach the game while it is
  up, and what was held to press OK stays hidden until let go.
- Text input (and so the IME) is on only while the dialog is up. It works with **In-game overlay: off** too (no menu,
  key or combo then; the dialog only).
- `TOTK_SWKBD_ANSWER=<text>` answers every request without a dialog (`<cancel>`: cancel, `<initial>`: the game's
  text), for scripted runs. With no window at all, the game's initial text is confirmed if it gave one, else cancelled.
- The log has one `[swkbd]` line per request and per answer (texts hidden in password mode).
- Limits: the game's optional text-check callback is not called (the filters above are the check); characters outside
  the overlay font (CJK, ...) can be typed through an IME and reach the game, but show as `?` in the dialog.
- Cost: nothing until the game asks (no thread, no event filter, text input off); one relaxed atomic load per frame.

## Settings: included (verified live)

Each one has a setter in `src/host/uit_live.h`, implemented next to the code that reads the value at each use (an
atomic store picked up by the reading thread, never `setenv`).

| Overlay tab | Setting | cfg key | Launcher path | Why it is live |
|---|---|---|---|---|
| Display | AMD FSR 1 | TOTK_UPSCALER | Graphics > Image quality > Upscaler: AMD FSR 1 | present_fsr.c reads it per present (F8 already did this) |
| Display | NVIDIA DLSS (session only) | - | Graphics > Image quality > Upscaler: NVIDIA DLSS | F10's toggle; the upscaler choice itself is read at start, so it is not saved |
| Display | Sharpening | TOTK_FSR_SHARPNESS | Graphics > Image quality > Sharpening | constants rebuilt per present (F9) |
| Display | Fullscreen | TOTK_FULLSCREEN | Graphics > Display > Start in fullscreen | SDL window state (F11) |
| Display | Window size (windowed only) | TOTK_WINDOW_SIZE | Graphics > Display > Window size | SDL_SetWindowSize; the swapchain follows the resize |
| Display | Present mode | TOTK_PRESENT_MODE | Graphics > More display settings > Present mode | swapchain rebuilt before the next present |
| Display | Triple buffering | TOTK_TRIPLE_BUFFER | Graphics > More display settings > Triple buffering | swapchain rebuilt before the next present |
| Performance | Frame rate target (60 <-> 120) | TOTK_FPS | Performance > Frame rate > Frame rate target | fixed frame step and present clock follow; only when the run started at 60 or 120 |
| Performance | Frame pacing hold | TOTK_INTERVAL_HOLD | Performance > Frame rate > Frame pacing hold | read at each interval decision |
| Performance | Lowest frame rate step | TOTK_MAX_INTERVAL | Performance > Frame rate > Lowest frame rate step | read at each interval decision |
| Performance | Texture upload budget | TOTK_TEX_BUDGET_MB | Performance > Stutter > Textures > Texture upload budget | relaxed atomic per frame (only with Texture streaming on) |
| Performance | Wait for nearly-ready textures | TOTK_TEX_GRACE_MS | Performance > Stutter > More texture settings > Wait for nearly-ready textures | relaxed atomic per frame (same) |
| Game | Volume (new) | TOTK_VOLUME | Game > Sound > Volume | gain at the audio device pull |
| Game | Controller vibration | TOTK_RUMBLE | Game > Controller > Controller vibration | hid_feedback.c flag; on again only if it was on at start |
| Game | Motion controls (gyro) | TOTK_GYRO | Game > Controller > Motion controls (gyro) | same |
| Game | Gyro deadzone | TOTK_GYRO_DEADZONE | Game > More controller settings > Gyro deadzone | read per sensor sample |
| Game | Left / Right stick deadzone (new, 0290) | TOTK_STICK_DEADZONE_L, TOTK_STICK_DEADZONE_R | Game > Controller > Left stick deadzone, Right stick deadzone | relaxed atomic per controller poll (window thread) |
| Game | Stick outer deadzone (new, 0290) | TOTK_STICK_OUTER | Game > More controller settings > Stick outer deadzone | same |
| Game | Trigger press point (new, 0291) | TOTK_TRIGGER_POINT | Game > More controller settings > Trigger press point | same; the overlay's controller combo uses it too |
| Amiibo | Amiibo folder | TOTK_AMIIBO_DIR | Game > Amiibo > Amiibo folder | read each time the tab is shown |
| Cheats | Infinite item durability, Limitless amiibo scanning | TOTK_CHEAT_* | Game > Cheats | relaxed atomic at the patched site |
| Overlay | Frame rate counter | TOTK_OVERLAY_FPS | Game > In-game overlay > Frame rate counter | the overlay's own |
| Overlay | Save changes to the settings file | - | - | off = this session only |
| Overlay | Mark in log | - | - | as F7 |
| Overlay | Capture a frame for diagnosis (new, 0157) | - | - | as Shift+F7: what every draw of the next frame read, to `diag\frame-capture-*.txt` |

## Settings: excluded (not live)

Everything else in `launcher/settings.ini` has no live path in the code: it is read once at start by the subsystem
that uses it, so changing it in game would show a value the game is not using. The main ones:

| Setting (launcher path) | Why not |
|---|---|
| Graphics > Image quality > Upscaler (choosing DLSS vs FSR), DLSS mode, DLSS model, Scene resolution | render targets and the DLSS feature are created at start |
| Graphics > Resolution and shape (output resolution, aspect, HUD placement) | patched into the game's setup at start |
| Graphics > Detail and pop-in, texture filtering, game FXAA, display fixes, console mode, dynamic resolution | game patches / pipeline state chosen at start |
| Performance > Frame rate target 30 <-> 60/120 | the 60 FPS game patches are applied once at start |
| Performance > High frame rate fixes, CPU, Shaders, Textures (streaming on/off, threads, prefetch), Video memory, Texture cache, Memory copies, Loading, Saving | threads, pools, caches and code paths set up at start |
| Game > Language, gyro axis mapping | read at start / when a controller attaches (controller buttons and keyboard keys: the Controls tab) |
| Game > Sound > Mute all sound, audio buffer, clean sound stops; Videos | the audio device and the movie player are set up at start |
| Mods, Advanced (troubleshooting, compatibility, developer) | read at start; changing them mid-run is unsafe or meaningless |
| The overlay's own key, combo and hold time | read when the window opens; change them in the launcher |

## Saving

On (default): each change is written to `ultrainstincttears.cfg` with the launcher's rules (only the changed keys,
other lines and comments kept, the same "-" defaults, FSR written as the launcher does). Off: changes last for this
session; turning it back on saves them. With no settings file loaded, changes last for the session. Environment
variables set outside the cfg still win at the next start, as in the launcher.

## Cost

- Closed: one relaxed atomic load per present (`g_uit_ov_visible`); nothing is created until it is first shown.
- Open: one dynamic-rendering pass after the upscaler into the swapchain image, all quads in one instanced draw
  (48-byte instances, about 630 when the panel is open: ~29 KiB, re-uploaded only when something changes; FPS counter:
  6 quads). Font atlas (the launcher's Inter fonts, SIL OFL 1.1) built once per DPI step: 512x256 at 1080p (1.1 ms),
  1024x384 at 4K (1.9 ms); freed on shutdown. Layout: ~0.003 ms on the window thread.

# Ultra Instinct Tears

A native Windows PC port of *The Legend of Zelda: Tears of the Kingdom* (version 1.4.3), built from your own copy of the game.

[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
![Platform: Windows](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-lightgrey.svg)

Ultra Instinct Tears (UIT for short) started as a fork of [UltraTears](https://github.com/kuraeow/ultratears) by
kuraeow. It isn't an emulator. The game's ARM64 code is translated ahead of time into C and compiled for x86-64 on
your PC. The Switch system and its NVN graphics API are replaced with native code running on Windows and Vulkan.
Shaders and textures get converted when you build, so you're not compiling them while you play.

> **No game files live here.** This repository has no game code, assets, keys, firmware or Nintendo SDK code, and it
> won't tell you where to find any of that. It does nothing until you point it at a copy of the game you dumped
> yourself from your own console. Everything it makes from the game stays on your PC. Please don't share it, and
> please don't open issues asking for it.
>
> This project is not affiliated with or endorsed by Nintendo. You need to own the game. *The Legend of Zelda* and
> *Tears of the Kingdom* are trademarks of Nintendo, and the names only appear here to say which game this works with.

## Built on UltraTears

kuraeow did the hard foundational work: the static recompiler, the NVN-to-Vulkan layer, the builder and the first
playable build. That's the part that made everything else possible, and UltraTears is still credited throughout the
source. Go give it a star.

When I forked it there was no settings app, no tests, and the game ran at its original 30 FPS with a lot of stutter.
This is what I've added since then, counted straight from git:

- 234 commits of my own on top of the upstream commit I forked from (`1970c7a`), carrying 222 numbered changes
  (UIT 0001 to 0321; numbers that don't appear were skipped or used on side branches that aren't in this build).
- 212 new settings, on top of the 113 that were already there. The launcher shows all 325 of them,
  each one with a plain-English explanation.
- 110 test programs and 109 scripted launcher tests, where upstream had none.
- About 66,500 lines added across 359 files, not counting the bundled third-party code.

By subject, most of the work went into graphics (around 87 commits: Vulkan, textures, shaders, DLSS, FSR, and the
see-through or floating ground bugs), then performance and CPU work (about 50), then the launcher and in-game
overlay (about 50). The rest is mods, amiibo and cheats, controller support, crash fixes and tooling.


## What's in it

**Frame rate and performance.** You can run at 30 FPS (the original), 60, or an experimental 120. A lot of the work
went into the CPU side: faster translated game code, better thread placement on hybrid Intel CPUs, lighter frame
submission, and work spread across more threads. There's an optional PGO step (About > Faster game code (PGO)) that
rebuilds the game code using a profile of how you actually play. Most performance switches live under
Performance > CPU and Performance > Stutter, and nearly every one of them can be turned off to compare.

**Graphics.** AMD FSR 1 works on any GPU. NVIDIA DLSS (DLAA, Quality, Balanced, Performance) works on RTX cards
through NVIDIA Streamline, which you download yourself (more on that below). Shaders warm up before the title
screen and textures stream with fewer hitches. A long list of rendering bugs got fixed along the way: exploded
distant polygons, torn meshes, see-through floors, and slabs of ground floating in the air.

**The launcher.** `UltraInstinctTearsLauncher.exe` covers every setting the game reads, across the Graphics,
Performance, Game, Mods and Advanced tabs. Each setting says what it does and what it costs. There's a
"Recommended for this PC" preset, and each tab has a search box so you can jump straight to a setting by name, by
words from its description, or by its config key. It works the same with a controller as it does with a mouse and
keyboard.

**In-game overlay.** Press F1, or hold L3 + R3 for a second on a controller, to change the settings that take effect
live. That includes button and key rebinding, amiibo, cheats, and Quit game / Restart game (Overlay tab).
[docs/overlay.md](docs/overlay.md) has the details.

**Mods.** The Mods tab installs RomFS mods from a .zip, .7z, .rar or a folder, with a load order and an on/off switch
for each one. It uses the same layout Yuzu and Ryujinx mods use. Code patches (.ips, .pchtxt) don't load here, but
FPS and resolution are built-in settings anyway.

**Amiibo and cheats.** Amiibo can be scanned from NTAG215 files on your PC. Four optional cheats can be switched on
and off while you play: infinite item durability, limitless amiibo scanning, infinite stamina and infinite arrows.
They're all off by default, and when they're off the game runs its own code untouched. The cheat addresses come from
the community cheat list in TKMM's `Cheats.json` (MIT licensed, credited to @Arch9SK7 / MyNXCheats); I took the
addresses and instruction words from there and patch them into the game code at build time.

**Screenshots.** F12 saves what's on screen as a PNG in your Pictures folder, under `Ultra Instinct Tears`, without
the overlay and without the black bars. It's written in the background, so the game doesn't hitch. You can change
the key or add a controller combo under Game > Screenshots. The Capture / Share button is deliberately left alone,
since Windows, Steam and the controller software already use it.

**Save backups.** Every time you press Play, the launcher zips your save folder into the `backups` folder next to the
game and keeps the newest 10 (Game > Saves > Save backups; 0 turns it off). If nothing changed since the last
backup, it doesn't make a new one, so restarting a few times won't push your older backups out. About > Open save
backups takes you straight there.

**Mute in the background.** Turn on Game > Sound > Mute in the background and the game goes quiet when you
alt-tab away or minimise it, then comes back when you return. It's off by default.

**Updating and reporting.** The launcher checks GitHub for a new release, installs it from its source zip, rebuilds
only what changed, and rolls back on its own if anything fails. About > Report a problem packs your logs into one zip for a bug report.

## Requirements

- Windows 10 or 11, 64-bit, and a CPU with AVX2 (Intel Haswell, AMD Ryzen or newer).
- A GPU with a current Vulkan 1.3 driver that supports `VK_EXT_shader_object`.
- 16 GB of RAM. 32 GB is what I use.
- About 32 GB free for the build, plus about 16 GB for your game dump.
- Python 3 (tick "Add python.exe to PATH" when installing) and, if you want to clone, Git.
- Your own dump of Tears of the Kingdom with update 1.4.3, as two plain folders: `exefs` and `romfs`. Encrypted
  packages (.nsp, .xci, .nca) won't work. UIT doesn't decrypt anything and doesn't read keys.

> **Tested hardware.** I've only ever tested this on one PC: an Intel Core i9-12900K, an NVIDIA RTX 3080 Ti and 32 GB
> of RAM. If you're on an AMD CPU, an AMD GPU or Intel Arc, honestly, I don't know how it'll behave. It might be fine
> and it might not. Reports from other hardware are very welcome, good or bad.

There's no Mac or Linux version. I didn't have the time to optimize the Windows port this quickly and build Mac and
Linux ports on top of that. The Windows build also doesn't run under Proton or Wine right now.

## First-time setup

1. Get the source. Either clone it:

   ```powershell
   git clone https://github.com/SuperSenad/Ultra-Instinct-Tears.git
   cd Ultra-Instinct-Tears
   ```

   or download `UltraInstinctTears-src.zip` from [Releases](https://github.com/SuperSenad/Ultra-Instinct-Tears/releases)
   and extract it to its own folder.

2. Put your dump where the build can find it. You can copy it into the folder as `game\exefs` and `game\romfs`, or
   leave it where it is and pass the folder that contains `exefs` and `romfs` with `--dump`.

3. Build:

   ```powershell
   python tools/setup.py --output D:\Games\UltraInstinctTears --dump D:\Dumps\TOTK-1.4.3
   ```

   The first run downloads the build tools it needs (Python, a Clang toolchain and the shader translator, about
   150 MB) from the UltraTears builder release. No game files are ever downloaded. On an 8-core PC the whole build
   takes about half an hour. It's done when it says `Done`, and if it gets interrupted, just run the same command
   again and it picks up where it left off. From an extracted zip there's no `.git` folder, so it builds right
   there in that folder. Leave `--output` off in that case.

4. Open `UltraInstinctTearsLauncher.exe` in the build folder, look through the settings, and press Play.

For DLSS, run `python tools/setup.py --get-dlss` once. It fetches NVIDIA's Streamline SDK (pinned version, checked by
hash) from NVIDIA's own GitHub release. Without it, picking DLSS falls back to FSR 1, and setup tells you so.

[BUILDING.md](BUILDING.md) has every option and goes into how the build works.

## Updating

Use **About > Install update** in the launcher. It checks GitHub Releases for a newer version and downloads it. The
launcher also checks once in the background when it opens, and puts a dot on About when there's something new. You
can turn that off under Advanced > Updates and reports > Check for updates when the launcher starts. If
you'd rather use a zip you already have, put `UltraInstinctTears-src.zip` next to your game folder (or inside it) and
pick "Use a zip instead". Either way it backs up what it replaces, rebuilds, and puts everything back if the
build fails. Your saves, settings and built game data stay put. If you cloned with Git, `git pull` and run the same
`setup.py` command again instead.

## Settings worth knowing

The launcher's tabs are Play, Graphics, Performance, Game, Mods, Advanced and About. Folded groups hold the more
technical switches, so the defaults stay readable. Every setting has a short explanation with it. Settings are
saved to `ultrainstincttears.cfg` in the game folder, if you'd rather edit them by hand. To find something fast, press Ctrl+F or `/` on a keyboard, or
View on a controller (that one brings up an on-screen keyboard), and start typing.

In game, F1 or holding L3 + R3 opens the overlay. Both the key and the controller combo can be changed under
Game > In-game overlay, and that's also where you can switch to the Guide / PS / Home button instead. F7 to F11 work while you play (mark, FSR, sharpening, DLSS, full screen).

Default keyboard layout: WASD moves, the arrow keys move the camera, J = A, K or Space = B, I = X, U = Y, Q / E = L / R,
left Alt / Shift = ZL / ZR, Enter = +, Tab or Esc = − (map). Controllers work through SDL3, and the game's buttons go by
position, like on a Switch. In the launcher and the overlay, the confirm button is your choice (Game > Controller >
Confirm button (menus)). Everything can be rebound in the launcher or the overlay.

## Performance notes

How it runs depends a lot on your CPU, since the game's code runs natively and a fair bit of the work is
single-thread bound. A few things help:

- Start from "Recommended for this PC" and change things one at a time.
- Do the PGO steps once you're happy with your settings (About > Faster game code (PGO)). Redo them after big updates.
- Use Play with log when comparing, so you have the numbers afterwards.
- 60 FPS is the target I care about. 120 is there, but it's experimental.

## Something went wrong?

Run with **Play with log**, reproduce the problem, then use **About > Report a problem**. It packs the game's logs,
the crash report if there is one, and your settings into one zip. Open an issue, attach that zip, and say what
hardware you're on. The bug report template asks for exactly that.

If the build stops: a message about `exefs\main` or the RomFS not being 1.4.3 means the dump is the wrong version or
modded. A Vulkan error or a black window almost always means the graphics driver needs updating.

## A note from me

I'm SuperSenad, and I made this in my free time. I didn't take a single donation while working on it. If it made
your game better and you'd like to buy me a coffee, my [tip jar is here](https://streamlabs.com/supersenad1/tip).
It's completely optional: tips don't unlock anything, and every build stays free for everyone.

I'll be upfront: I used AI-assisted "vibe-coding" where it made sense. The amount of research, implementing and
testing in this project would have taken me years otherwise. That doesn't make it fake. The work is genuine: I play-test the builds on my own PC,
and the performance changes are measured in game, with each one behind its own switch so it can be compared and
turned off. If something doesn't hold up on your setup, tell me and I'll look at it.

## Contributing

Bug reports with a Report a problem zip are the most useful thing right now, especially from AMD and Intel Arc
hardware. Pull requests are welcome. Keep them focused, explain what you measured, and never include anything made
from the game (translated code, shaders, textures, dumps or keys). The tests live in `tools/tests/`, and
`tools/test_launcher.py` drives the launcher headlessly.

## License

GPL-3.0-or-later, same as UltraTears. See [LICENSE](LICENSE). The upstream copyright notices are kept. Bundled
third-party code keeps its own license (MIT for the Ryujinx shader translator, Dear ImGui and several smaller pieces,
and the SIL Open Font License for the Inter font). The full list is in
[third_party/README.md](third_party/README.md). NVIDIA Streamline and DLSS aren't included here; `setup.py --get-dlss`
downloads them from NVIDIA under NVIDIA's own license.

There's also a [Russian README](README.ru.md). It started as upstream's and is older than this one, so trust the English version if they disagree.

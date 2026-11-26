# Ultra Instinct Tears


A native Windows PC port of *The Legend of Zelda: Tears of the Kingdom* (version 1.4.3), built from your own copy of the game.

[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
![Platform: Windows](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-lightgrey.svg)

A native Windows PC version of *The Legend of Zelda: Tears of the Kingdom* (version 1.4.3), built around performance, customization, and giving you control over how the game runs.

If you want to play at higher frame rates, tune the graphics to your hardware, use your preferred controller layout, install mods, or change settings without digging through config files, that's what I've been building UIT for.

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


## Why try UIT?

The goal was never just to get the game running on Windows. I wanted to make it something you can actually tune to your PC and the way you like to play.

- **Go beyond the original 30 FPS.** Choose 30 or 60 FPS, with an experimental 120 FPS option.
- **Make the most of your GPU.** Use AMD FSR 1 or NVIDIA DLSS, including DLAA and multiple DLSS quality modes on supported RTX cards.
- **A launcher with real control.** More than 300 settings, clear explanations, search, presets, and controller navigation.
- **Change things while you're playing.** Open the in-game overlay to adjust live settings, rebind controls, scan amiibo, toggle cheats, or restart the game.
- **Bring your mods.** Install and manage RomFS mods, choose their load order, and turn individual mods on or off.
- **Keep your saves safer.** Automatic save backups before launching, with up to 10 recent backups.
- **Less time troubleshooting updates.** The launcher checks for updates, rebuilds what needs rebuilding, and can roll back if an update fails.
- **Useful tools built in.** Take clean screenshots, mute the game when you tab out, and package logs into a report when something goes wrong.

Everything is free. There are no paid features or unlocks.

## What's in it

### Performance and frame rate

The original game runs at 30 FPS. UIT gives you options to target **30 or 60 FPS**, plus an **experimental 120 FPS mode** if you want to try pushing further.

A lot of the work has gone into the CPU side of the game: improving translated game code, spreading work across more threads, improving thread placement on hybrid Intel CPUs, and reducing overhead when submitting frames. There are also options for stutter and CPU behavior, so you can compare settings on your own system instead of being stuck with one set of tweaks.

If you want to take it further, the launcher includes **Profile-Guided Optimization (PGO)**. It rebuilds the game code using a profile collected while you play, allowing the compiler to optimize around the way the game actually runs. You can find it under **About > Faster game code (PGO)**.

Most of the performance options are under **Performance > CPU** and **Performance > Stutter**. Nearly every tweak can be switched off individually.

### Graphics, upscaling, and visual fixes

Choose the image quality and upscaling approach that makes sense for your hardware:

- **AMD FSR 1** is available across GPUs.
- **NVIDIA DLSS** supports DLAA and Quality, Balanced, and Performance modes on supported RTX cards. DLSS requires NVIDIA Streamline, which you can download through the setup tool.
- Shader warm-up happens before the title screen to reduce the amount of shader compilation you run into while playing.
- Texture streaming is tuned to reduce interruptions.
- A long list of rendering problems has been fixed, including exploded distant polygons, torn meshes, see-through floors, and floating or missing-looking ground.

There are also options for sharpening, resolution, and other rendering behavior. You can experiment with them from the launcher or, where supported, from the in-game overlay.

### A launcher that puts the settings in your hands

The launcher is a big part of UIT. It isn't just a Play button with a few graphics options.

There are **325 settings** across the Graphics, Performance, Game, Mods, and Advanced sections. Each setting has an explanation of what it does, and many include notes about the trade-offs.

You also get:

- A **Recommended for this PC** preset to give you a starting point.
- Search across setting names, descriptions, and config keys.
- Controller navigation as well as mouse and keyboard support.
- Options for graphics, CPU behavior, stutter, controls, sound, screenshots, saves, mods, updates, and more.
- A readable layout that keeps the more technical options tucked into expandable groups until you need them.

You can change settings in the launcher or edit `ultrainstincttears.cfg` directly if that's more your style.

### In-game overlay

You don't have to quit the game every time you want to change something.

Press **F1**, or hold **L3 + R3** for a second on your controller, to open the overlay. You can change settings that apply live, rebind keyboard and controller inputs, scan amiibo, toggle cheats, and use the overlay's quit or restart options.

The overlay is designed to work with a controller, too. You can change its activation button and choose a different menu confirm button. See [docs/overlay.md](docs/overlay.md) for the full details.

### Controller and keyboard customization

Controllers work through SDL3, and the game uses button positions like a Switch controller. You can rebind controls in the launcher or in the overlay, including keyboard and controller inputs.

The default keyboard layout is:

| Action | Key |
|---|---|
| Move | WASD |
| Camera | Arrow keys |
| A | J |
| B | K or Space |
| X | I |
| Y | U |
| L / R | Q / E |
| ZL / ZR | Left Alt / Shift |
| + | Enter |
| − / Map | Tab or Esc |

You can also choose which button confirms selections in the launcher and overlay.

### Mods

The Mods tab gives you a straightforward way to manage **RomFS mods**:

- Install from a `.zip`, `.7z`, `.rar`, or an extracted folder.
- Enable or disable each mod.
- Set the load order.
- Use the same general mod folder layout used by Yuzu and Ryujinx.

Code patches such as `.ips` and `.pchtxt` are not supported by the mod manager. Frame rate and resolution options are already available as built-in settings.

### Amiibo and optional cheats

You can scan amiibo using NTAG215 files stored on your PC. No need to go through a separate tool every time you want to use one.

There are also four optional cheats you can toggle while playing:

- Infinite item durability
- Limitless amiibo scanning
- Infinite stamina
- Infinite arrows

They're **off by default**. When a cheat is off, the game runs its own code untouched.

### Screenshots

Press **F12** to save a screenshot as a PNG in your Pictures folder, inside **Ultra Instinct Tears**.

Screenshots are captured without the in-game overlay or black bars, and the image is written in the background so taking one shouldn't interrupt the game. You can change the screenshot key or add a controller combo under **Game > Screenshots**.

The Capture / Share button is left alone because Windows, Steam, and controller software may already use it.

### Automatic save backups

UIT backs up your save folder whenever you press **Play**. Backups are stored in a `backups` folder next to the game, and the launcher keeps the newest **10** by default.

It checks whether your save has changed before creating another backup, so repeatedly restarting the game won't fill the folder with identical copies. You can change the backup count or turn the feature off under **Game > Saves > Save backups**. Use **About > Open save backups** to get to the folder.

### Small quality-of-life features

A few other things that make day-to-day play nicer:

- **Mute in the background:** Automatically mute the game when you alt-tab or minimize it, then restore sound when you return.
- **Quick display controls:** F7 to F11 provide shortcuts for options such as marking, FSR, sharpening, DLSS, and fullscreen.
- **Update checks:** The launcher can check for new releases when it starts and let you know when an update is available.
- **Problem reports:** Package logs, crash information, and settings into one zip so you can attach it to a bug report.

### Updating and recovery

You can install updates directly through **About > Install update**. UIT checks GitHub Releases, downloads the update, and rebuilds what changed.

The updater backs up files it replaces and attempts to roll back if the build fails. Your saves, settings, and built game data are kept in place. You can also use a source zip you already downloaded, or update manually with Git if you cloned the repository.

## Requirements

- Windows 10 or 11, 64-bit, and a CPU with AVX2 (Intel Haswell, AMD Ryzen or newer).
- A GPU with a current Vulkan 1.3 driver that supports `VK_EXT_shader_object`.
- 16 GB of RAM. 32 GB is what I use.
- About 32 GB free for the build, plus about 16 GB for your game dump.
- Python and Git aren't needed for the installer. Only the [advanced route](#advanced-install-from-the-source-zip) needs Python 3 (tick "Add python.exe to PATH" when installing) and, to clone, Git.
- Your own dump of Tears of the Kingdom with update 1.4.3, as two plain folders: `exefs` and `romfs`. Encrypted
  packages (.nsp, .xci, .nca) won't work. UIT doesn't decrypt anything and doesn't read keys.

> **Tested hardware.** I've only ever tested this on one PC: an Intel Core i9-12900K, an NVIDIA RTX 3080 Ti and 32 GB
> of RAM. If you're on an AMD CPU, an AMD GPU or Intel Arc, honestly, I don't know how it'll behave. It might be fine
> and it might not. Reports from other hardware are very welcome, good or bad.

There's no Mac or Linux version. I didn't have the time to optimize the Windows port this quickly and build Mac and
Linux ports on top of that. The Windows build also doesn't run under Proton or Wine right now.

## First-time setup

1. **Download** [UltraInstinctTears-Setup.exe](https://github.com/SuperSenad/Ultra-Instinct-Tears/releases/download/installer/UltraInstinctTears-Setup.exe).
   It looks up the newest release on GitHub when it starts, so you always get the current version.
2. **Run it.** Windows may warn about an unknown publisher, because the installer isn't code-signed. Choose
   **More info**, then **Run anyway**.
3. **Pick an install folder.** Use a path without spaces. You need about 32 GB free.
4. **Point it at your game files:** the folder that holds `exefs` and `romfs` from your own dump of Tears of the
   Kingdom 1.4.3. It checks they're the right version before it does anything.
5. **Wait.** It downloads the build tools (about 150 MB) and builds the game on your PC, which takes about half an
   hour on an 8-core PC. You can keep using your PC meanwhile.
6. **Press Play.**

If something goes wrong, it says so plainly. Try again picks up where it stopped, and Close removes everything it put
on your PC. Updates come through the launcher (see [Updating](#updating)). Running the installer again repairs an
install and keeps your saves and settings. No Python or Git is needed. The installer brings what it needs.

## Advanced: install from the source zip

For when you'd rather not use the installer, or you want to build from a Git clone.

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

For DLSS, tick "Also get DLSS for your NVIDIA card" in the installer, or run `python tools/setup.py --get-dlss` once. It fetches NVIDIA's Streamline SDK (pinned version, checked by
hash) from NVIDIA's own GitHub release. Without it, picking DLSS falls back to FSR 1, and setup tells you so.

[BUILDING.md](BUILDING.md) has every option and goes into how the build works.

## Updating

Use **About > Install update** in the launcher. It checks GitHub Releases for a newer version and downloads it. The
launcher also checks once in the background when it opens, and puts a dot on About when there's something new. You
can turn that off under Advanced > Updates and reports > Check for updates when the launcher starts. If
you'd rather use a zip you already have, put `UltraInstinctTears-src.zip` next to your game folder (or inside it) and
pick "Use a zip instead". Either way the launcher first asks "Back up the game first?" (the answer is for that one
update only). "Update without backup" is the quick default: the files it replaces are still kept until the update is done,
so a failed build puts everything back. "Back up, then update" also keeps a full copy of your current game, so you can go
back after the update has finished, and takes longer. Your saves, settings and built game data stay put. If you cloned with Git, `git pull` and run the same
`setup.py` command again instead.

## Settings worth knowing

The launcher's tabs are Play, Graphics, Performance, Game, Mods, Advanced and About. Folded groups hold the more
technical switches, so the defaults stay readable. Every setting has a short explanation with it. Settings are
saved to `ultrainstincttears.cfg` in the game folder, if you'd rather edit them by hand. To find something fast, press Ctrl+F or `/` on a keyboard, or
View on a controller (that one brings up an on-screen keyboard), and start typing.

In game, F1 or holding L3 + R3 opens the overlay. Both the key and the controller combo can be changed under
Game > In-game overlay, and that's also where you can switch to the Guide / PS / Home button instead. F7 to F11 work while you play (mark, FSR, sharpening, DLSS, full screen).

Default keyboard layout: WASD moves, the arrow keys move the camera, J = A, K or Space = B, I = X, U = Y, Q / E = L / R,
left Alt / Shift = ZL / ZR, Enter = +, Tab or Esc = − (map). The mouse turns the camera and aims like in a PC game
(left click = Y, attack; right click = ZR, bow, which then follows the mouse exactly through the game's motion aiming;
sensitivity under Game > Mouse, the mouse buttons under Game > Keyboard keys; Alt+Tab or F1 gives the pointer back). Controllers work through SDL3, and the game's buttons go by
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

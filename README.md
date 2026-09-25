# Project Bumble
A native PC and Steamdeck port of **Buck Bumble**, with widescreen support, modern graphics, a plethora of QOL enhancements,
and keyboard, mouse and controller controls. This is hobby project originally built for my own playthrough, now shared because sharing is caring.
While feedback and bug reports are welcome, please keep your virtue signalling and code quality objections to reddit.

## Download and play

Get the latest build *FuzzyBumble* from [Releases](https://github.com/riseio/project-bumble/releases).
You need your own **Buck Bumble (USA), revision 0** ROM. Select it when prompted
on first launch. `.z64`, `.n64`, and `.v64` byte orders are supported.
No ROM or game assets are included.

On first launch, choose whether to generate enhanced textures (about 120 MB).
Select **Not now** to skip them, or **Don't ask again** to remember your choice.
Generation shows progress and can be cancelled; later launches use the cache.
You can restore the prompt from **Options > Graphics / Effects**.

- **Windows:** Run `FuzzyBumble_<version>.exe`. No installer or separate DLLs needed.
- **Steam Deck:** Make `FuzzyBumble_<version>.AppImage` executable, add it to Steam
  as a non-Steam game, and use the Gamepad controller layout.
- **Linux:** The AppImage is the simplest option. The bare
  `FuzzyBumble_<version>` executable is also available, but needs its runtime
  libraries installed.

Windows requires an x86-64 CPU with SSE4.1 and a compatible D3D12 or Vulkan GPU.
Linux requires x86-64 with AVX2 and F16C.
The AppImage includes its C/C++ runtime, SDL, Vulkan loader, and audio/windowing
libraries. It still needs a supported Linux kernel, a desktop session, and
working graphics drivers on the host.

Saves, settings, and generated textures live in `userdata` beside the executable
or AppImage. Keep that folder when updating or moving the game. On the first
portable launch, existing user-profile data is copied there without deleting
the original. The game folder must be writable.

Game-managed caches and temporary files also stay in `userdata`; there is no
fallback to AppData. Linux shader-cache locations are redirected there where the
driver supports it. AppImage mounting/extraction, OS crash reports, graphics-driver
services and desktop file-picker history are managed by the system, not the game.
Advanced `--data-root` and `--controller-pak-root` options explicitly override
their respective locations; diagnostic output paths must stay inside the data root.

## Enhancements

- Widescreen and ultrawide support, with an adapted HUD and menus.
- Original 30 Hz or interpolated 60/120 Hz rendering.
- Mouse aiming and dual-stick flight controls, with direct buttons for maneuvers.
- Remappable controls, stick-layout options, sensitivity, deadzone and inversion settings.
- Separate mouse sensitivity for each axis, with optional acceleration.
- Cutscene text speeds of 1x, 2x, and 4x (default).
- Modern menus with keyboard, mouse and controller navigation.
- Updated weapon labels, boss health display and damage feedback.
- Optional modern lighting, water effects, terrain textures and grass.
- Cycle original, modern, and modern with enhanced textures during play.
- Local saves and settings, with no account or online connection needed to play.

## PC controls

| Input | Action |
|---|---|
| WASD / mouse | Move / aim |
| Left click | Fire |
| Space | Take off / land |
| F / V | Fly straight up / down |
| Shift | Sprint |
| Ctrl | Barrel roll |
| Mouse 4 | Loop-de-loop |
| R | Quick flip |
| Mouse wheel or Q/E | Change weapon |
| Tab | Cycle visual modes |
| Enter or Escape | Pause |

Controls can be changed in the settings menu.

Enhanced textures use JetForce Capricorn's 9× reconstruction, generated once
from your ROM and cached locally. Original and modern modes remain available.

## Steam Deck controls

Use Steam's **Gamepad** layout. These are the default in-game bindings:

| Input | Action |
|---|---|
| Left stick | Move |
| Right stick | Aim |
| R2 | Fire |
| A | Take off / land |
| L1 / R1 | Previous / next weapon |
| X | Sprint |
| L2 | Barrel roll |
| Left / right stick click | Fly straight up / down |
| B | Loop-de-loop |
| Y | Quick flip |
| Menu (☰) | Pause |
| View (▢▢) | Cycle visual modes |
| A / B in menus | Confirm / back |

Custom bindings override these defaults. Trackpads and rear buttons can be
assigned through Steam Input.

## Disclaimer

Project Bumble is an unofficial fan project not associated or containing the original game in any way.
It is not affiliated with, endorsed by, or sponsored by Nintendo, Ubisoft, Argonaut Software, or any other
rights holder.

**We do not provide the original game.** This repository and its downloads do
not include a Buck Bumble ROM, music, sounds, textures, models, levels, or other
extracted original game assets. We do not provide ROM downloads or links to them.
Game assets are loaded or generated locally from the ROM you supply.

**You must own a legally purchased copy of Buck Bumble and provide your own
lawfully obtained ROM dump** of the supported US revision-0 release to use this
project. The port is not a substitute for owning the original game. Do not
upload or share ROMs or extracted game assets in this repository or its issues.

Buck Bumble, its characters, names, artwork, music, trademarks and all other
original game content belong to their respective copyright and trademark
holders. We claim no ownership of that content. This project grants no rights
to copy or redistribute the original game or its assets.

## Project

Built with N64ModernRuntime, RT64 and JoltPhysics.

[Report a bug](https://github.com/riseio/project-bumble/issues) When raising issues please make sure you include the build version you experienced the bug in ^_^

# Project Bumble
A native PC and Steamdeck port of **Buck Bumble**, with widescreen support, modern graphics, a plethora of QOL enhancements,
and keyboard, mouse and controller controls. This is hobby project originally built for my own playthrough, now shared because sharing is caring.

## Download and play

Get the latest build *FuzzyBumble* from [Releases](https://github.com/riseio/project-bumble/releases).
You need your own **Buck Bumble (USA), revision 0** ROM. Select it when prompted
on first launch. No ROM or game assets are included.

On first launch, choose whether to generate enhanced textures (about 120 MB).
Select **Not now** to skip them and **Don't ask again** to remember your choice.
Generation shows progress and can be cancelled. Restore the prompt from
**Options > Graphics / Effects** if you change your mind.

- **Windows:** Run `FuzzyBumble_<version>.exe`. No installer or separate DLLs needed.
- **Steam Deck:** Make `FuzzyBumble_<version>.AppImage` executable, add it to Steam
  as a non-Steam game, and use the Gamepad controller layout.
- **Linux:** The AppImage is the simplest option. The bare
  `FuzzyBumble_<version>` executable is also available, but needs its runtime
  libraries installed.

Windows requires an x86-64 CPU with SSE4.1 and a compatible D3D12 or Vulkan GPU.
Linux requires x86-64 with AVX2 and F16C.

Saves and settings stay in your user-data folder when you replace the executable.

## Enhancements

- Widescreen and ultrawide support, with an adapted HUD and menus.
- Original 30 Hz or interpolated 60/120 Hz rendering.
- Mouse aiming and dual-stick flight controls, with direct buttons for maneuvers.
- Remappable controls, stick-layout options, sensitivity, deadzone and inversion settings.
- Modern menus with keyboard, mouse and controller navigation.
- Updated weapon labels, boss health display and damage feedback.
- Optional modern lighting, water effects, terrain textures and grass.
- Cycle original, modern, and enhanced textures during play.
- Local saves and settings, with no account or online connection needed to play.
- Mid mission checkpoints.
- Weapon enhancements
- Portal standby indication

## PC controls

| Input | Action |
|---|---|
| WASD / mouse | Move / aim |
| Left click | Fire |
| Space | Take off / land |
| Shift | Sprint |
| Ctrl | Barrel roll |
| Mouse 4 | Loop-de-loop |
| R | Quick flip |
| Mouse wheel or Q/E | Change weapon |
| Tab | Cycle visual modes |
| Enter or Escape | Pause |

Controls can be changed in the settings menu.

Enhanced textures are generated once from your ROM and cached locally.
Enhanced mode is unavailable if you skip generation; Original and Modern
remain available.

## Steam Deck controls

Use Steam's **Gamepad** layout. These are the default in-game bindings:

| Input | Action |
|---|---|
| Left stick | Move |
| Right stick | Aim |
| R2 | Fire |
| A | Take off / land |
| L1 / R1 | Previous / next weapon |
| X or left-stick click | Sprint |
| L2 or right-stick click | Barrel roll |
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

[Report a bug](https://github.com/riseio/project-bumble/issues)

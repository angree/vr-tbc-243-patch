# vr-tbc-243-patch

VR for **World of Warcraft 2.4.3 (build 8606)** — renders the game in stereoscopic 3D to a
SteamVR / OpenVR headset. It is a `d3d9.dll` proxy: it does **not** modify, patch, or include
any game files.

It's a novelty, not a better way to play — you can't see your keyboard, menus and combat are
clumsier, and this old engine draws the world twice (once per eye). But with the multi-core
setup below it now runs smoothly on modest hardware: roughly **GTX 1060-class**, the same as
WoVR needed. Weaker cards work with a shorter draw distance.

## Requirements

- A SteamVR-compatible headset; start SteamVR **before** launching the game.
- Your own WoW **2.4.3 (build 8606)** client — not included.
- GPU: ~GTX 1060 or better recommended; runs on less with a lower draw distance.

## Install

> Back up your WoW folder first — this is experimental.

1. Copy next to `WoW.exe`: `d3d9.dll`, `openvr_api.dll`, the `vr_version` folder, the
   `Interface` folder, and `Set_VR_Mode.bat`.
2. Run **`Set_VR_Mode.bat`** once, with the game closed (see [Performance](#performance)).
3. Start SteamVR, then `WoW.exe`. To go back to flat play: delete `d3d9.dll`.

## In-game settings (VRConfig addon)

The bundled **VRConfig** addon tunes most features **live, in-game** — no file editing, no
restart. Open it with the minimap VR button or `/vr`. It controls world scale, crosshair,
gaze/target highlight, aim assist and nameplate style live as you drag the sliders; the
floating-screen size/distance/height apply on **Apply** with a 20-second "keep or it reverts"
safety. Settings are saved and re-applied on your next login. (You can still edit
`vr_version/vr_config.cfg` by hand — most keys apply within ~2 s.)

## Features

- **World-space nameplates** — names/health sit in real 3D and are hidden behind walls and
  terrain. In VR they stay **level to the horizon** and each **turns to face you**. Three
  styles (Original / 3D golden plate with level + power bars / clean NewPlate), picked in
  VRConfig; **Ctrl+V** toggles the bar panel.
- **World scale** — the "miniature / toybox" look (ships at `0.7`).
- **Crosshair**, **gaze / target highlight**, optional **aim assist**, and a curved
  **floating screen** for the flat UI.

## Performance

- **Run `Set_VR_Mode.bat`** (game closed). It turns on the engine's own multi-core worker
  threads and lets the game use every CPU core — the biggest smoothness win — and sets a clean
  VR display mode (1024x768, fullscreen, 75 Hz). It edits only those few lines of your
  `WTF/Config.wtf`, backs it up first, and leaves your account/settings alone. `INSTALL.txt`
  has the manual equivalent.
- Lower the **Draw Distance** for more frames; shadows off, no sunshafts.
- A black border around the image is normal and shrinks as fps rises.

## Known limitations

- The mouse pointer occasionally loads garbled — restart WoW to fix. It's a single simple
  cursor, not the full per-action set (planned).
- Settings may not save on Alt+F4 — exit through the game menu or type `/reload`.

## Credits & license

Made by **Grzegorz Korycki**. Based on and inspired by
**[WoVR](https://github.com/ProjectMimer/WoVR)** by ProjectMimer; the engine-hook and proxy
code was substantially rewritten for this client, and the Direct3D 9 interface headers
necessarily mirror Microsoft's SDK. WoVR ships no license; any unlicensed code was removed.
Released under the **MIT License** — see [LICENSE](LICENSE).

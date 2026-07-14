# vr-tbc-243-patch

A VR overlay for a **World of Warcraft 2.4.3 (build 8606)** client. It renders the game in
stereoscopic 3D to a SteamVR / OpenVR headset. It is a `d3d9.dll` proxy — it does **not**
modify, patch, or include any game files.

## ⚠️ Read this first — VR makes the game HARDER, not better

This is a novelty / experience, **not a better way to play**:

- **You cannot see your keyboard in VR.** You either touch-type blind or play almost
  entirely with the mouse.
- Menus, chat, and combat are all clumsier than on a flat monitor.
- It renders the whole world **twice** (once per eye), so it is heavy — expect much lower
  fps than flat, and don't expect a smooth raid.

If a slow, awkward, but genuinely-in-VR old-school WoW sounds fun anyway — read on.

## Requirements

- A SteamVR-compatible headset; SteamVR installed and **running / "green" before you launch**.
- A WoW **2.4.3 (build 8606)** client — **not included, bring your own.**
- GPU: **RTX 3060 or better recommended** (runs on less, just slower; to improve reduce draw 
distance and other details).
- Optimizations planned later.

## Install

> **Back up first.** This is experimental. Make a copy of your whole WoW folder (or work on a
> separate copy of the client) and install this there — not on your only working client. If
> anything misbehaves, you still have a clean copy to fall back to.

1. Copy next to `WoW.exe`: `d3d9.dll`, `openvr_api.dll`, and the whole `vr_version/` folder.
   Optional but recommended: copy `addon/VRConfig` into your `Interface/AddOns/` folder
   for the in-game settings panel (see below).
2. Set the game to a **low resolution** (e.g. `1024x768`) in `WTF/Config.wtf`. Windowed or
   fullscreen both work; if the picture goes black in one mode, try the other.
3. Start SteamVR and wait until it is ready (green). Then launch `WoW.exe`.
4. To go back to normal flat play: delete `d3d9.dll`.

## Tuning — `vr_version/vr_config.cfg`

Edit a value, save the file, and the change applies in about **2 seconds — no restart** —
*except* `ui_sharpness`, `world_sharpness` and `screen_curve`, which need a game restart.
Lines starting with `#` are comments. Every option:

**Nameplates** (names / health bars above characters)
- `nameplate_depth` — `1.0` = full 3D (names sit at each character's real depth), `0.0` = flat overlay.
- `nameplate_yshift` — height of the name above the head (small number).
- `nameplate_xshift` — horizontal nudge; only used in flat mode.

**Highlight glow** (the unit under your gaze / your current target lights up)
- `mouseover_color` — glow colour of the unit you are looking at, as `R G B` (each 0–1).
- `mouseover_bright` — how strongly it glows (1–5).
- `target_color` / `target_bright` — same, for your current target.

**Crosshair** (centre aiming dot)
- `crosshair` — `1` on, `0` off.
- `crosshair_size` — half-length in pixels (bigger = larger cross).
- `crosshair_color` — `R G B` (0–1).
- `crosshair_offset` — horizontal calibration (px): nudge the two eyes' crosshairs together/apart until they fuse into one.
- `crosshair_yoffset` — vertical calibration (px): both eyes up / down.

**Aim assist** (makes head-aiming at units more forgiving)
- `aim_assist` — `1` on, `0` off (off = exact centre ray only).
- `aim_rings` — how many wider rings to try if the centre misses (0–4).
- `aim_spread_deg` — angle step per ring in degrees (bigger = casts wider).
- `aim_samples` — probe points per ring (3–16).

**Floating screen** (the flat game image shown inside VR)
- `screen_size` — size of the screen (bigger number = bigger screen).
- `screen_distance` — how far in front of you the screen sits.
- `screen_height` — screen height (up / down).
- `screen_depth` — projection depth of the screen (advanced — leave as is unless you know).
- `screen_curve` — screen shape: `+1` concave (curves around you, the good look), `0` flat, `-1` convex (bulges toward you); in-between = gentler / stronger. **Restart to apply.**

**World scale** (make the whole VR world look smaller or bigger — the "miniature / toybox" effect)
- `world_scale` — apparent size of the 3D world. `1.0` = normal; below 1 makes everything look small / miniature (e.g. `0.25` = a toy-sized, Lego-like world); above 1 makes a giant world. **Ships at `0.7` by default** (`1.0` feels a bit large in VR). **Range 0.1–4.0** (values below 0.1 are ignored). Gameplay is unchanged — only the VR perspective scales. Live (~2s); recentre after a big change.

**Sharpness** (higher = sharper but heavier / lower fps). **Both need a restart.**
- `ui_sharpness` — interface / HUD render scale (1–3; `1` = native, fastest).
- `world_sharpness` — 3D world render scale (1–3; `1` = native, fastest).

## In-game config addon (VRConfig) — change settings without editing files

`addon/VRConfig/` is an **optional** WoW addon that lets you tune the VR settings from a
panel **inside the game** instead of editing the `.cfg`. Copy the `VRConfig` folder into
your `Interface/AddOns/` folder, then:

- Open it with the **minimap button** (the round VR icon) or by typing **`/vr`**.
- Two pages of sliders (**Prev / Next**). Most settings — world scale, crosshair, highlight,
  aim assist, nameplates — apply **live** as you drag them.
- The three *floating-screen* settings (size / distance / height) wait for the **Apply**
  button, which then starts a **20-second "Keep or it reverts"** countdown — so a bad value
  that throws the screen out of view undoes itself automatically.
- **Reset to defaults** restores known-good values (same safety countdown).
- Your settings are saved and re-applied automatically on your next login.

How it works: the addon sends values to the DLL live through the game's own `SetCVar`
(the DLL intercepts names that start with `vr`), so **nothing is written to disk while you
play**. Once you use the addon it becomes the source of truth and your `.cfg` is left alone.

## Performance tips

- **Lower the Draw Distance** in the video options — biggest win by far.
- Reduce particles / ground clutter; shadows off; no sunshafts.
- A black border around the image is normal and shrinks as fps rises.

## Known bugs & limitations

- **The pointer sometimes renders as garbage.** Now and then the mouse pointer loads as a
  mess of pixels. **Fix for now: restart WoW** and it comes back correct.
- **Mouse pointer is an original replacement.** The in-VR pointer is a single simple
  original cursor — the game's own cursor art is not used or included. It is not yet the
  full per-action cursor set (attack / cast / loot / etc.); a custom set is planned.
- **The warm-up pre-pass costs fps.** A throwaway render runs before each frame; it fixes a
  bug where parts of objects go missing in one eye, but it slows frame generation. It is the
  main item on the optimisation list.
- **Settings may not save if you force-close (Alt+F4).** Game and addon settings (e.g. XPerl)
  are written on a clean exit or on `/reload`. If a change won't stick, type `/reload` in
  game after changing it, or exit through the game menu instead of Alt+F4.
- **Low fps overall.** VR renders everything twice; see Performance tips. Optimisation is
  planned but not done.

## Notes

- **No game files are included.** Use your own, legally-obtained client.
- Not affiliated with or endorsed by the game's publisher. For personal / educational use.

## Credits

Made by **Grzegorz Korycki**.

A VR port for WoW 2.4.3, based on and inspired by **[WoVR](https://github.com/ProjectMimer/WoVR)** by ProjectMimer. The engine-hook and proxy code was substantially rewritten for this client; the Direct3D 9 interface headers necessarily mirror Microsoft's SDK.

The original WoVR project does not include a license. Any unlicensed code has been removed.

## License

Released under the MIT License — see [LICENSE](LICENSE).

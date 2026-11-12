<div align="center">

# OpenCE : Forge

**Halo: Combat Evolved for Windows, Linux & Android — Full [OpenCE](https://github.com/OpenCommunityEdition/OpenCE) Port with Integrated 3D Forge Mode (`v2.10.0`)**

<p align="center">
  <img src="port/assets/icon/opence-icon.png" alt="OpenCE" width="128">
</p>

[![Latest Release](https://img.shields.io/github/v/release/SutlabSol/HaloOpenCE-Forge?label=Download%20Release&style=for-the-badge&color=2ea44f)](https://github.com/SutlabSol/HaloOpenCE-Forge/releases/latest)
[![Upstream OpenCE](https://img.shields.io/badge/Upstream-OpenCommunityEdition%2FOpenCE-0078D6?style=for-the-badge)](https://github.com/OpenCommunityEdition/OpenCE)
[![License: AGPL-3.0-or-later](https://img.shields.io/badge/License-AGPL--3.0--or--later-blue?style=for-the-badge)](LICENSE)

[**Download Latest Release**](https://github.com/SutlabSol/HaloOpenCE-Forge/releases/latest) ·
[Overview](#overview--unified-opence-integration) ·
[Forge Menu & Unified Tag Vault](#forge-menu--unified-tag-vault) ·
[In-Game Forge Features](#in-game-forge-features) ·
[Installation](#installation) ·
[Controls](#controls-in-forge) ·
[Building from Source](#building-from-source)

</div>

---

## Overview — Unified OpenCE Integration

**OpenCE : Forge** builds on [**OpenCE** (Open Community Edition)](https://github.com/OpenCommunityEdition/OpenCE), the native C port of the Halo: Combat Evolved Xbox decompilation (`01.01.14.2342`).

**Forge Mode is natively integrated directly into `halo.exe`** alongside the complete Campaign, Standard Multiplayer, Online Co-Op, MCC Map Support, and Mods experience:

- **Full Campaign & Standard Multiplayer Intact:** Play solo or online Co-Op Campaign, browse the 128-player Internet/LAN Server Browser, play SWAT, Infection, Gun Game, or Hardcore, and load Mods or MCC Workshop packages from the main menu just like upstream OpenCE.
- **Unified `SELECT GAMEMODE` Multiplayer Hub:** Selecting **Multiplayer** on the main menu opens the **Select Gamemode** screen with two modes and live right-panel artwork/descriptions:
  - **STANDARD:** Normal OpenCE Multiplayer (Internet Server Browser, LAN, Split-Screen, Create Game). Standard sessions use stock OpenCE network protocol versioning so Forge builds can join stock OpenCE servers seamlessly.
  - **FORGE:** Opens the dedicated Forge Hub (**Solo Forge**, **Multiplayer Forge**, **Edit Maps**, and **Tag Options**).
- **Forging on Xbox, Custom Edition & MCC Multiplayer Maps:** Edit any retail Xbox multiplayer map, Halo Custom Edition multiplayer map (`custom_maps/`), or Halo 1 MCC multiplayer/Workshop map (`mcc_maps/`). *(Singleplayer campaign maps can be opened in the Tag Browser to extract custom tags, while map editing is restricted to multiplayer maps.)*
- **Unified Faber Tag Vault (`.fbrtag` + `faber_vault.bin`):** Custom tags extracted from **Xbox**, **MCC**, and **Custom Edition** maps are stored in a single unified, content-addressed, deduplicated storage format (`tag_profiles/*.fbrtag` + `tag_profiles/faber_vault.bin`). Shared bitmaps and sound permutations across multiple custom tags are stored once in `faber_vault.bin` and paged on demand.
- **Standalone `.map` Baking (`F5` / `Pause -> SAVE MAP..`):** Saving a forged map bakes all placed objects, spawns, objectives, weld prefabs, custom scripts, and imported custom tags directly into a self-contained `.map` file in `custom_maps/` so forged maps play natively in Standard Mode Multiplayer and Dedicated Servers without external tag files.

> [!IMPORTANT]
> **No proprietary game maps or copyrighted assets are included in this repository or its releases.** You must provide your own Halo: Combat Evolved `maps/` (or Xbox `.iso` / `.xiso` disc image), optional Halo Custom Edition `custom_maps/` (`bitmaps.map`, `sounds.map`, `loc.map`), and optional MCC `mcc_maps/`.

---

## Forge Menu & Unified Tag Vault

Navigating to **`MULTIPLAYER -> FORGE`** opens the Forge Hub:

1. **SOLO FORGE:** Choose any Xbox Multiplayer, Custom Edition Multiplayer, MCC Multiplayer, or saved Forge map (`FORGE MAPS` tab) and load directly into an endless single-player Forge editing session.
2. **MULTIPLAYER FORGE:** Host an Internet, LAN, or Split-Screen collaborative Forge lobby where up to 16 players can toggle between **343 Guilty Spark Monitor** (`F1`) and **Spartan** in real time and build together.
3. **EDIT MAPS:** Browse, rename, or delete **User Created** and **Downloaded** Forge maps with preview bitmaps and author metadata.
4. **TAG OPTIONS:** Complete in-menu tag extraction and cache management suite across Xbox, MCC, and Custom Edition maps:
   - **`ADD TAGS`:**
     - Opens a Map Picker (`RETAIL / XBOX`, `CUSTOM SINGLEPLAYER`, `CUSTOM MULTIPLAYER`, and `MCC MAPS`) for browsing any installed map.
     - Selecting any map opens the **Map Tag Browser** across 6 categories (`SCENERY`, `VEHICLES`, `BIPEDS`, `DEVICES`, `WEAPONS`, `EQUIPMENT`).
     - Features a **live rotating 3D wireframe model preview** on the right-hand panel along with `- <tag name>`, `- <tag type>`, and `- <tag size>`.
     - Pressing **`CHOOSE`** prompts you to enter a custom Forge menu name (`[C] <your_name>`), converts MCC/Xbox/CE geometry, shaders, bitmaps, and sounds as needed, and stores the tag and its full dependency closure into the deduplicated **Faber Tag Vault** (`tag_profiles/*.fbrtag` + `tag_profiles/faber_vault.bin`).
   - **`INSTALLED TAGS`:**
     - Browse all tags currently installed in your cache across all categories with the live rotating 3D preview and byte size breakdown.
     - Choosing any tag opens a confirmation dialog allowing you to **delete** individual tags from the vault.
   - **`RESET STOCK TAGS` / `RESET CUSTOM TAGS`:**
     - Confirmation dialogs (`CONFIRM` / `BACK`) to rebuild or clear stock cached tags or custom `[C]` tags independently.

---

## In-Game Forge Features

### 1. 3D Monitor Mode (`F1`) & Precision Manipulation
- **Instant Spartan ↔ Monitor Toggle (`F1` / D-Pad Up):** Seamless transition between playing as a Spartan and flying as 343 Guilty Spark Monitor with collision-free noclip movement (`F4`) and adjustable fly speed (`Alt`).
- **Raycast Grabbing & Push/Pull Distance:** Grab, move, and place objects within **20m**; use the **Mouse Wheel** (or Gamepad triggers) to push/pull held objects smoothly between **1m and 30m**.
- **3-Axis Gimbal Rotation & Snapping:** Hold **`R`** + Mouse (or `RT` on controller) and press **`Q` / `E`** to cycle **Yaw**, **Pitch**, and **Roll** axes with live 3D protractors. Press **`G`** to cycle rotation snap increments (`Off -> 5° -> 15° -> 45° -> 90°`), hold **`Z`** to snap to the floor, or hold **`F`** to align to any surface normal.

### 2. Tabbed Object Palette (`F2`) & Live 3D Previews
- Spawn **Vehicles**, **Weapons**, **Equipment**, **Scenery**, **Bipeds / AI**, **Devices / Doors / Machines**, **Dynamic Lights & Emitters**, **Gametype Objectives & Spawns**, **Weld Prefabs (`[W]`)**, and **Custom (`[C]`) Tags** with an interactive 3D preview window.
- **Playable Multiplayer Covenant Wraith:** Includes an indestructible, multiplayer-ready Covenant Wraith in the Vehicles menu with a functional parabolic plasma mortar and wing-mounted plasma cannons.

### 3. Weld Prefabs (`[W]`) & Multi-Part Group Welding (`V`)
- **Multi-Part Welding (`V`):** Link up to **64 objects** into a rigid weld group (`Group 1..16`). Grabbing, rotating, duplicating (`C`), or deleting any member manipulates the entire welded assembly while preserving exact relative local-space transforms.
- **Save as Reusable `[W]` Prefab:** Open **Object Properties (`X`) -> Weld & Group Options -> Save Weld as Prefab** to enter a custom name and save the assembly to `tag_profiles/weld_prefabs/<name>.fbrweld`. Saved prefabs appear under the **Prefabs** category of the `F2` Object Palette on every map and automatically bundle any custom `[C]` tag dependencies.

### 4. Uniform & Non-Uniform (`X / Y / Z`) Object Scaling
- Scale any placed Scenery, Vehicle, Weapon, Equipment, or Device either **Uniformly** or independently along **Scale X**, **Scale Y**, and **Scale Z** (`0.10x` to `10.0x`) via **Object Properties (`X`)**.
- Both the hardware vertex pipeline and 3D raycast/swept-sphere collision queries scale dynamically to match the object's visual bounds.

### 5. Dynamic Runtime Point Lights & Surface Emissive Overrides
- Spawn **`[L] Dynamic Light`** markers (`F2 -> Lighting`) and customize **Color Preset** (12 RGB presets), **Light Radius** (`1m–50m`), **Intensity** (`0.10–10.0`), and **Animation Mode** (`Steady`, `Slow Pulse`, `Fast Strobe`, `Plasma Flicker`, `Hazard Beacon`).
- Override **Surface Emissive Color & Power** on any placed 3D object so dark metallic or concrete scenery can glow without modifying shader tags on disk.

### 6. Biped AI Spawning & Automated BSP Nav-Graph Pathfinding
- Spawn Campaign AI bipeds (`Marines`, `Crewmen`, `Elites`, `Grunts`, `Jackals`, `Hunters`, `Flood Combat/Carrier/Infection`, `Sentinels`, `Monitor`) on any multiplayer map.
- Configure **AI Team / Allegiance**, **Primary Weapon Loadout**, **Respawn Timer**, **Move Speed**, **Sight Range**, and **Patrol Radius** via **`X` (Object Properties)**.
- Forge automatically builds an in-memory **BSP Walkable Surface Navigation Graph** (up to 4,096 walkable floor triangles and 24,576 adjacency edges) so AI navigate around walls, up ramps, and across bridges to pursue targets.

### 7. In-Game Custom Script Editor (`F6`) & Live HS Trigger Runtime
- Press **`F6`** in Monitor Mode to open the full-screen **Forge Custom Script Editor** with HaloScript syntax highlighting, line numbers, clipboard copy/paste (`Ctrl+C` / `Ctrl+V`), template insertion (`F2`), and one-key compilation (`F5`).
- Supports `(script startup ...)`, `(script continuous ...)`, `(script dormant ...)`, `(wake ...)`, `(sleep ...)`, `(if ...)`, `(begin ...)`, `(sv_say ...)`, `(object_create_anew ...)`, `(object_destroy ...)`, `(object_teleport ...)`, `(unit_kill ...)`, and `(device_set_power ...)` with full multiplayer synchronization and `.map` persistence.

### 8. Gametype Objectives, Teleporters & 3D Boundary Markers
- Existing and newly placed **Player Spawns**, **CTF Flags**, **Oddball Spawns**, **King of the Hill Zones** (with live 3D boundary beams), **Race Track Points**, and **Teleporters** (`Two-Way`, `Sender Only`, `Receiver Only` on `Channel 1–26`, with optional vehicle pass-through) render as translucent 3D markers in Monitor Mode and can be moved, rotated, or configured in real time.

### 9. In-Session Peer-to-Peer Custom Tag Streaming
- When a host spawns a custom `[C]` tag or `[W]` weld prefab that a connected client does not have installed, Forge automatically streams the compressed tag bundle over the reliable distributed channel (`0xF0..0xF3`) with a live HUD progress bar (`STREAMING CUSTOM TAG: XX%`).
- Streamed tags install into a sandboxed **session-ephemeral RAM slot** (`128 MB` session cap, zero disk writes during gameplay) and are only committed to the client's local Faber Tag Vault if the client explicitly saves the Forge map (`F5` / `Pause -> Save Map`).

---

## Installation

| Platform | Release Download | Debug Download |
| :--- | :--- | :--- |
| **Windows (x86)** | [**OpenCE-Forge-v2.10.0-windows-x86.zip**](https://github.com/SutlabSol/HaloOpenCE-Forge/releases/latest/download/OpenCE-Forge-v2.10.0-windows-x86.zip) / [halo-windows-release.zip](https://github.com/SutlabSol/HaloOpenCE-Forge/releases/latest/download/halo-windows-release.zip) | [halo-windows-debug.zip](https://github.com/SutlabSol/HaloOpenCE-Forge/releases/latest/download/halo-windows-debug.zip) |
| **Linux (x86)** | [halo-linux-release.zip](https://github.com/SutlabSol/HaloOpenCE-Forge/releases/latest/download/halo-linux-release.zip) | [halo-linux-debug.zip](https://github.com/SutlabSol/HaloOpenCE-Forge/releases/latest/download/halo-linux-debug.zip) |
| **Android (ARM64)** | [halo-android-release.zip](https://github.com/SutlabSol/HaloOpenCE-Forge/releases/latest/download/halo-android-release.zip) | [halo-android-debug.zip](https://github.com/SutlabSol/HaloOpenCE-Forge/releases/latest/download/halo-android-debug.zip) |

### Quick Start (Windows / Linux)
1. Download the latest release archive from the [**Releases**](https://github.com/SutlabSol/HaloOpenCE-Forge/releases/latest) page and extract it to a folder.
2. Place your Halo Xbox `maps/` folder (or Halo Xbox `.iso` / `.xiso` disc image) next to `halo.exe` (`halo` on Linux), along with your optional `custom_maps/` (`bitmaps.map`, `sounds.map`, `loc.map`, and Custom Edition `.map` files) and `mcc_maps/` folders.
3. Launch **`halo.exe`**.
4. Select **`MULTIPLAYER -> FORGE`** to start forging or manage custom tags under **`TAG OPTIONS`**.

---

## Controls in Forge

All Forge keyboard/mouse and controller bindings can be customized in-game under **`SETTINGS -> CONTROLS SETUP -> FORGE / FORGE PAD`**.

| Key / Input | Controller Default | Action |
| :--- | :--- | :--- |
| **`F1`** | **D-Pad Up** | Switch Play (Spartan) / Edit (3D Monitor) Mode |
| **`F2`** | **X** | Open / Close Inventory (Forge Object Palette) |
| **`X`** | **X** *(while holding/hovering item)* | Open Object Properties (Team, Scale, Physics, Weld, AI, Lighting) |
| **`Left Click`** | **A** | Grab / Drop Object |
| **`Mouse Wheel`** | **LT / RT** *(while grabbing)* | Push / Pull Held Object (`1m` – `30m`) |
| **`Delete` / `Backspace`** | **Y** | Delete Hovered / Held Object |
| **`Hold R` + Mouse** | **Hold Right Trigger (RT)** | Rotate Selected Object (`Q` / `E` cycles Yaw, Pitch, Roll on KB/M) |
| **`V`** | **Object Properties -> Weld** | Toggle Weld Membership in Active Weld Group (`1..16`) |
| **`C`** | **D-Pad Right** | Duplicate Hovered / Held Object (or entire Weld Group) |
| **`G`** | **Left Thumb (LS)** | Cycle Rotation Snap (`Off`, `5°`, `15°`, `45°`, `90°`) |
| **`Hold Z`** | **D-Pad Left** | Live-snap held object to floor |
| **`Hold F`** | **Right Thumb (RS)** | Align held object to surface normal |
| **`F3`** | **Back** | Toggle 1st-Person / 3rd-Person View |
| **`F4`** | **—** | Toggle Monitor Noclip |
| **`F5`** | **D-Pad Down** | Quick Save Forge Map (bakes standalone `.map` in `custom_maps/`) |
| **`F6`** | **—** | Open In-Game Custom HaloScript (`HS`) Editor |

---

## Building from Source

```powershell
python configure.py --release
ninja windows      # build/windows/halo.exe and SDL3.dll
ninja linux        # build/linux/halo
ninja android_apk  # port/android/app/build/outputs/apk/debug/app-debug.apk
```

If you modify menu definitions in `tools/ce_menus.py` or `tools/port_settings.py`, regenerate the embedded menu XML before building:
```powershell
python tools/port_settings.py
```

---

## Licence & Credits

- **OpenCE : Forge** is developed by **Mota-Lev** on top of [OpenCommunityEdition/OpenCE](https://github.com/OpenCommunityEdition/OpenCE) ([bnunu/halo-1](https://github.com/bnunu/halo-1) / [punpckhdq/halo](https://github.com/punpckhdq/halo)).
- High-resolution HUD and menu art redraws are credited in [AUTHORS.md](AUTHORS.md).
- Licensed under the [GNU Affero General Public License v3.0 or later (`AGPL-3.0-or-later`)](LICENSE).

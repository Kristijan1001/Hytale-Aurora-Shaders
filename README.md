
<a href="https://buymeacoffee.com/kristijan1001" target="_blank"> <img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" width="160" alt="Buy Me a Coffee"> </a>

https://youtu.be/DMA2joELGH0

https://github.com/user-attachments/assets/9c0842a6-0442-40e9-8760-67f695989e0c

https://github.com/user-attachments/assets/55316aae-4729-4537-8840-b3e5d5a30a6e

<img width="3840" height="2160" alt="HytaleClient_mA5cWvFjpR" src="https://github.com/user-attachments/assets/a9f44805-9e51-40f8-b493-9174b37dedc8" />

<img width="3840" height="2160" alt="HytaleClient_jA8qtVqNoz" src="https://github.com/user-attachments/assets/3a8d26e4-41b2-4e28-87d4-f94f4158ffc4" />

<img width="2956" height="1663" alt="HytaleClient_xQDaUhgztt" src="https://github.com/user-attachments/assets/098cc4b4-380c-488e-90d5-f2a9a69fa49f" />

<img width="3840" height="2160" alt="HytaleClient_ARPBNL3it1" src="https://github.com/user-attachments/assets/b79537e3-1a60-4631-a084-619f9bd18946" />

<img width="3840" height="2160" alt="HytaleClient_XLE7vjtrBp" src="https://github.com/user-attachments/assets/1cf4d1f9-71e0-4597-a119-d9321ea4f554" />

<img width="3840" height="2160" alt="image" src="https://github.com/user-attachments/assets/bba36eb8-ee54-4599-a0a7-34b65995309c" />

<img width="3840" height="2160" alt="HytaleClient_iXEQJUArUL" src="https://github.com/user-attachments/assets/ae779dc9-944d-4ca4-a88d-f1c164e0393e" />

<img width="3840" height="2160" alt="HytaleClient_jK6PYPZ5mO" src="https://github.com/user-attachments/assets/b16a91c7-6f33-4242-9e1a-ab10f8d40eb3" />

<img width="3840" height="2160" alt="HytaleClient_iPaT8SGzRg" src="https://github.com/user-attachments/assets/b8ce33f3-78bc-4877-977a-638c4319377b" />

<img width="3840" height="2160" alt="HytaleClient_1Ct0iumhDG" src="https://github.com/user-attachments/assets/605dfb67-8cc8-475a-9dc9-0d5d6c546140" />

<img width="3840" height="2160" alt="HytaleClient_yiDS4pwOJz" src="https://github.com/user-attachments/assets/3d899ec1-089d-4af7-8fac-14bec9981215" />

<img width="3840" height="2160" alt="HytaleClient_9uBVvnw0u2" src="https://github.com/user-attachments/assets/fa8cc289-cdb0-4e77-b53e-8d5332a578e1" />

<img width="3840" height="2160" alt="image" src="https://github.com/user-attachments/assets/e35d3088-7386-4fbb-8ac3-0dd1624d1a57" />

<img width="3840" height="2160" alt="HytaleClient_nKmVvN2vCb" src="https://github.com/user-attachments/assets/287daae9-44d0-4441-810f-5fac7b174c9a" />

<img width="3840" height="2160" alt="image" src="https://github.com/user-attachments/assets/6c50f4f3-098d-4eac-86d0-67ac4d0e60f9" />

<img width="3840" height="2160" alt="HytaleClient_pmXZrodYOq" src="https://github.com/user-attachments/assets/d1f9550a-67e7-4a15-9620-515b5e6832af" />

# Aurora Shaders for Hytale

[Download 1.5.2](../../releases/tag/v1.5.2) · [Changelog](CHANGELOG.md)

A shader pack for Hytale: soft sun shadows, volumetric clouds, light shafts through trees and
caves, reflective water, ray-traced bounce light and ambient occlusion, rain puddles, rainbows,
a rotating star sky, lens flare and a filmic tonemap - with an in-game options menu.

> **Not tested online, use at your own discretion.**

## Install

### Ubuntu-based Linux

The native Linux build targets **x86_64 Ubuntu 22.04 or newer**, including Ubuntu-based
distributions such as Linux Mint and Pop!_OS. It uses the Linux Hytale client and its SDL3
OpenGL renderer. Both native launcher installations and the official Flatpak are detected.

Close Hytale, then install the build dependencies and automatic hook from this folder:

```bash
sudo apt update
sudo apt install build-essential pkg-config libcairo2-dev fonts-dejavu-core python3 python3-pil
./install.sh
```

**Start Hytale through your normal launcher or desktop shortcut.** Aurora loads automatically;
`launch.sh` is now just an optional shortcut to the normal launcher.

The installer builds `build/libaurora-sdl.so` and installs it as the game's `Client/libSDL3.so`.
The original SDL library is preserved unchanged as `Client/libSDL3.aurora-original.so`.
The proxy forwards SDL calls and intercepts shader loading and presentation. Hytale's client
executable is unchanged, and no Flatpak overrides or environment variables are required.

Shaders and settings are copied into `Client/.aurora/shaderpack`, inside the game's own folder,
so the normal Flatpak launcher can read them. Changes made through the menu are saved there.
Reinstalling preserves the installed settings while refreshing the pack. Keep this source
folder for reinstalling after game updates; it is no longer needed to launch the game.

- **After a game update:** close Hytale and run `./install.sh` again. If an update replaces
  the whole Client folder, restore any custom settings from your own backup.
- **Update Aurora:** download the current source or run `git pull` in your checkout, close
  Hytale, then run `./install.sh --build` to rebuild and install the updated Linux hook.
- **Custom game location:** `./install.sh --client-dir "/path/to/Client"`.
- **Custom native launcher:** add `--launcher "/path/to/hytale-launcher"`.
- **Both versions installed:** select `--backend flatpak` or `--backend native`.
- **Rebuild:** `./install.sh --build`.
- **Uninstall:** `./install.sh --uninstall` restores the original SDL library. Installed
  settings are kept in `Client/.aurora/shaderpack`. If the game already replaced SDL during
  an update, uninstall keeps that newer library.

**DLSS on Linux:** on NVIDIA RTX systems `./install.sh` checks for native DLSS support, then
prepares and builds it in: Super Resolution, DLAA and, where supported, Ray Reconstruction. If
the check fails it installs normally and the pack's temporal anti-aliasing is used.
`AU_DLSS=1 ./install.sh` requires DLSS instead. Frame Generation is Windows-only. Linux DLSS
has passed its build and bridge checks but has not yet been verified in gameplay; see
[Linux validation](docs/dlss-diagnostics.md#linux-validation).

### macOS (Apple Silicon)

The macOS build targets **Apple Silicon Macs** with the native client from the official
launcher. It is the Linux SDL3 proxy, built for arm64. Install Apple's command line tools and
two Homebrew packages, close Hytale, then run the installer from this folder:

```bash
xcode-select --install
brew install cairo pkg-config
./install.sh
```

**Start Hytale through the launcher as usual.** The first run creates `build/venv` and installs
Pillow into it; shader preparation uses it to read the game's block textures.

The installer builds `build/libaurora-sdl.dylib` and installs it as `libSDL3.dylib` in
`Hytale.app/Contents/MacOS`. The original library stays there unchanged as
`libSDL3.aurora-original.dylib`, and the proxy re-exports it. Shaders and settings go to
`Client/.aurora/shaderpack`, next to `Hytale.app` rather than inside it.

Hytale's executable is signed with the hardened runtime, which only loads libraries signed by
the same developer. The installer therefore re-signs `Hytale.app` ad hoc without the hardened
runtime, keeping its identifier and entitlements. Hypixel's signed executable and seal are saved
first, and uninstalling puts them back, so the game ends up byte-for-byte as it was.

OpenGL on macOS stops at version 4.1, which has no image load/store. The features built on it
stay off: **Colored area lights**, **Coloured light fog**, **Path tracing**, the voxel
**Scattering method**, GI denoising and DLSS. The hook disables them when it compiles the
shaders, so those menu options do nothing on a Mac. Mac keyboards have no Home or Pause key:
**fn+Left** opens the menu, and **General → Shaders** switches the pack off for comparison.

- **After a game update:** close Hytale and run `./install.sh` again.
- **Custom game location:** `./install.sh --client-dir "/path/to/Client"`, the folder that
  holds `Hytale.app`.
- **Uninstall:** `./install.sh --uninstall` restores the original library and Hypixel's
  signature. Installed settings are kept in `Client/.aurora/shaderpack`.

Details, including what was tested: [macOS notes](docs/macos.md).

### Windows

1. Download the latest release zip from the [Releases](../../releases) page and extract it
   somewhere permanent (the game loads the pack from this folder, so don't delete it).
2. Close Hytale and double-click **`Install.bat`**.
3. Start Hytale.

The installer offers the detected game, an executable picker that supports any drive,
or a pasted installation/executable path. It reads the game's shaders from your own `HytaleClient.exe`, and puts a small
`opengl32.dll` next to the game. Nothing in the game itself is modified.

Install and Update both remove the previous Aurora files and links before installing the new build.
Changed DLL hashes or stale receipts do not block cleanup. Recovery files are stored under the
Aurora package's `backups/` folder, outside the game directory. If the destination
drive cannot create a junction, the installer copies the pack instead. Copied installs preserve
existing setting values on upgrade; edit that installed copy for hot reload. Installation failures
restore the previous client files, and the installed DLL is checksum-verified.

- **After a Hytale update**, run `Update.bat`, or choose **Update shaders for a new game version** in the installer. It clean-installs and regenerates shaders from the new client.
- **To remove it**, double-click `Uninstall.bat`: Aurora's DLL, shaderpack link/copy, receipt, and old Aurora install debris are removed from the game folder. Junction targets are preserved; recovery files stay outside the game.

Requires Windows 10 or 11 and a GPU that runs Hytale. The pack is heavier than vanilla,
especially the RTX preset; see [Performance](#performance).

## In game

Press **Home** to open the compact left-side menu. Mouse controls depend on your platform:

Click **General → Menu hotkey** (or select it and press Right), release held keys, then press
your preferred key. **Escape** cancels. Arrow keys, PgUp/PgDn and Pause remain reserved.
The binding is saved across restarts and is not changed by profiles. **Reset selected** restores Home.

| Platform | Mouse editing |
|---|---|
| **Windows** | Hold **Alt** to click tabs and switches, drag sliders, and scroll. |
| **Linux, macOS** | Hold either **Ctrl** key for the same controls. **Alt** stays available to your window manager. |

While editing, mouse input goes to the menu instead of moving the camera. Release the modifier
to resume normal control. On Linux and macOS, closing the menu or switching away from the game also
restores normal input; pointer positions account for display scaling.

These keyboard shortcuts do not require Alt or Ctrl:

| Key | |
|---|---|
| **Menu hotkey** (Home by default) | Opens / closes the options menu. |
| **Up / Down** | Select an option. |
| **Left / Right** | Change it; hold to slide numbers. |
| **PgUp / PgDn** | Switch category tabs. |
| **Escape** | Close the menu. |
| **Pause** | Shaders off / on, for quick comparisons. |

Changes apply instantly; default values and slider markers are yellow, including On/Off switches.
Click the search box while holding the mouse modifier to search by setting label or define; Backspace edits
the search text only. Tab, Enter and Space do not navigate or change settings.
Enter only confirms text entry when naming a saved preset.
Changed values are mint, and the selected option's default is always shown in the footer. Settings are saved in
`shaderpack/include/AU_Settings.glsl`, which you can also edit by hand while the game runs (saving
the file reloads the shaders). On Linux and macOS, edit the installed copy at
`Client/.aurora/shaderpack/include/AU_Settings.glsl`.

The compact left-side menu leaves the centre of the game visible and does not capture movement
or camera input unless the mouse modifier is held. Keyboard shortcuts remain available without it;
typing in search captures keys until Enter or an arrow key leaves the search field.
The vertically centred panel has category tabs, global search, per-category scrolling, and help for
the selected setting. **Reset selected** resets only that option, never the entire pack.
The ore sweep uses a continuous world-space band with a feathered highlight instead of
triangle-by-triangle screen-space brightening. Its strength, speed and band width are in
**Ores & materials**, separate from the ore's base glow and surrounding light.
Ore blink strength and speed control a coloured glow pulse; zero strength restores steady glow.
**Ore animation** selects blinking plus the soft glint, the original Hytale glint without
blinking, or entirely steady glow. None of these choices disables the base coloured glow.

### Profiles

Open **Profiles** and click an entry, or highlight it with the arrow keys and press Right.
**Default** restores the saved original defaults, kept separately from the quality presets.
**RTX** and **Non-RTX** each offer **Lowest, Low, Medium, High, Highest and Ultra**.
Higher levels increase shadow, cloud, reflection and lighting quality. The old Preset selector
is replaced by this list; the shipped configuration still uses the RTX feature set.

RTX profiles include bounce GI and material reflections, using NVIDIA Ray Reconstruction
with **77% Quality+ DLSS** where supported. Non-RTX profiles use native resolution, the lighter
lighting path and Original denoising, with DLSS off. Neither family enables Frame
Generation or path tracing. Those remain available as manual settings. Builds without
NVIDIA support use Original as the fallback for RTX profiles.
Coloured area lights are enabled only by **Ultra** in either quality list; you can still toggle them manually.

**Save current settings as...** lets you name and save up to 16 custom presets. On Windows,
hold **Alt** while typing the name; on Linux and macOS, type without holding Ctrl. Press Enter to confirm,
or hold your platform's mouse modifier and click **Save name**. Presets are stored in
`shaderpack/presets/` and retained during updates. Applying a built-in profile resets other
shader-menu settings to their defaults; saving a custom preset keeps your current settings.

To manage a custom preset, apply it, then choose **Export saved profile** or **Delete saved profile**.
Exports are named `.glsl` files in `shaderpack/presets/exports/`; repeated exports create new copies.
Deletion requires a second confirmation within five seconds, keeps your current shader settings and
never deletes Default, quality presets or exported copies. To import a shared export, copy it into an
unused `shaderpack/presets/preset-00.glsl` through `preset-15.glsl` slot, then reopen the menu. Do not
overwrite an existing slot. On Linux, use the installed `Client/.aurora/shaderpack/presets/` folder.

## Menu options

### Surface detail and atmosphere

The TAA reminder is the first entry in **General**.

**Ores & materials → Surface detail & shading** adds adjustable texture definition and roughness,
specular antialiasing, directional ambient depth, matte foliage and outdoor sunlight. It defaults to On in all
profiles. Individual controls are under Lighting, Ores & materials and Shadows. [Details](docs/visual-polish.md).

**Sky & atmosphere → Biome & weather atmosphere** keeps Hytale's fog, sky and cloud colours, including special worlds,
without replacing Aurora's sunlight, ambient lighting or colour grading. Switch it off to restore the previous
atmosphere. It defaults to Off in all profiles and reloads immediately. [Details and research](docs/biome-atmosphere.md).

| Option | What it does |
|---|---|
| **Shaders** | Turns the whole pack on or off (same as the Pause key). |
| **Brightness** | Overall brightness of shaded areas and nights, like Minecraft's brightness slider: low is moody, high is bright. |
| **Cave lighting %** | The minimum light in places the sun and sky can't reach - caves, deep interiors. Lower makes caves darker. |

### Lighting

**Outdoor sunlight** balances direct sun against directional sky fill, protects bright diffuse textures
and refines backlit foliage. It defaults to On from Medium through Ultra in both profile families,
and Off in Lowest and Low. Each effect has its own slider. Sunlight bounce reuses the existing GI rays
in the RTX family and path tracing; it does not enable GI in Non-RTX profiles. **Shadows → Sun-sized soft
shadows** controls distance-dependent penumbrae and contact strength. These controls require Surface
detail & shading, add no passes or history buffers, and leave biome atmosphere independent. [Details](docs/outdoor-sunlight.md).

| Option | What it does |
|---|---|
| **Ambient occlusion** | How dark corners, crevices and the spots under objects get. |
| **Torch light** | Brightness of light from torches, lanterns, lava and other light blocks. |
| **Torch colour (warm to Hytale)** | 0 gives every light a classic warm orange glow; 1 uses Hytale's own colour for each light (blue crystals, green lights...). |
| **Emissive glow** | How strongly glowing blocks themselves shine - lava, flames, crystals. |
| **Block relief** | Gives block textures a slight 3D surface, so stone, bricks and wood catch the light per pixel. |
| **Relief strength** | How deep that relief looks. |
| **Foliage translucency** | Sunlight shining through leaves and grass when the sun is behind them. |
| **Scattering method** | Surface approximation, voxel depth + accumulation, or foliage shadow dithering. [Foliage masks and controls](shaderpack/sss/README.md). |
| **Sun highlights** | Soft sun reflections on surfaces and a sheen on leaves. |
| **Sky lighting** | **Classic** or **Sky-derived**: walls and undersides lit by the sky actually drawn, warmer toward a sunset, with light bounced off the ground. [Details](docs/clouds-and-sky.md#sky-lighting). |
| **Ambient occlusion method** | Hytale's SSAO or **GTAO**: occlusion from the real geometry around each pixel, and sky light from the directions the sky is open. [Details](docs/surfaces-and-water.md#ambient-occlusion). |

New looks are off by default; the quality profiles switch them on by cost. **Ores & materials**
also adds **Rain soaking** (absorbent blocks darken in rain) and **Smooth material relief**
(polished stone, ice and metal reflect cleanly). [Materials](docs/surfaces-and-water.md#block-materials).

### Shadows

| Option | What it does |
|---|---|
| **Shadow softness** | How blurry shadow edges are. |
| **Contact shadows** | Small shadows for grass, plants and props, which Hytale leaves out of its shadow map. |
| **Distant shadows** | Shadows for trees and hills beyond the range of the normal shadows, so the landscape doesn't go flat in the distance. |

### RTX

These only apply with the **RTX** preset.

| Option | What it does |
|---|---|
| **Bounce light** | Sunlight bouncing off surfaces: shade under a tree picks up green from the sunlit grass, a room fills with warm light from a sunlit floor, torch flames light the walls around them. |
| **Bounce rays** | How many rays per pixel the bounce light uses. More is smoother but slower; this is the biggest performance setting in the pack. |
| **Bounce reach** | How far (in blocks) bounced light travels. |
| **Ray-traced AO** | How much those rays also darken enclosed corners and the shade under overhangs. |

### Sky & atmosphere

| Option | What it does |
|---|---|
| **Stars** | **Off**, **Procedural** (sparse generated stars), or **Hytale** (Hytale's own star texture with the milky way, turning across the sky through the night). |
| **Star brightness** | Brightness of the stars. |
| **Star density** | Size of the star field on the sky: lower gives bigger, fewer stars. |
| **Night sky blue** | How deep blue the night sky stays down to the horizon; 1 is a grey-blue. |
| **Rainbows** | A rainbow opposite the sun after rain, in the morning or evening. |
| **Atmospheric fog** | Haze that softens the distance. |
| **Fog density** | How thick that haze is. |
| **Atmosphere model** | **Classic** gradient sky or **Enhanced** scattering, shared with distant haze and cloud ambience. |
| **Aurora** | Complementary's aurora borealis on clear nights, in its Reimagined or Unbound style. [Details](docs/clouds-and-sky.md#aurora). |
| **Ground mist** | Mist banks in valleys and over lakes, thickest at dawn, dusk and after rain, lit through the sun's shadows. Set **Mist altitude** near your valleys. [Details](docs/clouds-and-sky.md#ground-mist). |

### Clouds

| Option | What it does |
|---|---|
| **Volumetric clouds** | Replaces Hytale's flat cloud layers with 3D clouds that the sun lights from inside. |
| **Cloud amount** | How much of the sky is covered. |
| **Cloud speed** | How fast they drift. |
| **Cloud layers** | One or two cloud layers. |
| **Cloud shadows** | Clouds cast moving shadows on the ground. |
| **Cloud lighting** | **Classic** shading or **Detailed**: self-shadowed interiors and sunlit edges. |

### Light shafts

| Option | What it does |
|---|---|
| **Light shafts** | Volumetric rays of sun and moon light in the air. |
| **Strength** | Overall strength of the rays. |
| **Forest / cave beams** | How much the beams falling through gaps - between leaves, through roofs and cave openings - stand out from the general haze. 0 makes them plain haze. |
| **Beam boost** | Extra brightness for those beams. |
| **Moonlight haze** | Strength of the moonlit haze at night. |
| **Coloured light fog** | Torches, lanterns, glowing ores and coloured bulbs light the air around them in their own colour; walls block it. Requires Colored area lights. [Details](docs/clouds-and-sky.md#coloured-light-fog). |

### Water

| Option | What it does |
|---|---|
| **Shader water** | The pack's water: moving waves, reflections of the sky and the world, depth colour, sun glints. Off gives Hytale's own water. |
| **Refraction** | How much the water surface bends the view of the riverbed. |
| **Shore foam** | Foam along shorelines. |
| **Underwater caustics** | Rippling light patterns on surfaces while you are under water. |
| **Rain puddles** | Reflective puddles on open ground while it rains, drying up after. |
| **Puddle strength** | How many puddles form and how reflective they are. |
| **Water colour model** | **Classic** or **Enhanced**: light absorbed per colour, so each lake keeps its own tint, shallow edges stay clear and deep beds dim. **Water clarity** sets how far light travels. [Details](docs/surfaces-and-water.md#water-colour). |

### Post-processing

| Option | What it does |
|---|---|
| **Exposure** | Overall image brightness. |
| **Contrast** | Difference between dark and bright. |
| **Saturation** | Colour intensity. |
| **Vibrance** | Boosts dull colours more than already strong ones. |
| **Bloom** | Glow around bright light: the sun, lava, torches. |
| **Bloom strength** | How strong that glow is. |
| **Lens flare** | Camera lens flares from the sun and moon. |
| **Lens flare strength** | How strong the flares are. |
| **Sun glare** | A soft glow around the visible sun. |
| **Vignette** | Slightly darker screen edges. |
| **Temporal anti-aliasing** | Smooths edges and cleans up the fine grain in shadows, light shafts and bounce light by combining frames. Turning it off makes those grainy. |
| **Sharpening** | Sharpens the image (0 is off). |
| **Highlight colour protection** | Very bright coloured light keeps its hue instead of draining to white. |
| **White balance** | Warmer or cooler, greener or more magenta, at unchanged brightness. |
| **Adaptive exposure** | The image brightens where little sky is in view (caves, deep interiors), up to **Adaptation strength**. |

**Screen-space GI → Refined reflections** (on by default) refines reflections so they no longer
step and fade where screen space runs out. [Details](docs/surfaces-and-water.md#reflections).

## Performance

The biggest costs, roughly in order: the **RTX** preset (try fewer **Bounce rays** first),
**Volumetric clouds**, **Light shafts**, and **Shader water** reflections. Non-RTX profiles
use the lighter lighting path; lower quality levels reduce the other costs. The newer looks are
light: on an RTX 5080 at 1440p, GTAO at Medium adds about 0.3 ms and Ground mist about 0.5 ms
([GTAO](docs/surfaces-and-water.md#ambient-occlusion), [sky effects](docs/clouds-and-sky.md#cost)).

## Build it yourself

On Linux, `./build.sh` builds `build/libaurora-sdl.so` (automatic loading) and
`build/libaurora.so` (optional manual preloading) with the system C compiler. It requires
`build-essential`, `pkg-config` and `libcairo2-dev`; shader preparation additionally uses
`python3` and `python3-pil`. Build on the target distribution rather than copying a shared
library built against a newer glibc onto an older Ubuntu release.

On macOS, `./build.sh` builds `build/libaurora-sdl.dylib` with Apple's compiler. It needs the
Xcode command line tools and Homebrew's `cairo` and `pkg-config`. Cairo draws the menu and is
linked weakly: if it is removed later, the game still starts, only without the menu.

On Windows:

The release includes a prebuilt `opengl32.dll`. If you would rather not run a DLL you did not
build, double-click **`Build From Source.bat`** instead of `Install.bat`: it compiles the DLL
from the C source in `hook/` and installs that. The compiler (Zig) is downloaded once from
ziglang.org and checked against the checksum ziglang.org publishes; no Visual Studio needed.

From PowerShell: `.\build.ps1` builds `build\opengl32.dll`, `.\install.ps1 -Build` builds and
installs. Without `-Build` the installer takes the newer of `bin\opengl32.dll` and
`build\opengl32.dll`.

## How it works

Aurora replaces selected Hytale shaders as the game loads them, using an OpenGL proxy on
Windows or an SDL3 proxy on Linux and macOS. The installer extracts the original shader code
from your game; the executable stays untouched, although on macOS it is re-signed.

The shaders use the game's colour, depth, normals and lighting to render the scene. Changes
made through the menu or shader files reload while you play.

## Troubleshooting

- **Black world with the UI visible, after changing a setting (often switching DLSS on):**
  update to 1.5.0, which fixes it. If DLSS itself delivers a black picture, 1.5.0 switches DLSS
  off for that session and writes the reason to `hook.log`; attach that file to your issue
  ([DLSS diagnostics](docs/dlss-diagnostics.md#one-shot-diagnostic)).
- **Linux looks like vanilla:** close Hytale and run `./install.sh`, then start it normally.
  Check `Client/.aurora/shaderpack/hook.log` for `automatic proxy`. Run `./install.sh` again
  after game updates. `./launch.sh --print-command` shows the optional launcher command.
- **Linux build dependencies missing:** install the Ubuntu packages listed above. The installer
  does not install system packages or download a compiler for you.
- **Linux menu too large or clicks misaligned:** update the source, close Hytale and run
  `./install.sh --build`. Use **Ctrl**, not Alt, for mouse editing.
- **macOS looks like vanilla, or Hytale stops starting, after a game update:** close Hytale
  and run `./install.sh` again. `Client/.aurora/shaderpack/hook.log` should mention
  `automatic proxy`. `./install.sh --uninstall` returns the game to Hypixel's signed original.
- **Flatpak reports a missing library or glibc version:** rebuild with `./install.sh --build` on
  the machine where you play and ensure the Hytale Flatpak runtime is up to date.
- **Looks like vanilla:** press Pause once (the shaders may be toggled off), and check that
  `opengl32.dll` sits next to `HytaleClient.exe`. After a game update, run `Install.bat` again.
- **Something renders wrong after a game update:** a shader that fails to compile falls back to
  the game's own; the compiler log is in `shaderpack/errors/`, and `shaderpack/hook.log` records
  what the hook did. On Linux and macOS these files are under `Client/.aurora/shaderpack`.
  Please open an issue with those files.
- **Antivirus warning:** some antivirus tools flag DLLs that load next to a game. Use
  `Build From Source.bat` to build the DLL from the source yourself.

## Known Issues

- **Disable native TAA in Hytale's Graphics settings to avoid shaking and double temporal filtering.**


## Credits and license

Aurora Shaders is a modified pack based on Complementary Shaders by Complementary Development
(https://www.complementary.dev/). It is distributed under the Complementary License Agreement,
included unchanged in [LICENSE.txt](LICENSE.txt).

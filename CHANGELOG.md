# Changelog

## 1.5.2

- Fixed Coloured light fog flickering in 8x8-pixel squares near lights, introduced in 1.5.1. The fog evaluates every step again; the Area light budget keeps it fast near lava.

## 1.5.1

- Fixed a severe frame-rate drop near lava with Colored area lights (6 FPS on an RTX 5080). Every lava block is a light, and each pixel evaluated all of the hundreds in reach, Coloured light fog once per step. A spot reached by more lights than the new **Area light budget** (default 32) now evaluates a random share each frame, weighted so the brightness stays the same; places with few lights are unchanged. Next to a lava lake the lighting pass went from 155 ms to 15 ms.
- Coloured light fog evaluates half of its steps each frame and fewer lights per step.

## 1.5.0

- Fixed the world turning black, with the UI still visible, after changing a shader setting and then loading a world. Switching DLSS on was the most common trigger.
- Fixed a dark, unlit world on AMD GPUs: the lighting shader failed to compile there.
- DLSS now checks that the world actually reaches the screen. If it arrives black, DLSS is switched off for the session and the reason is written to `hook.log`.
- Added native Linux DLSS Super Resolution, DLAA and capability-detected Ray Reconstruction through the Vulkan/OpenGL bridge, with render scaling and native-size fallback. `./install.sh` checks DLSS support and prepares and builds it on supported systems, with a normal installation as the fallback, and keeps Linux DLSS settings across updates. Frame Generation remains Windows-only. RTX gameplay validation on Linux is still pending.
- Added Sky-derived sky lighting: walls and undersides are lit by the sky actually drawn, with ground bounce.
- Added GTAO ambient occlusion with sky light from open directions (bent normals).
- Added rain soaking for absorbent blocks and graded polished-stone materials; smooth materials keep less texture relief.
- Added Enhanced water colour (per-colour absorption that keeps each water's own tint) and a Water clarity control.
- Added refined screen-space reflections: bisection-refined hits, back-face rejection and confidence fades. On by default.
- Added Enhanced atmosphere scattering and Detailed cloud lighting with self-shadowing.
- Added Complementary's aurora borealis, ground mist anchored to the world and lit through the shadow map, and coloured light fog around torches, lanterns and glowing ores.
- Added highlight colour protection, white balance and adaptive exposure.
- New looks are off by default. The quality profiles turn them on by measured cost, and Default resets them.
- The Windows installer uses the newer of the shipped and a locally built `opengl32.dll`.

## 1.4.3

- Fixed DLSS black screens affecting some RTX GPUs, including Super Resolution and Ray Reconstruction.
- Fixed DLSS frame capture and presentation with flipped texture coordinates or offset viewports.
- Improved shared-image synchronization and resizing; added one-shot DLSS diagnostics.

## 1.4.2

- Added press-to-bind menu hotkeys under General. Bindings save across restarts and stay unchanged by profiles.
- Fixed Linux menu size, clipping and click alignment with display scaling.
- Added Ctrl+mouse editing on Linux, with normal input restored when releasing Ctrl, closing the menu or switching windows.

## 1.4.1

- Fixed native glowing blocks, including Cracked Volcanic Rock, turning black while preserving their coloured light.

## 1.4.0

- Added outdoor sunlight controls: directional sky fill, sun colour balance, highlight protection and improved backlit foliage.
- Added sun-sized soft shadows, angular diameter and contact-shadow strength controls.
- Added texture definition, material roughness detail, specular antialiasing and matte foliage controls.
- Added optional biome/weather fog, sky and cloud colours; Off by default.
- Added custom-profile export and confirmed deletion.
- Fixed moving cloud-shadow bands at dawn and sunset.
- Fixed Sandy Gravel incorrectly glowing and emitting light.
- Enabled Outdoor sunlight from Medium upward in both profile families; moved the TAA warning to the top of General.
- Reduced unnecessary shadow filtering in fully shadowed areas.

## 1.3.0

- Added RTX and Non-RTX quality profiles, from Lowest to Ultra, plus a saved Default profile.
- Coloured area lights now enable only in Ultra quality profiles; manual controls remain available.
- Fixed Non-RTX area-light denoising and reduced Original denoiser grain.
- Improved light-cache turnover and culling so new cave lights load reliably with less overhead.
- Added per-pixel foliage transmission masks and optional shadow dithering.
- Added more quality controls and preserved custom foliage masks during Windows/Linux updates.

## 1.2.0

- RTX is now default; Standard is marked deprecated.
- Added Default, DLSS Preset and Path Tracing profiles, plus named custom presets.
- Restored configurable Original denoising; added DLSS Ray Reconstruction and fixed lighting-history trails.
- Improved DLSS HDR/motion handling, live resolution, Quality+/Quality++ and sharpness; fixed black speckles on bright lights.
- Added texture-coloured emissive lighting, stable bulb/potion colours, shorter light caching and torch-relative ore lighting.
- Added foliage/ice depth-based transmission and reduced lighting overhead.
- Redesigned the menu with tabs, reset controls and a native-TAA warning.
- Improved clean install/update/uninstall and added native x86_64 Linux installation.
- Included experimental Windows Frame Generation; off by default, with in-game validation still pending.

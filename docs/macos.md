# macOS port

## What runs

On macOS the official launcher installs a native arm64 client that renders through SDL3 and
OpenGL 4.1, like the Linux one. Aurora therefore uses the Linux hook: `build.sh`
builds it as `libaurora-sdl.dylib`, an SDL3 proxy that re-exports the game's own library, linked
against a placeholder named `@loader_path/libSDL3.aurora-original.dylib`. The installer puts the
proxy in place of `Hytale.app/Contents/MacOS/libSDL3.dylib` and renames the original next to it.
Hytale looks up SDL functions with `dlsym`, so the hooked ones come from the proxy and the rest
from the original library. The pack lives in `Client/.aurora/shaderpack`, outside the app bundle,
so saving a setting never touches signed files.

The installer is the Linux one (`tools/linux_install.py`, `tools/linux_hook.py`) with macOS
branches: paths inside the bundle, a Mach-O header check, `pgrep` instead of `/proc`, and the
signing steps below. `install.sh` creates `build/venv` with Pillow, because Homebrew's Python
refuses system-wide `pip` installs.

## Signing

Hypixel signs `Hytale.app` with the hardened runtime, whose library validation rejects libraries
from any other team. A test binary with the hardened runtime refuses an ad-hoc signed library
("different Team IDs") and loads it once re-signed without it. The installer therefore runs
`codesign --force --sign - --preserve-metadata=identifier,entitlements` on the bundle. That keeps
the identifier and entitlements (microphone, network) and replaces Hypixel's signature with an
ad-hoc one that has no hardened runtime.

Before the first re-sign, the installer checks Hypixel's signature with
`codesign --verify --strict` and saves the signed executable and `_CodeSignature/CodeResources`
in `Client/.aurora/signature`. Uninstalling puts them back only while the executable is still the
one Aurora signed, so an older executable is never copied over an update. If an update replaced
the executable under the hook, `./install.sh` undoes the old install first and saves the new
signature. `tools/macos_test/test_install.py` covers install, reinstall, uninstall, an update of
the executable alone and an update of the whole bundle. The same round trip on a copy of the real
0.7.3 client left the bundle byte-for-byte identical, with Hypixel's signature valid again.

## What stays off

`GL_ARB_shader_image_load_store` is not among the 43 extensions of macOS's OpenGL 4.1, so
everything the pack writes through images stays off: the voxel grid behind **Colored area
lights**, **Coloured light fog**, **Path tracing** and the voxel **Scattering method**, the
history of the Original GI denoiser, and DLSS. On macOS the hook appends `#define`s after
`AU_Settings.glsl`, as it already does for DLSS, so profiles and menu changes cannot turn them
back on. Voxel scattering falls back to the surface approximation; screen-space GI still runs,
smoothed by TAA alone.

## Apple's OpenGL

Two differences broke shaders that compile and draw on other platforms.

Apple's compiler rejects an `#extension` directive that follows any code. The pack inserts
declarations above the game's own directives (`uHookTextureLodBias` in MapChunkFS and
BlockyModelFS, the tonemap include in ParticleFS), so those three failed to compile and fell back
to the game's shaders. On macOS the hook now moves every `#extension` line to the top of an
override. All of them are `: enable`, so lifting them out of `#if` blocks is safe; an unsupported
one only produces a warning.

The macOS client also binds its own textures to units 16-18 (`game uses texture unit 16` and
`18` in `hook.log`), where the hook kept the block materials, emission map and SSS masks. In the
water shader the hook's material atlas and a texture of the game's both ended up on unit 18, and
the water vanished while the riverbed under it still rendered. Taking the atlas off that unit
brought the water back. OpenGL forbids samplers of different types on one unit, and Apple's
driver skips such draws. On macOS the hook's three units now start at 32, within the 80 combined
units macOS allows; other platforms keep 16-18.

## Shared fix

The SDL path never advanced `g_frame`; only `wglSwapBuffers` did. On Linux as well, temporal
reprojection (`temporal_prepare`) therefore kept the first frame's camera, and `uHookFrame` stayed
at 0. `SDL_GL_SwapWindow` now advances the counter.

## Other differences

- Shader hot reload polls `programs/` and `include/` twice a second instead of using inotify.
- The menu draws with Homebrew's cairo, linked weakly: without cairo the game still starts, and
  only the menu is missing.
- Mac keyboards have no Home or Pause key. **fn+Left** sends Home and opens the menu, and
  **General → Shaders** does what Pause does elsewhere. Mouse editing uses Ctrl, as on Linux.

## Performance

On a MacBook Pro with an M4 Pro (macOS 27.2, Hytale 0.7.3, game window at 4112×2516), the default
profile ran at 37-48 fps in a river scene where vanilla ran at 79 fps.

## Tested

- In game on that machine: every override compiles and links (`hook.log` has no `COMPILE ERROR`,
  `LINK ERROR` or `REJECTED` lines); water, sky, clouds, shadows and the menu render; settings
  reload while playing.
- `tools/macos_test/test_install.py`, and `tools/linux_test` run on macOS with `sys.platform` set
  to `linux`.
- The Linux branches of the hook pass a syntax check against stub Linux headers. They have not
  been built or run on Linux.

## Not tested

- Intel Macs (the installer accepts arm64 only), other macOS versions and other Hytale versions.
- How the launcher's updates and repairs treat the re-signed bundle over time.
- Whether macOS asks for App Management permission when the installer runs from Terminal.

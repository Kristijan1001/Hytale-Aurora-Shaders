# Clouds and sky

## Clouds

- **Cloud layers** enables one or two volumetric layers. Both share one ray budget.
- **Cloud lighting: Classic / Detailed** selects height-based shading or self-shadowed interiors and sunlit edges.
- **Cloud light samples** trades view samples for extra light samples in Detailed mode. Start with 1.
- **Cloud ray step** still controls sampling density. Long rays cover the entire visible cloud interval instead of stopping after a fixed distance.

Clouds accumulate front to back and use stable sample positions. Cloud shadows retain the continuous low-sun projection. No temporal buffers are added. Classic keeps the old colour treatment, but both modes use the corrected layer integration.

## Sky & atmosphere

**Atmosphere model: Classic / Enhanced** selects the original gradient or an analytic scattering approximation. Enhanced shares its sky colour with distant haze, cloud ambience and reflection fallback. It follows the sun, weather and camera height, without changing surface sunlight, surface ambient light, exposure or either denoiser.

**Biome & weather atmosphere** takes priority over this setting and remains a separate option. Fog density and Atmospheric fog still work normally. Classic is the default for the new selectors; existing saved profiles are not rewritten.

These controls apply immediately. They do not add ground mist or underwater shafts.

## Sky lighting

**Lighting → Sky lighting: Classic / Sky-derived** selects where outdoor ambient light comes from. Sky-derived integrates the sky actually drawn (Classic or Enhanced atmosphere, or biome atmosphere) into a six-direction ambient cube, so a wall facing a bright horizon or a sunset receives more and warmer fill, and undersides receive light bounced off the ground. Upward faces keep the existing ambient brightness; the hue is bounded like the classic fill.

It replaces the classic directional sky fill rather than adding to it; **Sky light strength** sets how fully (1 = completely, 0 = the classic look). **Sky light ground bounce** sets how bright the ground is that passes sky light up to walls and undersides. Hytale's sky light, AO and the path tracer's sky visibility still darken rooms and caves, which keep their existing fill, as does underwater lighting. Sunlight, torch light, GI and the denoisers are unchanged. Translucent surfaces lit in the forward pass (water, glass) keep the classic fill.

The cube is computed by the deferred pass's vertex shader (three vertices per draw) from 48 sky directions: no render targets, buffers or extra passes. That shader, `ScreenVS`, is generated from the game's own shader by the installer (`tools/prepare_shaders`), so re-run the installer after updating the pack from source.

## Aurora

**Sky & atmosphere → Aurora** (off by default) is Complementary's aurora borealis, ported as is under the licence this pack ships with. **Aurora style** picks Reimagined's blocky curtains or Unbound's smooth ones (the default). Hytale gives shaders no moon phase or snowy-biome signal, so it shows on every clear night instead of only full moons in snowy places. It is drawn with the sky, so water reflects it, and it doesn't light the world. **Aurora brightness** (1 = Complementary's) and **Aurora drift speed** tune it.

## Ground mist

**Sky & atmosphere → Ground mist** (off by default) adds patches of mist anchored to the world. They settle below **Mist altitude**, in valleys, over lakes and on low ground, and thin out over **Mist depth** above it.
- **Time and weather:** thickest at dawn and dusk and while the ground is still wet after rain. Toward midday it burns off to **Midday mist**.
- **Light:** the march samples the sun's shadow map, so morning light falls through the mist in beams, and the mist glows around a low sun.
- **Around you:** you stand in a clearer pocket and see the banks ahead.
- **Indoors:** in caves and rooms it fades out.
- **Coverage:** it covers terrain, sky and water alike.

**Mist density**, **Mist range** and **Mist quality** (8/12/16 samples per pixel) trade look for cost. Set the altitude near the level of your valleys and lakes: Hytale worlds have no fixed sea level, so it isn't guessed.

## Coloured light fog

**Light shafts → Coloured light fog** (off by default; requires Colored area lights) lets torches, lanterns, glowing ores and coloured bulbs light the air around them in their own colour: a warm glow in a torch-lit cave, coloured haze around a bulb.
- **The march:** a short march along each view ray (**Light fog quality**: 4/6/8 steps, up to **Light fog range** blocks). At each step it sums the lights listed for that spot's 16-block light cell, the same culled list surfaces use. A cell listing more lights than an eighth of the **Area light budget** (by lava, hundreds) contributes a random share of them, weighted so the average is unchanged; the anti-aliasing averages it.
- **Walls:** one of those lights is picked by brightness and checked against the voxel grid, so walls block the glow and a doorway lets a beam through.
- **Removed lights:** they fade with the light cache, leaving no coloured air behind.

The effect is mostly visible in caves, interiors and at night. **Light fog density** sets its strength. Complementary's version reads a flood-filled colour-light volume, which Aurora doesn't have, so this one is built on Aurora's own light list.

A high cirrus layer was tried and removed: in game it read as painted streaks rather than cloud.

## Cost

The bounded cloud march adds no render targets. In a controlled 1920x1080 test, the original one-layer path took about 0.19 ms, corrected two-layer Classic about 0.61 ms, and two-layer Detailed about 0.69 ms. The original path ignored the second-layer setting, so these are not equivalent workloads.

The sky/fog approximation also adds no render targets. Ground mist at 12 samples measured about 0.3 ms in the lighting pass plus 0.18 ms in the sky pass on an RTX 5080 at 2560x1351. Coloured light fog was below the scene's frame-to-frame noise with a few torches. Beside a lava lake, where every lava block is a light, it added about 3.6 ms and area lighting about 4 ms at the default Area light budget of 32, down from about 125 ms and 25 ms when every listed light was evaluated. Measurements are pass-specific, not a promise about whole-game FPS; output resolution, GPU and view affect cost.

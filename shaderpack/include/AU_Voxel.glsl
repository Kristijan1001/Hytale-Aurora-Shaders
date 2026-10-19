// Aurora Shaders for Hytale - voxel world for the path tracer.
//
// A 256x128x256 block grid around the player, kept by the hook (opengl32.dll) in two 3D
// images that persist across frames: auVoxGeo (what is in each block) and auVoxCol (its
// colour). The grid wraps around (block b lives in cell b & mask), so walking never needs a
// copy; every cell carries a tag of which wrap it was written for, and a cell whose tag does
// not match the block asked for reads as unknown.
//
// Writers: MapChunkVS (every block the game draws: solid / porous / partial) and DeferredFS
// (colour of what is on screen, air in front of visible block faces - this is what removes
// broken blocks). The program file needs "#extension GL_ARB_shader_image_load_store : enable"
// before anything else.
#ifndef AU_VOXEL_INC
#define AU_VOXEL_INC

#define AU_VOX_SIZE  ivec3(256, 128, 256)
#define AU_VOX_MASK  ivec3(255, 127, 255)
#define AU_VOX_SHIFT ivec3(8, 7, 8)
#define AU_VOX_HALF  ivec3(128, 64, 128)

// auVoxGeo cell: bits 0-1 type, bit 2 small model (torch, rock: no vertex on a cell corner),
// bit 3 emissive, bits 8-13 wrap tag.
#define AU_VOX_UNKNOWN 0u
#define AU_VOX_AIR     1u
#define AU_VOX_SOLID   2u
#define AU_VOX_POROUS  3u   // leaves: light gets through the gaps
#define AU_VOX_SMALL   4u
#define AU_VOX_EMISSIVE 8u
#define AU_VOX_COLOR_FRAME_MASK 0x70u
#define AU_VOX_ICE 0x80u
#define AU_VOX_GEOMETRY_FRAME_MASK 0xC000u

layout(r32ui) uniform coherent uimage3D auVoxGeo;
layout(rgba8) uniform image3D auVoxCol;

#ifdef AU_LIGHT_LIST_CACHED
// Lighting only reads what earlier draws wrote (the hook puts a memory barrier between them), so
// it reads through the cache: coherent loads bypass it on NVIDIA, and by a lava lake every pixel
// reads the light list dozens of times.
layout(r32ui) uniform uimage2D auLights;
#else
layout(r32ui) uniform coherent uimage2D auLights;
#endif
#define AU_PT_MAX_LIGHTS 1024u
#ifndef AU_LIGHT_CACHE_SECONDS
#define AU_LIGHT_CACHE_SECONDS 1.0
#endif
#define AU_LIGHT_CACHE_MILLISECONDS uint(AU_LIGHT_CACHE_SECONDS * 1000.0)
#define AU_LIGHT_VISIBILITY_MILLISECONDS min(AU_LIGHT_CACHE_MILLISECONDS, 100u)
#ifndef AU_COMMON
uniform float uHookTime;
#endif
#ifdef AU_VOX_WRITER_ONLY
#ifndef AU_COMMON
uniform int uHookFrame;
#endif
#endif

uint AU_VoxTag(ivec3 b)
{
    ivec3 k = (b >> AU_VOX_SHIFT) & 3;
    return uint(k.x | (k.y << 2) | (k.z << 4));
}

// Blocks the grid can hold right now: the window centred on the camera's block.
bool AU_VoxInWindow(ivec3 b, ivec3 camBlock)
{
    ivec3 d = b - camBlock + AU_VOX_HALF;
    return all(greaterThanEqual(d, ivec3(0))) && all(lessThan(d, AU_VOX_SIZE));
}

vec3 AU_EmitterPosition(ivec3 block, uint packedPosition)
{
    uint offset = (packedPosition >> 23) & 31u;
    if (offset == 0u) return vec3(block) + 0.5;
    offset--;
    return vec3(block) + (vec3(offset % 3u, (offset / 3u) % 3u, offset / 9u) + 0.5) / 3.0;
}

uint AU_EmitterKey(ivec3 block, uint hue)
{
    uvec3 coordinates = uvec3(block);
    uint key = coordinates.x * 0x9e3779b9u ^ coordinates.y * 0x85ebca6bu
        ^ coordinates.z * 0xc2b2ae35u ^ hue * 0x27d4eb2du;
    key ^= key >> 16;
    key *= 0x7feb352du;
    key ^= key >> 15;
    return (key & 0x7fffffffu) | 1u;
}

uint AU_LightCacheClock()
{
    return uint(max(uHookTime, 0.0) * 1000.0);
}

void AU_PublishFineEmitter(ivec3 block)
{
    ivec3 cell = block & AU_VOX_MASK;
    uint tag = AU_VoxTag(block) << 8;
    uint previous = imageAtomicAdd(auVoxGeo, cell, 0u);
    for (int attempt = 0; attempt < 8; attempt++) {
        uint replacement = ((previous & 0x3f00u) == tag ? previous & 0xfff7u : 0u)
            | tag | AU_VOX_EMISSIVE | ((AU_LightCacheClock() & 65535u) << 16);
        if (previous == replacement) return;
        uint observed = imageAtomicCompSwap(auVoxGeo, cell, previous, replacement);
        if (observed == previous) return;
        previous = observed;
    }
}

void AU_RecordEmitterCached(vec3 worldPosition, vec3 radiance, bool fineSource, bool observedSource)
{
#if AU_LOCAL_LIGHTS || AU_PT_LIGHTS
    ivec3 block = ivec3(floor(worldPosition));
    if (max(radiance.r, max(radiance.g, radiance.b)) <= 0.001 || !AU_VoxInWindow(block, ivec3(floor(CameraPosition)))) return;
    ivec3 cell = block & AU_VOX_MASK;
    uint tag = AU_VoxTag(block);
    uint packedCell = uint(cell.x | (cell.y << 8) | (cell.z << 15));
    radiance = clamp(radiance, 0.0, 1.0);
    float peak = max(radiance.r, max(radiance.g, radiance.b));
    uvec3 channels = uvec3(step(vec3(peak * 0.55), radiance));
    uint hue = fineSource ? channels.r | (channels.g << 1) | (channels.b << 2) : 0u;
    uint key = AU_EmitterKey(block, hue);
    uint clock = AU_LightCacheClock();
    uint stamp = clock + 1u;
    float quality = peak - 0.5 * min(radiance.r, min(radiance.g, radiance.b));
    ivec3 subcell = clamp(ivec3(fract(worldPosition) * 3.0), ivec3(0), ivec3(2));
    uint offset = fineSource ? uint(1 + subcell.x + 3 * subcell.y + 9 * subcell.z) : 0u;
    uint selectedIndex = AU_PT_MAX_LIGHTS;
    uint selectedKey = 0u;
    bool matching = false;
    bool reusable = false;
    float selectedPriority = -1.0;
    vec3 cameraDelta = vec3(block) + 0.5 - CameraPosition;
    float distanceSquared = dot(cameraDelta, cameraDelta);
    ivec3 base = ivec3(floor(CameraPosition)) - AU_VOX_HALF;
    uint stride = (key >> 11) | 1u;
    for (uint probe = 0u; probe < 16u; probe++) {
        uint index = ((key >> 1) + probe * stride) & (AU_PT_MAX_LIGHTS - 1u);
        uint previous = imageAtomicAdd(auLights, ivec2(int(index), 3), 0u);
        if ((previous & 0x80000000u) != 0u) {
            if ((previous & 0x7fffffffu) == key) return;
            continue;
        }
        if (previous != key && reusable) continue;
        uint oldStamp = imageLoad(auLights, ivec2(int(index), 2)).r;
        uint oldPosition = imageLoad(auLights, ivec2(1 + 2 * int(index), 0)).r;
        uint metadata = imageLoad(auLights, ivec2(int(index), 4)).r;
        bool sameSource = previous == key && (oldPosition & 0x7fffffu) == packedCell && (metadata & 63u) == tag;
        uint age = clock - (oldStamp - 1u);
        ivec3 oldBlock = base + ((ivec3(oldPosition & 255u, (oldPosition >> 8) & 127u, (oldPosition >> 15) & 255u) - base) & AU_VOX_MASK);
        bool occupied = previous != 0u && oldStamp != 0u && age < AU_LIGHT_CACHE_MILLISECONDS && (metadata & 63u) == AU_VoxTag(oldBlock);
        float priority = 1e20;
        if (!sameSource && occupied) {
            bool visible = (metadata & 64u) != 0u && ((clock - (metadata >> 16)) & 65535u) < AU_LIGHT_VISIBILITY_MILLISECONDS;
            vec3 oldDelta = vec3(oldBlock) + 0.5 - CameraPosition;
            float oldDistanceSquared = dot(oldDelta, oldDelta);
            if ((!observedSource && visible) || (observedSource == visible && oldDistanceSquared <= distanceSquared * 1.25 + 1.0)) continue;
            priority = oldDistanceSquared + (visible ? 0.0 : 1e8);
        }
        if (!sameSource && priority <= selectedPriority) continue;
        selectedIndex = index;
        selectedKey = previous;
        matching = sameSource;
        reusable = !occupied;
        selectedPriority = priority;
        if (matching) break;
    }
    if (selectedIndex >= AU_PT_MAX_LIGHTS) return;
    int slot = int(selectedIndex);
    uint oldStamp = imageLoad(auLights, ivec2(slot, 2)).r;
    uint oldColor = imageLoad(auLights, ivec2(2 + 2 * slot, 0)).r;
    vec3 oldRadiance = unpackUnorm4x8(oldColor).rgb;
    float oldQuality = max(oldRadiance.r, max(oldRadiance.g, oldRadiance.b))
        - 0.5 * min(oldRadiance.r, min(oldRadiance.g, oldRadiance.b));
    bool expired = oldStamp == 0u || clock - (oldStamp - 1u) >= AU_LIGHT_CACHE_MILLISECONDS;
    bool improve = !matching || expired || (fineSource ? quality > oldQuality + 0.01 : oldStamp != stamp);
    uint metadata = imageLoad(auLights, ivec2(slot, 4)).r;
    bool visibilityChanged = observedSource && ((metadata & 64u) == 0u || (metadata >> 16) != (clock & 65535u));
    if (matching && !improve && !visibilityChanged && oldStamp == stamp) return;
    uint observed = imageAtomicCompSwap(auLights, ivec2(slot, 3), selectedKey, key | 0x80000000u);
    if (observed != selectedKey) return;
    if (!matching) {
        uint latestMetadata = imageLoad(auLights, ivec2(slot, 4)).r;
        uint latestPosition = imageLoad(auLights, ivec2(1 + 2 * slot, 0)).r;
        uint latestStamp = imageLoad(auLights, ivec2(slot, 2)).r;
        ivec3 latestBlock = base + ((ivec3(latestPosition & 255u, (latestPosition >> 8) & 127u, (latestPosition >> 15) & 255u) - base) & AU_VOX_MASK);
        bool occupied = selectedKey != 0u && latestStamp != 0u && clock - (latestStamp - 1u) < AU_LIGHT_CACHE_MILLISECONDS
            && (latestMetadata & 63u) == AU_VoxTag(latestBlock);
        bool visible = (latestMetadata & 64u) != 0u && ((clock - (latestMetadata >> 16)) & 65535u) < AU_LIGHT_VISIBILITY_MILLISECONDS;
        vec3 latestDelta = vec3(latestBlock) + 0.5 - CameraPosition;
        if (occupied && ((!observedSource && visible) || (observedSource == visible && dot(latestDelta, latestDelta) <= distanceSquared * 1.25 + 1.0))) {
            imageAtomicExchange(auLights, ivec2(slot, 3), selectedKey);
            return;
        }
    }
    if (improve) {
        imageStore(auLights, ivec2(1 + 2 * slot, 0), uvec4(packedCell | (offset << 23)));
        imageStore(auLights, ivec2(2 + 2 * slot, 0), uvec4(packUnorm4x8(vec4(radiance, 0.0)) & 0xffffffu));
    }
    metadata = matching ? imageLoad(auLights, ivec2(slot, 4)).r : tag | (hue << 8);
    if (observedSource) metadata = (metadata & 65535u) | 64u | ((clock & 65535u) << 16);
    imageStore(auLights, ivec2(slot, 4), uvec4(metadata));
    imageStore(auLights, ivec2(slot, 2), uvec4(stamp));
    memoryBarrier();
    imageAtomicExchange(auLights, ivec2(slot, 3), key);
    imageAtomicMax(auLights, ivec2(0, 0), selectedIndex + 1u);
    if (fineSource) AU_PublishFineEmitter(block);
#endif
}

void AU_RecordEmitterAt(vec3 worldPosition, vec3 radiance, bool fineSource)
{
    AU_RecordEmitterCached(worldPosition, radiance, fineSource, true);
}

void AU_RecordEmitter(ivec3 block, vec3 radiance)
{
    AU_RecordEmitterCached(vec3(block) + 0.5, radiance, false, false);
}

// Raw cell of block b, or 0 (unknown) when the cell belongs to another wrap.
uint AU_VoxGet(ivec3 b)
{
    uint v = imageLoad(auVoxGeo, b & AU_VOX_MASK).r;
    return ((v >> 8) & 63u) == AU_VoxTag(b) ? v : 0u;
}

// Albedo of block b; a = 0 when it was never seen on screen.
vec4 AU_VoxGetColor(ivec3 b)
{
    vec4 c = imageLoad(auVoxCol, b & AU_VOX_MASK);
    uint tag = uint(c.a * 255.0 + 0.5);
    return (tag & 64u) != 0u && (tag & 63u) == AU_VoxTag(b) ? vec4(c.rgb, 1.0) : vec4(0.0);
}

#include "AU_Transmission.glsl"

#ifndef AU_VOX_WRITER_ONLY

float AU_VoxHash(ivec3 b, float salt)
{
    return fract(sin(dot(vec3(b), vec3(12.9898, 78.233, 37.719)) + salt) * 43758.5453);
}

// Amanatides-Woo walk through the grid from `origin` (world space) along `dir`. Solid blocks
// stop the ray; leaves and partial blocks stop it only part of the time (`rnd` 0-1 decides,
// per block), which averages to light getting through. The start block is never tested.
// Unknown blocks count as air. Returns false when the ray leaves the grid or `maxDist`.
bool AU_VoxTrace(vec3 origin, vec3 dir, float maxDist, int maxSteps, float rnd,
                 out ivec3 hitBlock, out vec3 hitNormal, out float hitT, out uint hitCell)
{
    ivec3 camBlock = ivec3(floor(CameraPosition));
    ivec3 b = ivec3(floor(origin));
    dir = mix(dir, vec3(1e-6), lessThan(abs(dir), vec3(1e-6)));
    ivec3 stepDir = ivec3(sign(dir));
    vec3 tDelta = abs(1.0 / dir);
    vec3 tMax = (vec3(b) + max(vec3(stepDir), 0.0) - origin) / dir;
    hitBlock = b;
    hitNormal = vec3(0.0);
    hitT = 0.0;
    hitCell = 0u;
    for (int i = 0; i < maxSteps; i++) {
        // One axis per step, also on exact ties: stepping two at once slips diagonally
        // between blocks that only touch at an edge.
        vec3 mf = tMax.x < tMax.y ? (tMax.x < tMax.z ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 0.0, 1.0))
                                  : (tMax.y < tMax.z ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0));
        float t = dot(tMax, mf);
        if (t > maxDist) return false;
        b += ivec3(mf) * stepDir;
        tMax += mf * tDelta;
        if (!AU_VoxInWindow(b, camBlock)) return false;
        uint v = AU_VoxGet(b);
        uint type = v & 3u;
        if (type < AU_VOX_SOLID) continue;
        float pass = type == AU_VOX_POROUS ? 0.45 : ((v & AU_VOX_SMALL) != 0u ? 0.5 : 0.0);
        if (pass > 0.0 && fract(rnd + AU_VoxHash(b, 0.0)) < pass) continue;
        hitBlock = b;
        hitNormal = -mf * vec3(stepDir);
        hitT = t;
        hitCell = v;
        return true;
    }
    return false;
}

// DeferredFS: teaches the grid what is on screen. A tenth of the pixels per frame (`rnd`)
// blend their albedo slowly into the block behind them (a steady average of its texture), and clear a full block standing right in
// front of a visible block face - a face can only be seen when nothing solid is there, so
// that block was broken.
void AU_VoxFeed(vec3 playerPos, vec3 normal, vec3 albedo, bool foliage, bool emissive, float rnd)
{
    if (rnd > 0.1 || dot(playerPos, playerPos) > 128.0 * 128.0) return;
    ivec3 camBlock = ivec3(floor(CameraPosition));
    vec3 wp = playerPos + CameraPosition;
    // Only pixels well inside a face: at its edges the position from the depth buffer can
    // slip into the neighbouring block, which would then be coloured or cleared by mistake.
    vec3 an = abs(normal);
    vec3 axis = step(vec3(0.99), an);
    vec3 edge = abs(wp - round(wp));
    if (max(an.x, max(an.y, an.z)) > 0.99 && dot(step(edge, vec3(0.08)), 1.0 - axis) > 0.0) return;

    ivec3 inside = ivec3(floor(wp - normal * 0.08));
    uint insideCell = AU_VoxGet(inside);
    // Plants are not in the grid; their colour goes to the ground they grow on (grass
    // covers most of the ground's own pixels).
    bool plant = (insideCell & 3u) < AU_VOX_SOLID;
    if (plant) inside = ivec3(floor(wp)) - ivec3(0, 1, 0);
    // Flames and glowing pixels are not the block's colour (and animate).
    if (!emissive && AU_VoxInWindow(inside, camBlock) && (AU_VoxGet(inside) & 3u) >= AU_VOX_SOLID) {
        ivec3 c = inside & AU_VOX_MASK;
        uint tag = AU_VoxTag(inside);
        uint stamp = (uint(uHookFrame % 7) + 1u) << 4;
        uint cell = imageLoad(auVoxGeo, c).r;
        for (int attempt = 0; attempt < 8; attempt++) {
            if (((cell >> 8) & 63u) != tag || (cell & 3u) < AU_VOX_SOLID || (cell & AU_VOX_COLOR_FRAME_MASK) == stamp) break;
            uint previous = imageAtomicCompSwap(auVoxGeo, c, cell, (cell & ~AU_VOX_COLOR_FRAME_MASK) | stamp);
            if (previous == cell) {
                vec4 old = imageLoad(auVoxCol, c);
                uint oldTag = uint(old.a * 255.0 + 0.5);
                vec3 col = (oldTag & 64u) != 0u && (oldTag & 63u) == tag ? mix(old.rgb, albedo, 0.05) : albedo;
                imageStore(auVoxCol, c, vec4(col, float(64u | tag) / 255.0));
                break;
            }
            cell = previous;
        }
    }

    // Only from the face of a full block the grid knows: a grass blade standing against a
    // hill step would otherwise erase the step every frame (and the chunk pass re-add it).
    // Near the player only (where blocks get broken): far off, a face seen past a slab or
    // stair would clear it every frame and the chunk pass re-add it, flickering the rays.
    if (!plant && !foliage && (insideCell & 7u) == AU_VOX_SOLID && max(an.x, max(an.y, an.z)) > 0.99
        && dot(playerPos, playerPos) < 32.0 * 32.0) {
        float offBoundary = dot(edge, axis);
        if (offBoundary < 0.04) {
            ivec3 front = ivec3(floor(wp + normal * 0.5));
            uint fv = AU_VoxGet(front);
            uint geometryFrame = (uint(uHookFrame) & 3u) << 14;
            if (AU_VoxInWindow(front, camBlock) && (fv & 3u) == AU_VOX_SOLID && (fv & AU_VOX_SMALL) == 0u
                && (fv & AU_VOX_GEOMETRY_FRAME_MASK) != geometryFrame)
                imageAtomicCompSwap(auVoxGeo, front & AU_VOX_MASK, fv, AU_VOX_AIR | (AU_VoxTag(front) << 8));
        }
    }
}

vec3 auLocalLightUnshadowed = vec3(0.0);

#if AU_LOCAL_LIGHTS
uniform int uHookLocalLightTiles;

float AU_CachedLightWeight(uint index, uint packedPosition)
{
    uint key = imageLoad(auLights, ivec2(int(index), 3)).r;
    if (key == 0u) return 1.0;
    uint stamp = imageLoad(auLights, ivec2(int(index), 2)).r;
    uint clock = AU_LightCacheClock();
    uint age = clock - (stamp - 1u);
    if (stamp == 0u || age >= AU_LIGHT_CACHE_MILLISECONDS) return 0.0;
    ivec3 base = ivec3(floor(CameraPosition)) - AU_VOX_HALF;
    ivec3 block = base + ((ivec3(packedPosition & 255u, (packedPosition >> 8) & 127u, (packedPosition >> 15) & 255u) - base) & AU_VOX_MASK);
    if ((imageLoad(auLights, ivec2(int(index), 4)).r & 63u) != AU_VoxTag(block)) return 0.0;
    if (((packedPosition >> 23) & 31u) == 0u) {
        uint geometry = AU_VoxGet(block);
        if ((geometry & AU_VOX_EMISSIVE) != 0u) {
            uint fineAge = (clock - (geometry >> 16)) & 65535u;
            if (fineAge < AU_LIGHT_CACHE_MILLISECONDS) return 0.0;
            imageAtomicCompSwap(auVoxGeo, block & AU_VOX_MASK, geometry, geometry & ~AU_VOX_EMISSIVE);
        }
    }
    return 1.0 - smoothstep(float(AU_LIGHT_CACHE_MILLISECONDS) * 0.75, float(AU_LIGHT_CACHE_MILLISECONDS), float(age));
}

struct AU_LocalLightSample {
    vec3 energy;
    uvec3 indices;
    vec3 positions[3];
    ivec3 blocks[3];
};

void AU_SelectLocalLight(inout AU_LocalLightSample sampleData, vec3 radiance, vec3 position, ivec3 block, uint index, float selection)
{
    sampleData.energy += radiance;
    for (int channel = 0; channel < 3; channel++) {
        if (radiance[channel] > 0.0 && selection * sampleData.energy[channel] < radiance[channel]) {
            sampleData.indices[channel] = index;
            sampleData.positions[channel] = position;
            sampleData.blocks[channel] = block;
        }
    }
}

vec3 AU_ResolveLocalLight(AU_LocalLightSample sampleData, vec3 origin, float randomValue)
{
    vec3 visibility = vec3(0.0);
    for (int channel = 0; channel < 3; channel++) {
        if (sampleData.energy[channel] <= 0.0) continue;
        bool reused = false;
        for (int previous = 0; previous < channel; previous++) {
            if (sampleData.indices[channel] == sampleData.indices[previous]) {
                visibility[channel] = visibility[previous];
                reused = true;
                break;
            }
        }
        if (reused) continue;
        vec3 delta = sampleData.positions[channel] - origin;
        float distanceToLight = length(delta);
        ivec3 hitBlock;
        vec3 hitNormal;
        float hitDistance;
        uint hitCell;
        bool blocked = AU_VoxTrace(origin, delta / max(distanceToLight, 0.0001), distanceToLight, 64,
            randomValue, hitBlock, hitNormal, hitDistance, hitCell);
        visibility[channel] = blocked && any(notEqual(hitBlock, sampleData.blocks[channel])) ? 0.0 : 1.0;
    }
    return sampleData.energy * visibility * AU_LOCAL_LIGHT_STRENGTH;
}

void AU_BuildLocalLightTileWord(int word, int tile, bool expandedTiles)
{
    ivec3 tileCell = expandedTiles ? ivec3(tile & 15, (tile >> 4) & 7, tile >> 7)
                                 : ivec3(tile & 3, (tile >> 2) & 3, tile >> 4);
    vec3 tileCenter = vec3(tileCell) * 16.0 + 8.0;
    vec3 tilePeriod = expandedTiles ? vec3(AU_VOX_SIZE) : vec3(64.0);
    int row = 0;
    uint lightCount = min(imageLoad(auLights, ivec2(0, row)).r, AU_PT_MAX_LIGHTS);
    uint mask = 0u;
    for (int bit = 0; bit < 32; bit++) {
        uint index = uint(word * 32 + bit);
        if (index >= lightCount) break;
        uint cell = imageLoad(auLights, ivec2(1 + 2 * int(index), row)).r;
        float weight = AU_CachedLightWeight(index, cell);
        if (tile == 0) imageStore(auLights, ivec2(1024 + int(index), 4), uvec4(floatBitsToUint(weight)));
        if (imageLoad(auLights, ivec2(2 + 2 * int(index), row)).r == 0u || weight <= 0.0) continue;
        vec3 center = AU_EmitterPosition(ivec3(cell & 255u, (cell >> 8) & 127u, (cell >> 15) & 255u), cell);
        vec3 distanceToTile = max(abs(mod(center - tileCenter + tilePeriod * 0.5, tilePeriod) - tilePeriod * 0.5) - 8.0, 0.0);
        if (dot(distanceToTile, distanceToTile) <= pow2(AU_LOCAL_LIGHT_RANGE + AU_LOCAL_LIGHT_SOFTNESS))
            mask |= 1u << uint(bit);
    }
    int address = tile * 32 + word;
    ivec2 maskAddress = expandedTiles ? ivec2(address & 2047, 5 + (address >> 11)) : ivec2(1 + address, 1);
    imageStore(auLights, maskAddress, uvec4(mask));
    ivec2 wordsAddress = ivec2(expandedTiles ? tile : 1024 + tile, expandedTiles ? 1 : 2);
    if (mask != 0u) imageAtomicOr(auLights, wordsAddress, 1u << uint(word));
    else imageAtomicAnd(auLights, wordsAddress, ~(1u << uint(word)));
}

void AU_BuildLocalLightTile(int tileRows)
{
    bool expandedTiles = uHookLocalLightTiles >= 2;
    for (int tile = int(gl_FragCoord.y); tile < (expandedTiles ? 2048 : 64); tile += max(tileRows, 1))
        AU_BuildLocalLightTileWord(int(gl_FragCoord.x), tile, expandedTiles);
}

#ifndef AU_LOCAL_LIGHT_BUDGET
#define AU_LOCAL_LIGHT_BUDGET 32
#endif

ivec2 AU_TileMaskAddress(int tile, int word, bool expandedTiles)
{
    int address = tile * 32 + word;
    return expandedTiles ? ivec2(address & 2047, 5 + (address >> 11)) : ivec2(1 + address, 1);
}

// Position of the n-th (from 0) set bit of mask, which has more than n bits set.
int AU_NthSetBit(uint mask, uint n)
{
    int bit = 0;
    for (int width = 16; width >= 1; width >>= 1) {
        uint low = uint(bitCount(mask & ((1u << uint(width)) - 1u)));
        if (n >= low) { n -= low; mask >>= uint(width); bit += width; }
    }
    return bit;
}

// How many lights one 16-block cell lists (one read per non-empty list word; neighbouring pixels
// share the cell, so the reads mostly come from the cache).
uint AU_TileLightCount(int tile, bool expandedTiles, uint activeWords)
{
    uint total = 0u;
    for (uint words = activeWords; words != 0u; words &= words - 1u)
        total += uint(bitCount(imageLoad(auLights, AU_TileMaskAddress(tile, findLSB(words), expandedTiles)).r));
    return total;
}

// The lights listed for one 16-block cell (`total` of them), in list order, at most `budget`: all of
// them when the cell lists no more, otherwise a stratified random share (offset 0-1, new every
// frame), each one standing for total / budget lights, so the sum stays the same and the temporal AA
// averages the noise. Every lava block is a light: a cell by a lava lake lists hundreds, and visiting
// all of them per pixel took 25 ms of lighting, and the light fog 120 ms more.
struct AU_TileLights {
    int tile, word;
    bool expandedTiles;
    uint words, mask, prefix, total, count, taken;
    float stride, offset;
};

AU_TileLights AU_TileLightsBegin(int tile, bool expandedTiles, uint activeWords, uint total, int budget, float offset)
{
    AU_TileLights walk;
    walk.tile = tile;
    walk.word = -1;
    walk.expandedTiles = expandedTiles;
    walk.words = activeWords;
    walk.mask = 0u;
    walk.prefix = 0u;
    walk.total = total;
    walk.taken = 0u;
    walk.offset = offset;
    walk.count = min(total, uint(max(budget, 1)));
    walk.stride = walk.count > 0u ? float(total) / float(walk.count) : 1.0;
    return walk;
}

// What each visited light stands for.
float AU_TileLightsWeight(AU_TileLights walk)
{
    return walk.stride;
}

bool AU_TileLightsNext(inout AU_TileLights walk, out uint index)
{
    index = 0u;
    if (walk.taken >= walk.count) return false;
    uint ordinal = walk.count == walk.total ? walk.taken
        : min(uint((float(walk.taken) + walk.offset) * walk.stride), walk.total - 1u);
    walk.taken++;
    // Advance to the list word holding that light.
    while (walk.word < 0 || ordinal >= walk.prefix + uint(bitCount(walk.mask))) {
        if (walk.word >= 0) walk.prefix += uint(bitCount(walk.mask));
        if (walk.words == 0u) { walk.taken = walk.count; return false; }
        walk.word = findLSB(walk.words);
        walk.words &= walk.words - 1u;
        walk.mask = imageLoad(auLights, AU_TileMaskAddress(walk.tile, walk.word, walk.expandedTiles)).r;
    }
    index = uint(walk.word * 32 + AU_NthSetBit(walk.mask, ordinal - walk.prefix));
    return true;
}

vec3 AU_LocalLightingOriginal(vec3 playerPos, vec3 normal, bool flatShading, vec2 randomValue)
{
    int row = 0;
    uint lightCount = min(imageLoad(auLights, ivec2(0, row)).r, AU_PT_MAX_LIGHTS);
    if (lightCount == 0u) return vec3(0.0);
    ivec3 base = ivec3(floor(CameraPosition)) - AU_VOX_HALF;
    vec3 origin = playerPos + CameraPosition + normal * 0.04;
    bool expandedTiles = uHookLocalLightTiles >= 2;
    ivec3 tileCell = ivec3(floor(origin / 16.0)) & (expandedTiles ? ivec3(15, 7, 15) : ivec3(3));
    int tile = expandedTiles ? tileCell.x | (tileCell.y << 4) | (tileCell.z << 7)
                             : tileCell.x | (tileCell.y << 2) | (tileCell.z << 4);
    uint activeWords = imageLoad(auLights, ivec2(expandedTiles ? tile : 1024 + tile, expandedTiles ? 1 : 2)).r;
    if (activeWords == 0u) return vec3(0.0);
    float sampleAngle = randomValue.y * 6.2831853;
    float sampleHeight = randomValue.x * 2.0 - 1.0;
    float sampleRadius = sqrt(max(1.0 - sampleHeight * sampleHeight, 0.0));
    vec3 sampleOffset = vec3(cos(sampleAngle) * sampleRadius, sampleHeight, sin(sampleAngle) * sampleRadius) * AU_LOCAL_LIGHT_SOFTNESS;
    AU_LocalLightSample sampleData;
    sampleData.energy = vec3(0.0);
    sampleData.indices = uvec3(0xffffffffu);
    // The budget applies to the game's expanded cells; the legacy 64-cell layout (wrapping every 64
    // blocks, so most of a list is out of reach) visits every light.
    AU_TileLights walk = AU_TileLightsBegin(tile, expandedTiles, activeWords, AU_TileLightCount(tile, expandedTiles, activeWords),
                                            expandedTiles ? AU_LOCAL_LIGHT_BUDGET : int(AU_PT_MAX_LIGHTS),
                                            fract(randomValue.x * 7.31 + randomValue.y));
    float share = AU_TileLightsWeight(walk);
    uint index;
    while (AU_TileLightsNext(walk, index)) {
        if (index >= lightCount) continue;
        uint packedCell = imageLoad(auLights, ivec2(1 + 2 * int(index), row)).r;
        ivec3 block = base + ((ivec3(packedCell & 255u, (packedCell >> 8) & 127u, (packedCell >> 15) & 255u) - base) & AU_VOX_MASK);
        vec3 centerDelta = AU_EmitterPosition(block, packedCell) - origin;
        if (dot(centerDelta, centerDelta) > pow2(AU_LOCAL_LIGHT_RANGE + AU_LOCAL_LIGHT_SOFTNESS)) continue;
        vec3 position = AU_EmitterPosition(block, packedCell);
        vec3 delta = centerDelta;
        float distanceSquared = dot(delta, delta);
        if (distanceSquared >= pow2(AU_LOCAL_LIGHT_RANGE) || distanceSquared < 0.0001) continue;
        float distanceToLight = sqrt(distanceSquared);
        float facing = flatShading ? 0.7 : max(dot(normal, delta / distanceToLight), 0.0);
        if (facing <= 0.0) continue;
        float attenuation = pow2(max(1.0 - distanceToLight / AU_LOCAL_LIGHT_RANGE, 0.0));
        attenuation /= ((packedCell >> 23) & 31u) != 0u ? 0.0625 + distanceSquared : 1.0 + 0.08 * distanceSquared;
        if (((packedCell >> 23) & 31u) != 0u) attenuation *= AU_EMISSIVE_LIGHT_STRENGTH;
        float cacheWeight = uintBitsToFloat(imageLoad(auLights, ivec2(1024 + int(index), 4)).r);
        vec3 radiance = unpackUnorm4x8(imageLoad(auLights, ivec2(2 + 2 * int(index), row)).r).rgb * (facing * attenuation * cacheWeight * share);
        auLocalLightUnshadowed += radiance;
        uint selectionBits = floatBitsToUint(randomValue.y * 1973.0 + randomValue.x * 9117.0) ^ packedCell * 0x9e3779b9u;
        selectionBits ^= selectionBits >> 16;
        selectionBits *= 0x7feb352du;
        selectionBits ^= selectionBits >> 15;
        selectionBits *= 0x846ca68bu;
        selectionBits ^= selectionBits >> 16;
        float selection = float(selectionBits >> 8) * (1.0 / 16777216.0);
        AU_SelectLocalLight(sampleData, radiance, position + sampleOffset, block, index, selection);
    }
    return AU_ResolveLocalLight(sampleData, origin, randomValue.x);
}
#endif

vec3 AU_LocalLighting(vec3 playerPos, vec3 normal, bool flatShading, vec2 randomValue)
{
#if AU_LOCAL_LIGHTS
    int row = 0;
    uint lightCount = min(imageLoad(auLights, ivec2(0, row)).r, AU_PT_MAX_LIGHTS);
    if (lightCount == 0u) return vec3(0.0);
    ivec3 base = ivec3(floor(CameraPosition)) - AU_VOX_HALF;
    vec3 origin = playerPos + CameraPosition + normal * 0.04;
    AU_LocalLightSample sampleData;
    sampleData.energy = vec3(0.0);
    sampleData.indices = uvec3(0xffffffffu);
    float sampleAngle = randomValue.y * 6.2831853;
    float sampleHeight = randomValue.x * 2.0 - 1.0;
    float sampleRadius = sqrt(max(1.0 - sampleHeight * sampleHeight, 0.0));
    vec3 sampleOffset = vec3(cos(sampleAngle) * sampleRadius, sampleHeight, sin(sampleAngle) * sampleRadius) * AU_LOCAL_LIGHT_SOFTNESS;
    int candidates = min(int(lightCount), AU_LOCAL_LIGHT_CANDIDATES);
    for (int candidate = 0; candidate < candidates; candidate++) {
        uint index = lightCount <= uint(AU_LOCAL_LIGHT_CANDIDATES) ? uint(candidate)
            : min(uint(fract(randomValue.x + float(candidate) * 0.618034) * float(lightCount)), lightCount - 1u);
        uint packedCell = imageLoad(auLights, ivec2(1 + 2 * int(index), row)).r;
        ivec3 block = base + ((ivec3(packedCell & 255u, (packedCell >> 8) & 127u, (packedCell >> 15) & 255u) - base) & AU_VOX_MASK);
        vec3 centerDelta = AU_EmitterPosition(block, packedCell) - origin;
        if (dot(centerDelta, centerDelta) > pow2(AU_LOCAL_LIGHT_RANGE + AU_LOCAL_LIGHT_SOFTNESS)) continue;
        vec3 position = AU_EmitterPosition(block, packedCell);
        vec3 delta = centerDelta;
        float distanceSquared = dot(delta, delta);
        if (distanceSquared >= AU_LOCAL_LIGHT_RANGE * AU_LOCAL_LIGHT_RANGE || distanceSquared < 0.0001) continue;
        float distanceToLight = sqrt(distanceSquared);
        float facing = flatShading ? 0.7 : max(dot(normal, delta / distanceToLight), 0.0);
        float attenuation = pow2(max(1.0 - distanceToLight / AU_LOCAL_LIGHT_RANGE, 0.0));
        attenuation /= ((packedCell >> 23) & 31u) != 0u ? 0.0625 + distanceSquared : 1.0 + 0.08 * distanceSquared;
        if (((packedCell >> 23) & 31u) != 0u) attenuation *= AU_EMISSIVE_LIGHT_STRENGTH;
        vec3 radiance = unpackUnorm4x8(imageLoad(auLights, ivec2(2 + 2 * int(index), row)).r).rgb * (facing * attenuation * AU_CachedLightWeight(index, packedCell));
        auLocalLightUnshadowed += radiance * (float(lightCount) / float(candidates));
        uint selectionBits = floatBitsToUint(randomValue.y * 1973.0 + randomValue.x * 9117.0) ^ uint(candidate) * 0x9e3779b9u;
        selectionBits ^= selectionBits >> 16;
        selectionBits *= 0x7feb352du;
        selectionBits ^= selectionBits >> 15;
        selectionBits *= 0x846ca68bu;
        selectionBits ^= selectionBits >> 16;
        float selection = float(selectionBits >> 8) * (1.0 / 16777216.0);
        AU_SelectLocalLight(sampleData, radiance, position + sampleOffset, block, index, selection);
    }
    return AU_ResolveLocalLight(sampleData, origin, randomValue.x) * (float(lightCount) / float(candidates));
#else
    return vec3(0.0);
#endif
}

#if AU_LOCAL_LIGHTS && AU_LIGHT_FOG
// One light's contribution to the air at a point: its colour, the surfaces' falloff (no facing:
// the air scatters every way) and its cache fade, so a removed light leaves no coloured air.
vec3 AU_LightFogLight(uint index, ivec3 base, vec3 origin, out vec3 position, out ivec3 block, bool tiled)
{
    uint packedCell = imageLoad(auLights, ivec2(1 + 2 * int(index), 0)).r;
    block = base + ((ivec3(packedCell & 255u, (packedCell >> 8) & 127u, (packedCell >> 15) & 255u) - base) & AU_VOX_MASK);
    position = AU_EmitterPosition(block, packedCell);
    vec3 delta = position - origin;
    float distanceSquared = dot(delta, delta);
    if (distanceSquared >= pow2(AU_LOCAL_LIGHT_RANGE)) return vec3(0.0);
    float distanceToLight = sqrt(distanceSquared);
    float attenuation = pow2(max(1.0 - distanceToLight / AU_LOCAL_LIGHT_RANGE, 0.0));
    attenuation /= ((packedCell >> 23) & 31u) != 0u ? 0.25 + distanceSquared : 1.0 + 0.08 * distanceSquared;
    if (((packedCell >> 23) & 31u) != 0u) attenuation *= AU_EMISSIVE_LIGHT_STRENGTH;
    float cacheWeight = tiled ? uintBitsToFloat(imageLoad(auLights, ivec2(1024 + int(index), 4)).r)
                              : AU_CachedLightWeight(index, packedCell);
    return unpackUnorm4x8(imageLoad(auLights, ivec2(2 + 2 * int(index), 0)).r).rgb * (attenuation * cacheWeight);
}

// Coloured light in the air: torches, lanterns, glowing ores and coloured bulbs light the haze
// around them. A short march along the view ray; at each step, the nearby lights of that place's
// light cell (the same culled list the surfaces use) are added up, one of them is picked by its
// brightness and checked against the voxel grid - walls block it, a doorway lets a beam through -
// and the step takes the sum when that light is visible. Unbiased per step; the temporal AA or
// DLSS resolves the noise. Light added to the scene in front of the surface.
vec3 AU_LightFog(vec3 playerPos, float dither, vec2 noise)
{
    uint lightCount = min(imageLoad(auLights, ivec2(0, 0)).r, AU_PT_MAX_LIGHTS);
    if (lightCount == 0u) return vec3(0.0);
    float lViewPos = length(playerPos);
    vec3 dir = playerPos / max(lViewPos, 0.0001);
    float rayEnd = min(lViewPos, AU_LIGHT_FOG_DISTANCE);
    const int steps = AU_LIGHT_FOG_SAMPLES;
    float stepLength = rayEnd / float(steps);
    ivec3 base = ivec3(floor(CameraPosition)) - AU_VOX_HALF;
    bool tiled = uHookLocalLightTiles != 0;
    bool expandedTiles = uHookLocalLightTiles >= 2;
    vec3 fog = vec3(0.0);
    int countedTile = -1;   // steps in the same cell share its light count
    uint countedLights = 0u;
    for (int i = 0; i < steps; i++) {
        vec3 origin = CameraPosition + dir * ((float(i) + dither) * stepLength);
        vec3 total = vec3(0.0), chosenPosition = vec3(0.0);
        ivec3 chosenBlock = ivec3(0);
        float weightSum = 0.0;
        float pick = fract(noise.x + float(i) * 0.618034);
        if (tiled) {
            ivec3 tileCell = ivec3(floor(origin / 16.0)) & (expandedTiles ? ivec3(15, 7, 15) : ivec3(3));
            int tile = expandedTiles ? tileCell.x | (tileCell.y << 4) | (tileCell.z << 7)
                                     : tileCell.x | (tileCell.y << 2) | (tileCell.z << 4);
            uint activeWords = imageLoad(auLights, ivec2(expandedTiles ? tile : 1024 + tile, expandedTiles ? 1 : 2)).r;
            if (tile != countedTile) { countedTile = tile; countedLights = AU_TileLightCount(tile, expandedTiles, activeWords); }
            // An eighth of the surfaces' light budget per step: the steps and frames average each other.
            AU_TileLights walk = AU_TileLightsBegin(tile, expandedTiles, activeWords, countedLights, expandedTiles ? max(AU_LOCAL_LIGHT_BUDGET / 8, 2) : int(AU_PT_MAX_LIGHTS),
                                                    fract(noise.y + float(i) * 0.381966));
            float share = AU_TileLightsWeight(walk);
            uint index;
            while (AU_TileLightsNext(walk, index)) {
                if (index >= lightCount) continue;
                vec3 position; ivec3 block;
                vec3 energy = AU_LightFogLight(index, base, origin, position, block, true) * share;
                float weight = GetLuminance(energy);
                if (weight <= 0.0) continue;
                total += energy;
                weightSum += weight;
                // Pick one in proportion to its brightness (one running choice, re-randomised per light).
                if (fract(pick + float(index) * 0.754878) * weightSum < weight) { chosenPosition = position; chosenBlock = block; }
            }
        } else {
            int candidates = min(int(lightCount), AU_LOCAL_LIGHT_CANDIDATES);
            for (int candidate = 0; candidate < candidates; candidate++) {
                uint index = lightCount <= uint(AU_LOCAL_LIGHT_CANDIDATES) ? uint(candidate)
                    : min(uint(fract(noise.y + float(candidate) * 0.618034) * float(lightCount)), lightCount - 1u);
                vec3 position; ivec3 block;
                vec3 energy = AU_LightFogLight(index, base, origin, position, block, false) * (float(lightCount) / float(candidates));
                float weight = GetLuminance(energy);
                if (weight <= 0.0) continue;
                total += energy;
                weightSum += weight;
                if (fract(pick + float(candidate) * 0.754878) * weightSum < weight) { chosenPosition = position; chosenBlock = block; }
            }
        }
        if (weightSum <= 0.0) continue;
        vec3 delta = chosenPosition - origin;
        float distanceToLight = length(delta);
        ivec3 hitBlock; vec3 hitNormal; float hitDistance; uint hitCell;
        bool blocked = AU_VoxTrace(origin, delta / max(distanceToLight, 0.0001), distanceToLight, 48,
                                   noise.y, hitBlock, hitNormal, hitDistance, hitCell);
        if (!blocked || all(equal(hitBlock, chosenBlock))) fog += total;
    }
    return fog * (stepLength * AU_LIGHT_FOG_DENSITY * 0.015 * AU_LOCAL_LIGHT_STRENGTH);
}
#endif

#ifdef AU_RTX
// Sun light leaving a block face the camera can't see: the block's colour (grey until it
// has been on screen) lit by the sun when a shadow ray through the grid reaches the sky.
vec3 AU_VoxHitLight(ivec3 hb, vec3 hn, uint hv, vec4 hitColor, vec3 hitWorld, float rnd)
{
    float NdotL = (hv & 3u) == AU_VOX_POROUS ? 0.6 : max0(dot(hn, lightVec));
    vec3 alb = hitColor.a > 0.5 ? hitColor.rgb : vec3(0.3);
    vec3 amb = ambientColor * (AU_PT_AMBIENT_BOUNCE * 0.5);   // its sky light is unknown off screen
    if (NdotL <= 0.0 || max(lightColor.r, max(lightColor.g, lightColor.b)) <= 0.0) return alb * amb;
    ivec3 sb; vec3 sn; float st; uint sv;
    if (AU_VoxTrace(hitWorld + hn * 0.02, lightVec, 64.0, 64, rnd, sb, sn, st, sv)) return alb * amb;
    return AU_SunBounce(alb, lightColor, NdotL, 1.0, sunVisibility2 * (1.0 - clamp(IsCameraUnderwater, 0.0, 1.0)), shadowTime) + alb * amb;
}

// Path-traced diffuse bounce + ambient occlusion through the voxel grid; same outputs as
// AU_GetSSGI (AU_RTX.glsl), which it replaces, plus the share of rays that reach the open
// sky. Rays are stratified over the cosine hemisphere with blue noise. A hit on a surface
// that is on screen takes that surface's light from the G-buffer (exact shadows, texture
// colour); hits behind objects or off screen are lit from the grid.
vec2 AU_GIHitAlbedoSky(vec2 uv);   // defined by the including program (G-buffer access)
vec3 AU_GIHitLightData(vec2 uv, out vec2 albedoSky);


vec3 AU_GetVoxelGI(sampler2D depthTex, vec3 playerPos, vec3 normal, float skyLightHere,
                   out float occlusion, out float skyVis)
{
    const int rays = AU_PT_RAYS;
    const float reach = AU_PT_REACH;
    vec3 origin = playerPos + CameraPosition + normal * 0.02;
    vec2 pn = GetDither2(gl_FragCoord.xy);
    vec3 gi = vec3(0.0);
    float occluded = 0.0, escaped = 0.0;
    for (int r = 0; r < rays; r++) {
        float u1 = (float(r) + pn.x) / float(rays);
        float u2 = fract(float(r) * 0.618034 + pn.y);
        vec3 dir = AU_HemisphereDir(normal, u1, u2);
        float rnd = fract(pn.x * 13.73 + pn.y * 7.31 + float(r) * 0.381966);
        ivec3 hb; vec3 hn; float ht; uint hv;
        if (!AU_VoxTrace(origin, dir, reach, 96, rnd, hb, hn, ht, hv)) { escaped += 1.0; continue; }
        occluded += max(0.0, 1.0 - ht / AU_RTX_AO_RADIUS);
        vec3 hitRel = origin - CameraPosition + dir * ht;
        vec2 uv = AU_ProjectToScreen(hitRel);
        float slin = all(greaterThan(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0))) ? AU_SceneLinAt(depthTex, uv) : -1.0;
        float lin = AU_LinOf(hitRel);
        bool onScreen = slin > AU_NEAR_IGNORE && abs(slin - lin) < 0.35 + lin * 0.01;
        vec4 hitCol = onScreen ? vec4(0.0) : AU_VoxGetColor(hb);
        vec2 hitAS;
        vec3 light;
        if (onScreen) light = AU_GIHitLightData(uv, hitAS);
        else {
            light = AU_VoxHitLight(hb, hn, hv, hitCol, hitRel + CameraPosition, rnd);
            hitAS = vec2(hitCol.a > 0.5 ? GetLuminance(hitCol.rgb) : 0.3, skyLightHere);
        }
        // A hit surface passes on part of the sky light it gets itself (more bounces):
        // its albedo x its sky light (off screen: this pixel's).
        escaped += hitAS.x * smoothstep1(clamp(hitAS.y, 0.0, 1.0));
        gi += min(light, vec3(1.5)) * (0.5 + 0.5 * (1.0 - ht / reach));
    }
    occlusion = 1.0 - occluded / float(rays);
    skyVis = escaped / float(rays);
    return gi / float(rays);
}

#endif // AU_RTX

#endif // !AU_VOX_WRITER_ONLY

#endif // AU_VOXEL_INC

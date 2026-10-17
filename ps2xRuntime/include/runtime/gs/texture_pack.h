#pragma once

// Texture replacement packs for the native renderer (gs/texture_pack.cpp, Windows / WIC).
//   <settings folder>/Textures/<id>.png (or .dds, .jpg, .bmp): replaces the game texture with that content id
//   (ps2gs::textureContentId: 16 hex digits). Any size; normal 0..255 alpha.
//   [Display] DumpTextures=1 / PS2X_TEXTURE_DUMP=1: every texture the native renderer decodes is written once
//   to Textures/dump/<id>.png, the starting point for a pack.

#include <cstdint>
#include <vector>

namespace ps2gs::pack
{
    struct Image
    {
        uint32_t w = 0, h = 0;
        std::vector<uint32_t> px; // RGBA8 (0xAABBGGRR), alpha in the scale the game texture used
    };

    // The replacement for a texture, loaded on first use; nullptr when the pack has none. ps2Alpha: the game
    // texture's alpha is 0..0x80 (PS2 scale), so the image's 0..255 alpha is converted to it.
    const Image *find(uint64_t id, bool ps2Alpha);

    // Writes the texture to Textures/dump/<id>.png once (when dumping is on); alpha shown as 0..255.
    void dump(uint64_t id, const uint32_t *px, uint32_t w, uint32_t h, bool ps2Alpha);
}

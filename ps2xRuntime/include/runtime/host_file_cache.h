#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

// Game data served from memory. The disc folders the game loads from mid-match (asset bundles, audio,
// IOP data) are read into memory in the background at boot; any other file of up to 64 MB (the arena
// screen movies) is loaded in the background on its first read. Until a range is loaded, reads go to
// the disk as before. Without this, a cold read on a busy hard disk stalled the game for up to a second.
namespace ps2cache
{
    // Copies [offset, offset + size) of `path` (clipped to the file) into dst when that part is in memory.
    bool read(const std::filesystem::path &path, uint64_t offset, void *dst, size_t size);
}

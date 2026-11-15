#pragma once

#include "core/Export.h"
#include "core/ImageIO.h"

#include <cstdint>
#include <span>
#include <vector>

/**
 * Patch Fill: content-aware filling of a hole from patches of the rest of an image (multi-scale PatchMatch with patch
 * voting). texPatchFill fills a texture set in uv space, island by island; the texture paint tool's projection buffer
 * fills in screen space.
 */
namespace lila::tex
{
struct PatchFillOptions
{
    /** Patch side in texels, odd. */
    int patch = 7;
    /** PatchMatch passes per pyramid level. */
    int iterations = 5;
    uint32_t seed = 1;
};

/**
 * Fills the texels where hole is non-zero in every image (all the same size, matched on all of them together) from patches
 * whose texels all lie where source is non-zero, or anywhere outside the hole when source is empty. Returns the filled
 * images; texels outside the hole are unchanged. Throws std::runtime_error when no patch fits in the source.
 */
LILA_SCENE_API std::vector<image::Image> patchFill(const std::vector<image::Image>& images, std::span<const uint8_t> hole,
                                                   std::span<const uint8_t> source = {}, const PatchFillOptions& options = {});

LILA_SCENE_API void registerPatchFillCommands();
} // namespace lila::tex

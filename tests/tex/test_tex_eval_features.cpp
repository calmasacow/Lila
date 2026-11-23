#include "TexTest.h"

#include "core/Paths.h"

#include <filesystem>

using namespace textest;

namespace
{
namespace fs = std::filesystem;

/** A size x size checkerboard of single texels, 0 and 1, alpha 1. */
image::Image checker(int size)
{
    image::Image img{size, size, std::vector<float>(size_t(size) * size_t(size) * 4)};
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            float* t = &img.rgba[(size_t(y) * size_t(size) + size_t(x)) * 4];
            t[0] = t[1] = t[2] = float((x + y) & 1);
            t[3] = 1.0f;
        }
    return img;
}

std::string writeImage(const fs::path& dir, const std::string& name, const image::Image& img)
{
    const fs::path path = dir / name;
    image::write(path, img, 32);
    return paths::toUtf8(path);
}

Graph imageGraph(const std::string& file, json values = json::object())
{
    Graph g;
    values["file"] = file;
    add(g, "I", "image", values);
    return g;
}
} // namespace

TEST_CASE("tex eval features: image mip levels pack after the image, area averaged")
{
    image::Image img{8, 4, std::vector<float>(8 * 4 * 4, 0.0f)};
    for (size_t i = 0; i < 32; ++i)
        img.rgba[i * 4] = float(i);
    uint32_t flags = 0;
    image::Image packed = withMipLevels(img, flags);
    // 8x4, 4x2, 2x1, 1x1: 43 texels in rows of 8.
    CHECK(((flags >> 2) & 31u) == 4u);
    CHECK((flags >> 8) == 4u);
    CHECK((flags & 3u) == 0u);
    CHECK(packed.width == 8);
    CHECK(packed.height == 6);
    const float* level1 = &packed.rgba[32 * 4];
    CHECK(level1[0] == doctest::Approx((0.0 + 1.0 + 8.0 + 9.0) / 4.0));
    const float* last = &packed.rgba[42 * 4];
    CHECK(last[0] == doctest::Approx(15.5));

    uint32_t none = 7;
    CHECK(withMipLevels(image::Image{1, 1, {1, 1, 1, 1}}, none).width == 1);
    CHECK(none == 0u);
}

TEST_CASE("tex eval features: an image read at its own uv has no mips, a tiled one does")
{
    const fs::path dir = tempDir("eval_features_mips_plan");
    const std::string file = writeImage(dir, "checker.exr", checker(16));
    auto mips = [&](json values) {
        Plan p = plan(imageGraph(file, values), "I", "out", 16);
        REQUIRE_MESSAGE(p.error.empty(), p.error);
        for (const BufferDesc& b : p.buffers)
            if (b.kind == BufferKind::Image)
                return b.mips;
        FAIL("no image buffer");
        return false;
    };
    CHECK_FALSE(mips({{"tiles", 1}}));
    CHECK(mips({{"tiles", 4}}));
}

TEST_CASE("tex eval features: a tiled image reads the prefiltered level instead of aliasing")
{
    const fs::path dir = tempDir("eval_features_mips_read");
    const std::string file = writeImage(dir, "checker.exr", checker(64));

    // At its own size the checker reads back exactly.
    const image::Image same = eval(imageGraph(file, {{"tiles", 1}}), "I", "out", 64);
    CHECK(texel(same, 0, 0)[0] == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(texel(same, 1, 0)[0] == doctest::Approx(1.0f).epsilon(1e-4));

    // Three repeats: each output texel covers 3 x 3 checks, so the read lands between levels 1 and 2, both grey.
    const image::Image three = eval(imageGraph(file, {{"tiles", 3}}), "I", "out", 64);
    double worst = 0.0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            worst = std::max(worst, std::abs(double(texel(three, x, y)[0]) - 0.5));
    CHECK(worst < 0.01);

    // Noise read four times over lands on level 2 texel for texel: the 4 x 4 averages.
    image::Image noise = checker(64);
    for (size_t i = 0; i < noise.rgba.size(); ++i)
        if (i % 4 != 3)
            noise.rgba[i] = float((i / 4 * 2654435761u) % 1000u) / 999.0f;
    const std::string noisy = writeImage(dir, "noise.exr", noise);
    const image::Image tiled = eval(imageGraph(noisy, {{"tiles", 4}}), "I", "out", 64);
    const image::Image level2 = downsample(noise, 4);
    worst = 0.0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            worst = std::max(worst, std::abs(double(texel(tiled, x, y)[0]) - double(texel(level2, x % 16, y % 16)[0])));
    CHECK(worst < 1e-4);
}

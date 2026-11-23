#include "TexTest.h"

#include "tex/NodeDef.h"
#include "tex/TextRaster.h"

using namespace textest;

namespace
{
struct Box
{
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    double sum = 0.0;
    int width() const { return x1 - x0 + 1; }
    int height() const { return y1 - y0 + 1; }
};

/** Where a mask covers more than half a texel, and its total. */
Box coverage(const image::Image& img)
{
    Box b;
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x)
        {
            const float v = texel(img, x, y)[0];
            b.sum += v;
            if (v > 0.5f)
            {
                b.x0 = std::min(b.x0, x), b.y0 = std::min(b.y0, y);
                b.x1 = std::max(b.x1, x), b.y1 = std::max(b.y1, y);
            }
        }
    return b;
}

image::Image textMask(json values, int res = 128)
{
    Graph g;
    add(g, "T", "text", std::move(values));
    return eval(g, "T", "mask", res, PortType::Gray);
}
} // namespace

TEST_CASE("text: draws its words into a mask, sized and placed in uv")
{
    REQUIRE(findNode("text"));
    const Box b = coverage(textMask({{"text", "HHHH"}, {"size", 0.25}}));
    REQUIRE(b.x1 >= 0);
    // Centred on (0.5, 0.5), wider than tall.
    CHECK(std::abs((b.x0 + b.x1) * 0.5 - 63.5) < 3.0);
    CHECK(std::abs((b.y0 + b.y1) * 0.5 - 63.5) < 3.0);
    CHECK(b.width() > 2 * b.height());
    // Cap height of Inter is about 0.73 em: 0.25 * 128 * 0.73.
    CHECK(b.height() > 20);
    CHECK(b.height() < 27);

    const Box big = coverage(textMask({{"text", "HHHH"}, {"size", 0.4}}));
    CHECK(big.sum > b.sum * 2.0);

    const Box empty = coverage(textMask({{"text", ""}}));
    CHECK(empty.sum == 0.0);
}

TEST_CASE("text: alignment, rotation, line spacing and position")
{
    const Box left = coverage(textMask({{"text", "HHH"}, {"alignment", "left"}, {"positionX", 0.25}}));
    CHECK(std::abs(left.x0 - 32) <= 3);
    const Box right = coverage(textMask({{"text", "HHH"}, {"alignment", "right"}, {"positionX", 0.75}}));
    CHECK(std::abs(right.x1 - 95) <= 3);

    const Box flat = coverage(textMask({{"text", "HHHH"}, {"size", 0.2}}));
    const Box turned = coverage(textMask({{"text", "HHHH"}, {"size", 0.2}, {"rotation", 90.0}}));
    CHECK(std::abs(turned.width() - flat.height()) <= 2);
    CHECK(std::abs(turned.height() - flat.width()) <= 2);

    const Box one = coverage(textMask({{"text", "H\nH"}, {"size", 0.15}, {"lineSpacing", 1.0}}));
    const Box two = coverage(textMask({{"text", "H\nH"}, {"size", 0.15}, {"lineSpacing", 2.0}}));
    CHECK(two.height() > one.height() + 10);

    const Box low = coverage(textMask({{"text", "H"}, {"positionY", 0.25}}));
    CHECK((low.y0 + low.y1) * 0.5 > 90.0);
}

TEST_CASE("text: tile wraps letters across the edges; off, they are cut")
{
    auto leftColumns = [](const image::Image& m) {
        double s = 0.0;
        for (int y = 0; y < m.height; ++y)
            for (int x = 0; x < 8; ++x)
                s += texel(m, x, y)[0];
        return s;
    };
    CHECK(leftColumns(textMask({{"text", "HHHH"}, {"positionX", 1.0}, {"tile", true}})) > 10.0);
    CHECK(leftColumns(textMask({{"text", "HHHH"}, {"positionX", 1.0}, {"tile", false}})) == 0.0);
}

TEST_CASE("text: colour output mixes background into colour by the mask; fonts differ")
{
    Graph g;
    add(g, "T", "text", {{"text", "Lila"}, {"color", json::array({1.0, 0.5, 0.0, 1.0})}, {"background", json::array({0.0, 0.0, 0.25, 1.0})}});
    const image::Image mask = eval(g, "T", "mask", 64, PortType::Gray);
    const image::Image col = eval(g, "T", "out", 64);
    for (int i = 0; i < 64 * 64; i += 7)
    {
        const float m = mask.rgba[size_t(i) * 4];
        CHECK(col.rgba[size_t(i) * 4 + 0] == doctest::Approx(m).epsilon(1e-4));
        CHECK(col.rgba[size_t(i) * 4 + 1] == doctest::Approx(0.5f * m).epsilon(1e-4));
        CHECK(col.rgba[size_t(i) * 4 + 2] == doctest::Approx(0.25f * (1.0f - m)).epsilon(1e-4));
    }
    const image::Image mono = textMask({{"text", "Lila"}, {"font", "JetBrains Mono"}}, 64);
    CHECK(meanDiff(mono, mask) > 0.01);
}

TEST_CASE("text: a word edit redraws the image but keeps the plan's structure")
{
    Graph a, b;
    add(a, "T", "text", {{"text", "one"}});
    add(b, "T", "text", {{"text", "two"}, {"size", 0.3}, {"rotation", 15.0}});
    const Plan pa = plan(a, "T", "mask", 64, PortType::Gray), pb = plan(b, "T", "mask", 64, PortType::Gray);
    REQUIRE(pa.error.empty());
    CHECK(pa.structureHash == pb.structureHash);
    const auto it = std::find_if(pa.buffers.begin(), pa.buffers.end(), [](const BufferDesc& d) { return d.kind == BufferKind::Image; });
    REQUIRE(it != pa.buffers.end());
    CHECK(generatedSource(it->name));
    CHECK(textSourceImage(it->name)->width == 64);
}


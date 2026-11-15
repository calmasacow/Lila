#include "TexTest.h"

#include "core/ImageIO.h"
#include "geo/Attribute.h"
#include "scene/MeshNodes.h"
#include "tex/TextureSet.h"

using namespace textest;

namespace
{
/** A unit cube of six quads, each face on its own cell of a 3 x 2 uv grid with a margin. */
geo::Mesh uvCube()
{
    const std::vector<glm::dvec3> pts = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    const std::vector<geo::Index> sizes(6, 4);
    const std::vector<geo::Index> idx = {0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 2, 3, 7, 6, 1, 2, 6, 5, 0, 4, 7, 3};
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    int face = 0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const glm::vec2 o(float(face % 3) / 3.0f + 0.02f, (face < 3 ? 0.0f : 0.5f) + 0.02f);
        const glm::vec2 size(1.0f / 3.0f - 0.04f, 0.5f - 0.04f);
        const glm::vec2 st[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        geo::Index c = m.faceCorner(f);
        for (int k = 0; k < 4; ++k, c = m.cornerNext(c))
            uv[c] = o + st[k] * size;
        ++face;
    }
    return m;
}

image::Image channel(const char* set)
{
    scene::Node& n = *tex::findSet(scene::Graph::current(), set);
    const tex::SetSettings s = tex::readSettings(n);
    return tex::evaluateOutputs(n, tex::channelRequests(tex::readGraph(n), s), 1001, 128, 128, "cpu").at("baseColor");
}
} // namespace

TEST_CASE("tex ptex: a cube's set exported to .ptx and imported back matches within tolerance")
{
    run("newScene");
    scene::Graph& g = scene::Graph::current();
    scene::Node* cube = g.createNode(*scene::types::find("mesh"), "Cube");
    g.setValue({cube->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(uvCube())));

    // A smooth colour ramp over the uvs, so face texels resample cleanly.
    const std::filesystem::path dir = tempDir("ptex");
    image::Image ramp;
    ramp.width = ramp.height = 128;
    ramp.rgba.resize(size_t(128) * 128 * 4);
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x)
        {
            float* p = &ramp.rgba[(size_t(y) * 128 + x) * 4];
            p[0] = x / 127.0f;
            p[1] = y / 127.0f;
            p[2] = 0.5f;
            p[3] = 1.0f;
        }
    image::writeExr(dir / "ramp.exr", 128, 128, ramp.rgba, 32);
    for (const char* set : {"Src", "Dst"})
        run("texCreateSet", {{"args", {set}}, {"resolution", 128}});
    run("texAddLayer", {{"args", {"Src", "fill"}}, {"material", {{"baseColor", (dir / "ramp.exr").string()}}}});

    const std::string ptx = (dir / "cube.ptx").string();
    const json out = run("texExportPtex", {{"args", {"Src"}}, {"mesh", cube->path()}, {"file", ptx}, {"faceResolution", 5}});
    CHECK(out["faces"] == 6);
    CHECK(std::filesystem::exists(ptx));
    const json in = run("texImportPtex", {{"args", {"Dst"}}, {"mesh", cube->path()}, {"file", ptx}, {"folder", dir.string()}});
    CHECK(in.contains("id"));

    const image::Image a = channel("Src"), b = channel("Dst");
    // Inside each face's cell, away from the margin.
    double worst = 0.0;
    for (int face = 0; face < 6; ++face)
        for (float s : {0.2f, 0.5f, 0.8f})
            for (float t : {0.2f, 0.5f, 0.8f})
            {
                const float u = float(face % 3) / 3.0f + 0.02f + s * (1.0f / 3.0f - 0.04f);
                const float v = (face < 3 ? 0.0f : 0.5f) + 0.02f + t * (0.5f - 0.04f);
                const int x = int(u * 128), y = int((1.0f - v) * 128);
                for (int c = 0; c < 3; ++c)
                    worst = std::max(worst, double(std::abs(texel(a, x, y)[c] - texel(b, x, y)[c])));
            }
    CHECK(worst < 0.03);

    CHECK_THROWS(run("texImportPtex", {{"args", {"Dst"}}, {"mesh", cube->path()}, {"file", (dir / "missing.ptx").string()}}));
}

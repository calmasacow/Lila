#include "TestUtil.h"
#include "Generators.h"

#include "geo/Mesh.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <tuple>
#include <unordered_map>

using namespace lila;
using namespace settlement;

namespace
{
/** Up-facing faces of a mesh as XZ triangles with their heights, binned for point lookups. */
struct Cover
{
    struct Tri
    {
        glm::dvec3 a, b, c;
        int32_t material;
    };
    std::vector<Tri> tris;
    std::unordered_map<int64_t, std::vector<uint32_t>> bins;
    static constexpr double kCell = 1000.0;

    static int64_t key(int x, int z) { return (int64_t(x) << 32) ^ int64_t(uint32_t(z)); }

    explicit Cover(const geo::Mesh& m)
    {
        const auto mat = m.faceAttrs().get<int32_t>("material");
        for (geo::Index f = 0; f < geo::Index(m.numFaces()); ++f)
        {
            const std::vector<geo::Index> pts = m.facePoints(f);
            glm::dvec3 n(0.0);
            for (size_t k = 0; k < pts.size(); ++k)
            {
                const glm::dvec3 a = m.position(pts[k]), b = m.position(pts[(k + 1) % pts.size()]);
                n += glm::dvec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
            }
            if (n.y < 0.5 * glm::length(n))
                continue;
            for (size_t k = 1; k + 1 < pts.size(); ++k)
            {
                const Tri t{m.position(pts[0]), m.position(pts[k]), m.position(pts[k + 1]), mat.empty() ? 0 : mat[f]};
                const uint32_t id = uint32_t(tris.size());
                tris.push_back(t);
                const double x0 = std::min({t.a.x, t.b.x, t.c.x}), x1 = std::max({t.a.x, t.b.x, t.c.x});
                const double z0 = std::min({t.a.z, t.b.z, t.c.z}), z1 = std::max({t.a.z, t.b.z, t.c.z});
                for (int x = int(std::floor(x0 / kCell)); x <= int(std::floor(x1 / kCell)); ++x)
                    for (int z = int(std::floor(z0 / kCell)); z <= int(std::floor(z1 / kCell)); ++z)
                        bins[key(x, z)].push_back(id);
            }
        }
    }
    /** The triangle over p, or nullptr. */
    const Tri* at(Vec2 p) const
    {
        const auto it = bins.find(key(int(std::floor(p.x / kCell)), int(std::floor(p.y / kCell))));
        if (it == bins.end())
            return nullptr;
        for (uint32_t i : it->second)
        {
            const Tri& t = tris[i];
            const double d1 = cross(Vec2(t.b.x - t.a.x, t.b.z - t.a.z), Vec2(p.x - t.a.x, p.y - t.a.z));
            const double d2 = cross(Vec2(t.c.x - t.b.x, t.c.z - t.b.z), Vec2(p.x - t.b.x, p.y - t.b.z));
            const double d3 = cross(Vec2(t.a.x - t.c.x, t.a.z - t.c.z), Vec2(p.x - t.c.x, p.y - t.c.z));
            if ((d1 >= 0.0 && d2 >= 0.0 && d3 >= 0.0) || (d1 <= 0.0 && d2 <= 0.0 && d3 <= 0.0))
                return &t;
        }
        return nullptr;
    }
};

Layout city(double size, uint32_t seed)
{
    Options o;
    REQUIRE(builtinProfile("mexicanCity", &o.profile));
    o.seed = seed;
    Site s;
    s.size = size;
    return build(s, Ground::plane(), o);
}

/** A terrain like a heightfield's: one grid of quads, each split from its first corner, over rolling ground. */
geo::Mesh rollingTerrain(double half, double cell)
{
    const int n = int(std::ceil(2.0 * half / cell));
    std::vector<glm::dvec3> pts;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
        {
            const double x = -half + i * cell, z = -half + j * cell;
            pts.push_back({x, 300.0 * std::sin(x / 1900.0) * std::cos(z / 1700.0) + 0.002 * x * x / 100.0, z});
        }
    std::vector<geo::Index> sizes, corners;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
        {
            const geo::Index p = geo::Index(j * (n + 1) + i);
            sizes.push_back(4);
            corners.insert(corners.end(), {p, p + geo::Index(n + 1), p + geo::Index(n + 2), p + 1});
        }
    return geo::Mesh::fromPolygons(pts, sizes, corners);
}

bool insidePolygon(const Polygon& poly, Vec2 p) { return contains(poly.pts, p); }
} // namespace

TEST_CASE("settlement blocks: a concave face whose short side collapses when moved in keeps its whole inset")
{
    // An L of 20 m arms with a 1 m chamfer at one outer corner: moved in 4.5 m the chamfer turns back on itself.
    Polygon l = makePolygon({{0.0, 0.0}, {3900.0, 0.0}, {4000.0, 100.0}, {4000.0, 2000.0}, {2000.0, 2000.0}, {2000.0, 4000.0}, {0.0, 4000.0}});
    const std::vector<double> d(l.size(), 450.0);
    const Polygon in = inset(l, d);
    REQUIRE(in.size() >= 6);
    // The exact inset of the unchamfered L: 31 x 11 m plus 11 x 20 m.
    CHECK(area(in) == doctest::Approx(3100.0 * 1100.0 + 1100.0 * 2000.0).epsilon(0.01));
    for (const Vec2& q : in.pts)
        CHECK(insidePolygon(l, q));
}

TEST_CASE("settlement ground: on a heightfield's grid the ground lies on the terrain drawn, never under it")
{
    const Layout l = city(30000.0, 3);
    const geo::Mesh terrain = rollingTerrain(40000.0, 780.0);
    const Ground g = Ground::fromMesh(terrain, 200.0);
    REQUIRE(g.lattice() != nullptr);
    CHECK(g.lattice()->cell.x == doctest::Approx(780.0));
    CHECK(g.lattice()->mainDiagonal);

    const MeshParts parts = toMeshes(l, g);
    const geo::Mesh& m = parts.ground;
    REQUIRE(m.numFaces() > 100);
    int samples = 0, under = 0;
    double worst = 0.0;
    for (geo::Index f = 0; f < geo::Index(m.numFaces()); ++f)
    {
        const std::vector<geo::Index> pts = m.facePoints(f);
        for (size_t k = 1; k + 1 < pts.size(); ++k)
        {
            const glm::dvec3 a = m.position(pts[0]), b = m.position(pts[k]), c = m.position(pts[k + 1]);
            for (const glm::dvec3& w : {glm::dvec3(1.0, 1.0, 1.0) / 3.0, glm::dvec3(0.5, 0.5, 0.0), glm::dvec3(0.0, 0.5, 0.5), glm::dvec3(0.5, 0.0, 0.5)})
            {
                const glm::dvec3 p = a * w.x + b * w.y + c * w.z;
                const double gap = p.y - g.surfaceHeight({p.x, p.z});
                worst = std::min(worst, gap);
                under += gap < 0.5 ? 1 : 0;
                ++samples;
            }
        }
    }
    CAPTURE(samples);
    CAPTURE(worst);
    CHECK(under == 0);
}

TEST_CASE("settlement ground: blocks are covered whole, open blocks only near their streets and buildings")
{
    const Layout l = city(60000.0, 5);
    const MeshParts parts = toMeshes(l, Ground::plane());
    const Cover ground(parts.ground);
    REQUIRE(!ground.tris.empty());

    auto open = [](const Block& b) { return std::ranges::any_of(b.outline.tag, [](int t) { return t < 0; }); };
    REQUIRE(std::ranges::any_of(l.blocks, open));
    REQUIRE(std::ranges::any_of(l.blocks, [&](const Block& b) { return !open(b); }));
    auto streetDistance = [&](Vec2 p) {
        double best = 1e300;
        for (size_t e = 0; e < l.graph.edges.size(); ++e)
        {
            if (!l.graph.isStreet(int(e)))
                continue;
            const Vec2 a = l.graph.nodes[size_t(l.graph.edges[e].a)].p, b = l.graph.nodes[size_t(l.graph.edges[e].b)].p;
            const double t = std::clamp(glm::dot(p - a, b - a) / std::max(glm::dot(b - a, b - a), 1e-9), 0.0, 1.0);
            best = std::min(best, glm::length(p - (a + (b - a) * t)) - 0.5 * edgeWidth(l, int(e)));
        }
        return best;
    };
    const double reach = std::max(l.profile.lotDepth, 500.0);

    // No holes: points inside a closed block are all covered, and in an open block those near a street.
    int closedSamples = 0, closedHoles = 0, openSamples = 0, openHoles = 0;
    for (const Block& b : l.blocks)
    {
        Vec2 lo(1e300), hi(-1e300);
        for (const Vec2& q : b.outline.pts)
            lo = glm::min(lo, q), hi = glm::max(hi, q);
        for (double x = lo.x + 53.0; x < hi.x; x += 271.0)
            for (double z = lo.y + 61.0; z < hi.y; z += 263.0)
            {
                const Vec2 p(x, z);
                if (!insidePolygon(b.outline, p) || boundaryDistance(b.outline.pts, p) < 30.0)
                    continue;
                if (!open(b))
                {
                    ++closedSamples;
                    closedHoles += ground.at(p) ? 0 : 1;
                }
                else if (streetDistance(p) < reach - 100.0)
                {
                    ++openSamples;
                    openHoles += ground.at(p) ? 0 : 1;
                }
            }
    }
    CAPTURE(closedSamples);
    CAPTURE(openSamples);
    CHECK(closedSamples > 1000);
    CHECK(openSamples > 100);
    CHECK(closedHoles == 0);
    CHECK(openHoles <= openSamples / 200);

    // No spill: every ground triangle lies in a block (to within the trace's grid), and in open land only near a street
    // or a building.
    std::vector<const Polygon*> buildings;
    for (const Envelope& env : l.envelopes)
        buildings.push_back(&env.footprint);
    int spilled = 0, far = 0;
    for (const Cover::Tri& t : ground.tris)
    {
        const Vec2 c((t.a.x + t.b.x + t.c.x) / 3.0, (t.a.z + t.b.z + t.c.z) / 3.0);
        const auto block = std::ranges::find_if(l.blocks, [&](const Block& b) { return insidePolygon(b.outline, c) || boundaryDistance(b.outline.pts, c) < 150.0; });
        if (block == l.blocks.end())
        {
            ++spilled;
            continue;
        }
        if (open(*block) && streetDistance(c) > reach + 200.0 &&
            std::ranges::none_of(buildings, [&](const Polygon* fp) { return insidePolygon(*fp, c) || boundaryDistance(fp->pts, c) < 400.0; }))
            ++far;
    }
    CHECK(spilled == 0);
    CHECK(far == 0);
}

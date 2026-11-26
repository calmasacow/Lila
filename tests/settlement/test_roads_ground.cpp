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

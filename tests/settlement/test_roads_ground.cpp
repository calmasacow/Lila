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

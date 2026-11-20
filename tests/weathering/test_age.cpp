#include "TestUtil.h"

#include "Pieces.h"

#include "scene/Transform.h"

#include <algorithm>
#include <set>

using namespace weatheringtest;

namespace
{
glm::dvec3 worldPos(const std::string& path)
{
    g().evaluate();
    scene::Node* n = g().find(path);
    REQUIRE(n);
    return glm::dvec3(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
}

/** Distance in x, z from a point to an axis-aligned box footprint (0 inside). */
double outside(const glm::dvec3& p, const glm::dvec3& lo, const glm::dvec3& hi)
{
    const double dx = std::max({lo.x - p.x, 0.0, p.x - hi.x});
    const double dz = std::max({lo.z - p.z, 0.0, p.z - hi.z});
    return std::sqrt(dx * dx + dz * dz);
}

/** The house, the ground and three items: two chunks and a tall plank. */
void houseScene()
{
    fresh();
    cube("House", {0, 0, 0}, {400, 300, 400});
    run("polyPlane", {{"name", "Ground"}, {"width", 2000.0}, {"depth", 2000.0}, {"subdivisionsX", 4}, {"subdivisionsZ", 4}});
    cube("ChunkA", {3000, 0, 0}, {14, 10, 12});
    cube("ChunkB", {3100, 0, 0}, {22, 14, 18});
    cube("Plank", {3200, 0, 0}, {12, 150, 2});
}

json band(int64_t seed, bool realize = true)
{
    return run("baseDressing", {{"args", {"House"}},
                                {"items", {"ChunkA", "ChunkB", "Plank"}},
                                {"width", 90.0},
                                {"density", 12.0},
                                {"realize", realize},
                                {"seed", seed}});
}
} // namespace

TEST_CASE("weathering: base dressing stays in the band round the footprint")
{
    houseScene();
    const json r = band(1);
    REQUIRE(r["count"].get<int>() > 20);
    CHECK(r["ground"].is_array());
    CHECK(r["ground"].size() == 1);
    int leaning = 0;
    for (const json& n : r["nodes"])
    {
        const glm::dvec3 p = worldPos(n.get<std::string>());
        const double d = outside(p, {-200, 0, -200}, {200, 300, 200});
        CHECK(d > 0.0);
        CHECK(d < 90.0 + 30.0);
        CHECK(std::abs(p.y) < 1.0);
    }
    leaning = r["leaning"].get<int>();
    CHECK(leaning > 0);
    CHECK(r["perItem"]["Plank"].get<int>() >= leaning);
}

TEST_CASE("weathering: base dressing is the same for the same seed and differs for another")
{
    houseScene();
    const json a = band(4);
    std::vector<glm::dvec3> first;
    for (const json& n : a["nodes"])
        first.push_back(worldPos(n.get<std::string>()));
    run("delete", {{"args", {a["group"]}}});
    const json b = band(4);
    REQUIRE(b["nodes"].size() == first.size());
    for (size_t k = 0; k < first.size(); ++k)
        CHECK(glm::length(worldPos(b["nodes"][k].get<std::string>()) - first[k]) < 1e-6);
    const json c = band(5);
    bool differs = c["nodes"].size() != first.size();
    for (size_t k = 0; !differs && k < first.size(); ++k)
        differs = glm::length(worldPos(c["nodes"][k].get<std::string>()) - first[k]) > 1e-3;
    CHECK(differs);
}

TEST_CASE("weathering: base dressing follows an L-shaped outline and keeps out of both wings")
{
    fresh();
    cube("WingA", {0, 0, 0}, {600, 300, 200});
    cube("WingB", {-200, 0, 300}, {200, 300, 400});
    cube("Chunk", {3000, 0, 0}, {15, 10, 15});
    const json r = run("baseDressing", {{"args", {"WingA", "WingB"}}, {"items", {"Chunk"}}, {"width", 60.0}, {"density", 20.0}, {"realize", true}});
    REQUIRE(r["count"].get<int>() > 30);
    CHECK(r["ground"] == "plane");
    for (const json& n : r["nodes"])
    {
        const glm::dvec3 p = worldPos(n.get<std::string>());
        CHECK(outside(p, {-300, 0, -100}, {300, 300, 100}) > 0.0);
        CHECK(outside(p, {-300, 0, 100}, {-100, 300, 500}) > 0.0);
    }
}

TEST_CASE("weathering: live base dressing rebuilds when the host moves")
{
    houseScene();
    const json r = band(2, false);
    REQUIRE(r.contains("host"));
    const std::string host = r["host"].get<std::string>();
    auto centre = [&] {
        const geo::Mesh m = meshOf(host);
        glm::dvec3 lo(1e300), hi(-1e300);
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
            {
                lo = glm::min(lo, m.position(p));
                hi = glm::max(hi, m.position(p));
            }
        REQUIRE(lo.x < hi.x);
        return (lo + hi) * 0.5;
    };
    const glm::dvec3 before = centre();
    CHECK(std::abs(before.x) < 60.0);
    run("move", {{"args", {"House"}}, {"by", {500, 0, 0}}});
    const glm::dvec3 after = centre();
    CHECK(after.x - before.x == doctest::Approx(500.0).epsilon(0.1));
}

TEST_CASE("weathering: placeholders by role and style, and the user's own pieces win")
{
    fresh();
    run("weatheringPlaceholders", {{"force", true}});
    const std::vector<std::string> toon = weathering::roleAssets("rubble", weathering::Style::Toon);
    REQUIRE(!toon.empty());
    for (const std::string& id : toon)
        CHECK(id.find("Toon") != std::string::npos);
    const std::vector<std::string> real = weathering::roleAssets("rubble", weathering::Style::Realistic);
    REQUIRE(!real.empty());
    for (const std::string& id : real)
        CHECK(id.find("Toon") == std::string::npos);

    cube("MyRubble", {0, 0, 0}, {20, 15, 20});
    run("weatheringAddPieces", {{"args", {"MyRubble"}}, {"role", "rubble"}, {"thumbnail", false}});
    const std::vector<std::string> mine = weathering::roleAssets("rubble", weathering::Style::Toon);
    REQUIRE(mine.size() == 1);
    CHECK(mine[0] == "Weathering/rubble/MyRubble");
}

TEST_CASE("weathering: age runs a preset in either style, with the user's pieces swapped in")
{
    houseScene();
    const json presets = run("agePresets");
    std::set<std::string> names;
    for (const json& p : presets)
        names.insert(p["name"].get<std::string>());
    for (const char* n : {"Concrete Debris", "Bricks and Rocks", "Leaning Planks", "Demolished Building", "Burned Yard", "Abandoned", "Overgrown"})
    {
        CHECK(names.contains(n));
        CHECK(names.contains(std::string(n) + " Toon"));
    }
    const json r = run("age", {{"args", {"House"}}, {"preset", "Concrete Debris"}, {"style", "toon"}});
    CHECK(r["preset"] == "Concrete Debris Toon");
    CHECK(!r["group"].get<std::string>().empty());
    CHECK(g().find(r["group"].get<std::string>()));

    const json mine = run("age", {{"args", {"House"}}, {"preset", "Concrete Debris"}, {"slots", {{"rubble", {"ChunkA", "ChunkB"}}}}, {"seed", 3}});
    const json step = mine["steps"][0]["result"];
    CHECK(step["perItem"].contains("ChunkA"));
    CHECK(step["perItem"].contains("ChunkB"));
    CHECK(step["perItem"].size() == 2);
}

TEST_CASE("weathering: realized base dressing settles with Stagehand")
{
    houseScene();
    const json r = run("baseDressing", {{"args", {"House"}}, {"items", {"ChunkA", "ChunkB"}}, {"width", 60.0}, {"density", 6.0}, {"realize", true}, {"settle", true}});
    REQUIRE(r["count"].get<int>() > 0);
    CHECK(r.contains("atRest"));
    CHECK(r["atRest"].get<int>() >= r["count"].get<int>() / 2);
}

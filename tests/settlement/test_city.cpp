#include "TestUtil.h"
#include "Generators.h"
#include "Parcels.h"
#include "Streetscape.h"

#include "curve/CurveNodes.h"
#include "extensions/ExtensionManager.h"
#include "geo/Mesh.h"
#include "gn/NodeLib.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <map>

using namespace lila;
using namespace settlement;
using namespace settlementtest;

namespace
{
Layout mexicanCity(double size, uint32_t seed)
{
    Options o;
    REQUIRE(builtinProfile("mexicanCity", &o.profile));
    o.seed = seed;
    Site s;
    s.size = size;
    return build(s, Ground::plane(), o);
}

/** Faces per material slot name. */
std::map<std::string, int> facesByMaterial(const geo::Mesh& m)
{
    std::map<std::string, int> out;
    const auto mat = m.faceAttrs().get<int32_t>("material");
    for (int32_t id : mat)
        ++out[size_t(id) < m.materialNames().size() ? m.materialNames()[size_t(id)] : "?"];
    return out;
}

/** Faces of the mesh whose Newell normal points down. */
int facesFacingDown(const geo::Mesh& m)
{
    int down = 0;
    for (geo::Index f = 0; f < geo::Index(m.numFaces()); ++f)
    {
        glm::dvec3 n(0.0);
        std::vector<glm::dvec3> pts;
        for (geo::Index c : m.faceCorners(f))
            pts.push_back(m.position(m.cornerPoint(c)));
        for (size_t k = 0; k < pts.size(); ++k)
        {
            const glm::dvec3 &a = pts[k], &b = pts[(k + 1) % pts.size()];
            n += glm::dvec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
        }
        down += n.y < 0.0 ? 1 : 0;
    }
    return down;
}

void loadCityPackages()
{
    if (!ops::find("curve"))
    {
        lila::curve::registerTypes();
        lila::curve::registerCommands();
    }
    if (!gn::findDecl("Settlement"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_SETTLEMENT_PACKAGE) == "Settlement");
    if (!ops::find("terrainCreate"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
}
} // namespace

TEST_CASE("settlement mexicanCity: perimeter blocks built wall to wall, low and flat, narrow streets")
{
    const Layout l = mexicanCity(100000.0, 2);
    CHECK(streetWidth(l.profile, StreetClass::Local) == doctest::Approx(900.0));
    int perimeter = 0, ordinary = 0;
    double blockArea = 0.0;
    for (const Block& b : l.blocks)
        if (b.type != BlockType::Park && b.type != BlockType::Plaza && b.type != BlockType::Market)
        {
            ++ordinary;
            perimeter += b.type == BlockType::Perimeter ? 1 : 0;
            blockArea += b.area;
        }
    REQUIRE(ordinary > 20);
    CHECK(perimeter == ordinary);
    REQUIRE(l.envelopes.size() > 100);
    int party = 0, flat = 0, low = 0;
    double built = 0.0;
    for (const Envelope& env : l.envelopes)
    {
        built += area(env.footprint) / kM2;
        party += std::ranges::count(env.footprint.tag, 1) > 0 ? 1 : 0;
        flat += env.roof == "flat" ? 1 : 0;
        low += env.floors <= 2 ? 1 : 0;
    }
    const double n = double(l.envelopes.size());
    CAPTURE(built / blockArea);
    CHECK(built / blockArea > 0.45);
    CHECK(party > 0.7 * n);
    CHECK(flat > 0.9 * n);
    CHECK(low > 0.4 * n);
}

TEST_CASE("settlement meshes: streets are asphalt between sidewalks, junctions patched, blocks and lots show their ground")
{
    Layout l = mexicanCity(60000.0, 3);
    // A park lot shows grass over the block's yards.
    Lot* park = nullptr;
    for (Lot& lot : l.lots)
        if (lot.use == Use::Residential && (!park || lot.area > park->area))
            park = &lot;
    REQUIRE(park);
    park->use = Use::Park;
    const MeshParts parts = toMeshes(l, Ground::plane());

    const std::map<std::string, int> streets = facesByMaterial(parts.streets);
    CAPTURE(streets.size());
    CHECK(streets.count("Settlement_Asphalt"));
    CHECK(streets.count("Settlement_Sidewalk"));
    CHECK(parts.streets.materialNames().size() == surfaceMaterials().size());
    int patches = 0;
    for (int32_t s : parts.streets.faceAttrs().get<int32_t>("street"))
        patches += s < 0 ? 1 : 0;
    CHECK(patches > 10);
    CHECK(facesFacingDown(parts.streets) == 0);

    const std::map<std::string, int> ground = facesByMaterial(parts.ground);
    CHECK(ground.count("Settlement_Yard"));
    CHECK(ground.count("Settlement_Grass"));
    CHECK(facesFacingDown(parts.ground) == 0);
    // Large blocks are laid in pieces no wider than the grid cell, so they follow uneven ground.
    CHECK(parts.ground.numFaces() > l.blocks.size());
}

TEST_CASE("settlement ground: ribbons drape on the quad split the heightfield renderer draws")
{
    // A terrain cell whose shorter diagonal is 1-3: the renderer splits it 0-2, and the street must lie on that surface.
    const std::vector<glm::dvec3> pts = {{0.0, 0.0, 0.0}, {0.0, 50.0, 100.0}, {100.0, 20.0, 100.0}, {100.0, 50.0, 0.0}};
    const std::vector<geo::Index> sizes = {4}, corners = {0, 1, 2, 3};
    const geo::Mesh quad = geo::Mesh::fromPolygons(pts, sizes, corners);
    const Ground g = Ground::fromMesh(quad, 10.0);
    CHECK(g.surfaceHeight({50.0, 50.0}) == doctest::Approx(10.0));
    CHECK(g.surfaceHeight({75.0, 25.0}) == doctest::Approx(0.25 * 0.0 + 0.5 * 50.0 + 0.25 * 20.0));
}

TEST_CASE("settlement furniture: trees in parks and behind houses, none with empty park and yard sets")
{
    Layout l = mexicanCity(60000.0, 4);
    for (Lot& lot : l.lots)
        if (lot.area > 600.0 && lot.use == Use::Residential)
        {
            lot.use = Use::Park;
            break;
        }
    const RoadNetwork net = roadNetwork(l, Ground::plane());
    auto count = [&](const std::vector<FurnitureSet>& sets) {
        std::map<std::string, int> out;
        for (const FurniturePlacement& p : placeFurniture(l, net, Ground::plane(), sets))
            ++out[p.kind];
        return out;
    };
    const std::map<std::string, int> all = count(furnitureSets(false));
    CHECK(all.count("parkTree"));
    CHECK(all.count("yardTree"));
    CHECK(all.at("yardTree") < int(l.envelopes.size()));
    const std::map<std::string, int> none = count(applyFurnitureJson(furnitureSets(false), {{"park", json::array()}, {"yard", json::array()}}));
    CHECK_FALSE(none.count("parkTree"));
    CHECK_FALSE(none.count("yardTree"));
}

TEST_CASE("settlement styles: a wealthy house is a mansion only on a large lot")
{
    Layout l;
    l.center = {0.0, 0.0};
    l.radius = 100000.0;
    Envelope env;
    env.footprint = makePolygon({{50000, 50000}, {51000, 50000}, {51000, 51000}, {50000, 51000}}, 0);
    env.use = Use::Residential;
    env.wealth = 2;
    env.floors = 2;
    env.eraName = "modern";
    Lot lot;
    lot.use = Use::Residential;
    lot.area = 150.0;
    finishProgram(env, lot, l);
    CHECK(env.style != "mansion");
    lot.area = 900.0;
    finishProgram(env, lot, l);
    CHECK(env.style == "mansion");
}

TEST_CASE("settlement osm: the data's town point is the centre, its own squares stand in for a generated one")
{
    loadCityPackages();
    run("newScene");
    const json terrain = run("terrainCreate", {{"size", json::array({80000.0, 80000.0})}, {"resolution", 65}, {"amplitude", 200.0}, {"seed", 5}});
    const std::string file = (std::filesystem::path(LILA_SOURCE_DIR) / "tests" / "data" / "osm" / "w1_town.osm").string();
    const json town = run("settlementFromOsm", {{"args", {file}}, {"ground", terrain["mesh"]}, {"origin", {23.2, -106.42}},
                                                {"profile", "mexicanCity"}, {"name", "Pueblo"}, {"overrides", {{"maxFloors", 3}}}});
    CHECK(town["centreFrom"] == "the data's city or town point");
    const json places = json::parse(run("getAttr", {{"args", {town["import"]["curves"]["place"].get<std::string>() + ".osmPlaces"}}}).get<std::string>());
    json at;
    for (const json& p : places)
        if (p.value("kind", std::string()) == "town")
            at = p["at"];
    REQUIRE(at.is_array());
    const json info = run("settlementInfo", {{"args", {town["mesh"]}}});
    const double x = at[0].get<double>(), z = at[at.size() >= 3 ? 2 : 1].get<double>();
    CHECK(std::hypot(info["center"][0].get<double>() - x, info["center"][1].get<double>() - z) < 2000.0);
    CHECK(info["resolvedProfile"]["maxFloors"] == 3);
    CHECK_FALSE(info["lotUses"].contains("plaza"));
}

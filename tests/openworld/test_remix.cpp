// World Remix core: identity at scale 1, rigid regions, fold-free warp, heights and features through one map, road
// reconnection, river drainage, and remixCheck catching each failure. Synthetic fixtures only.

#include "Remix.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>

using namespace openworld;
using json = nlohmann::json;

namespace
{
std::string data(const char* f) { return std::string(LILA_TEST_OPENWORLD_DATA) + "/" + f; }

json coast() { return {{"id", "coast"}, {"dem", data("coast.asc")}, {"osm", data("coast.json")}}; }
json inland(Vec2 at) { return {{"id", "inland"}, {"dem", data("inland.asc")}, {"osm", data("inland.json")}, {"at", {at.x, at.y}}}; }

json base(double h = 1.0 / 3.0, double v = 1.0 / 1.5)
{
    return {{"scale", {{"horizontal", h}, {"vertical", v}}}, {"sources", json::array({coast()})}, {"seams", {{"erosion", 0}, {"detail", 0.0}}}};
}

json box(double x0, double z0, double x1, double z1) { return json::array({{x0, z0}, {x1, z0}, {x1, z1}, {x0, z1}}); }

const OsmDoc::Way* wayNamed(const OsmDoc& d, std::string_view name)
{
    for (const OsmDoc::Way& w : d.ways)
        if (tagOf(w.tags, "name") == name)
            return &w;
    return nullptr;
}

int countKind(const json& check, const char* kind) { return check["counts"][kind].get<int>(); }
} // namespace

TEST_CASE("remix spec: JSON round trip, scales and errors")
{
    json j = base();
    j["regions"] = json::array({{{"id", "a"}, {"op", "cut"}, {"polygon", box(0, 0, 100, 100)}, {"translate", {50, 0}}, {"angle", 30}}});
    j["pins"] = json::array({{{"id", "p"}, {"at", {1, 2}}}});
    const RemixSpec s = specFromJson(j);
    const RemixSpec t = specFromJson(toJson(s));
    CHECK(toJson(s) == toJson(t));
    CHECK(t.pins[0].to == Vec2(1, 2));
    CHECK(parseScale("1:3", "x") == doctest::Approx(1.0 / 3.0));
    CHECK(parseScale(0.5, "x") == 0.5);
    CHECK_THROWS(parseScale("abc", "x"));
    json bad = base();
    bad["nonsense"] = 1;
    CHECK_THROWS(specFromJson(bad));
    CHECK(remixSpecSchema()["$id"] == "lila://schema/remix-spec");
    const RemixSpec d = specFromJson(json::object());
    CHECK(d.horizontal == doctest::Approx(1.0 / 3.0));
    CHECK(d.vertical == doctest::Approx(1.0 / 1.5));
}

TEST_CASE("OSM doc: reads Overpass JSON and XML whole, writes them back")
{
    const OsmDoc d = readOsmDoc(data("coast.json"));
    CHECK(d.ways.size() == 5);
    size_t tagged = 0;
    for (const OsmDoc::Node& n : d.nodes)
        tagged += !n.tags.empty();
    CHECK(tagged == 1);
    const OsmDoc back = parseOsmDocJson(writeOsmDocJson(d));
    CHECK(back.nodes.size() == d.nodes.size());
    CHECK(back.ways.size() == d.ways.size());
    CHECK(wayKind(*wayNamed(back, "Ridge Road")) == WayKind::Road);
    CHECK(wayKind(*wayNamed(back, "Rio Uno")) == WayKind::River);

    const OsmDoc x = parseOsmDocXml(R"(<osm><node id="1" lat="1" lon="2"><tag k="shop" v="bakery"/></node><node id="2" lat="1.1" lon="2"/>
        <way id="5"><nd ref="1"/><nd ref="2"/><tag k="highway" v="residential"/></way>
        <relation id="9"><member type="way" ref="5" role="outer"/><tag k="type" v="route"/></relation></osm>)");
    REQUIRE(x.nodes.size() == 2);
    CHECK(tagOf(x.nodes[0].tags, "shop") == "bakery");
    CHECK(x.ways[0].nodes.size() == 2);
    CHECK(x.relations[0].members[0].ref == 5);
}

TEST_CASE("remix: identity at scale 1 with no regions")
{
    const RemixSpec s = specFromJson(base(1.0, 1.0));
    const std::shared_ptr<RemixResult> r = runRemix(s);
    CHECK(r->warp->identity());
    CHECK(r->errors.empty());
    const std::shared_ptr<const SourceData> src = loadSource(s.sources[0]);
    const Heightfield& h = src->hf;
    // Away from the feathered edge the heights are the source's.
    int checked = 0;
    for (int z = h.nz / 6; z < h.nz - h.nz / 6; z += 5)
        for (int x = h.nx / 6; x < h.nx - h.nx / 6; x += 5)
        {
            const Vec2 p = h.samplePos(x, z);
            CHECK(r->after.heightAt(p.x, p.y) == doctest::Approx(h.at(x, z)).epsilon(1e-4));
            ++checked;
        }
    CHECK(checked > 100);
    // Every original node comes back at its own latitude and longitude.
    const OsmDoc& in = src->osm;
    size_t same = 0;
    for (const OsmDoc::Node& n : in.nodes)
        if (const OsmDoc::Node* o = r->osm.node(n.id))
        {
            CHECK(o->at.lat == doctest::Approx(n.at.lat).epsilon(1e-9));
            CHECK(o->at.lon == doctest::Approx(n.at.lon).epsilon(1e-9));
            ++same;
        }
    CHECK(same == in.nodes.size());
    CHECK(r->roadEnds.empty());
    const json c = checkRemix(s, *r);
    CHECK(c["ok"].get<bool>());
}

TEST_CASE("remix: move and keep regions stay rigid, the warp never folds")
{
    json j = base();
    j["regions"] = json::array({{{"id", "city"}, {"op", "move"}, {"polygon", box(20000, -20000, 50000, 10000)}, {"translate", {-30000, 15000}}, {"angle", 20}},
                                {{"id", "port"}, {"op", "keep"}, {"polygon", box(-30000, -40000, -10000, -20000)}}});
    j["pins"] = json::array({{{"id", "p1"}, {"at", {80000, 40000}}, {"to", {70000, 30000}}}, {{"id", "hold"}, {"at", {-35000, 40000}}}});
    const RemixSpec s = specFromJson(j);
    const std::shared_ptr<RemixResult> r = runRemix(s);
    const Warp& w = *r->warp;
    CHECK_FALSE(w.identity());
    const Rigid city = s.regions[0].rigid();
    std::vector<Vec2> inside;
    for (double x = 21000; x < 50000; x += 3700)
        for (double z = -19000; z < 10000; z += 3300)
            inside.push_back({x, z});
    for (const Vec2& p : inside)
        CHECK(glm::length(w.forward(p) - city.apply(p)) < 0.01);
    for (size_t i = 0; i + 1 < inside.size(); ++i)
        CHECK(glm::length(w.forward(inside[i]) - w.forward(inside[i + 1])) == doctest::Approx(glm::length(inside[i] - inside[i + 1])).epsilon(1e-7));
    for (double x = -29000; x < -10000; x += 4000)
        CHECK(glm::length(w.forward({x, -30000}) - Vec2(x, -30000)) < 0.01);
    CHECK(glm::length(w.forward({80000, 40000}) - Vec2(70000, 30000)) < 1.0);
    // The inverse undoes the forward.
    for (const Vec2& p : {Vec2(0, 0), Vec2(60000, -30000), Vec2(-20000, 30000)})
        CHECK(glm::length(w.inverse(w.forward(p)) - p) < 0.5);
    const Warp::Folds f = w.folds(r->lo, r->hi, 96);
    CHECK(f.folded == 0);
    CHECK(f.minJacobian > 0.0);
    CHECK(w.minStepJacobian() > 0.3);
    CHECK(checkRemix(s, *r)["counts"]["fold"] == 0);
}

TEST_CASE("remix: a squeeze between a moved region and a held one stays fold-free")
{
    json j = base();
    j["regions"] = json::array({{{"id", "a"}, {"op", "move"}, {"polygon", box(30000, -10000, 50000, 10000)}, {"translate", {-55000, 0}}},
                                {{"id", "b"}, {"op", "keep"}, {"polygon", box(-40000, -10000, -30000, 10000)}}});
    const RemixSpec s = specFromJson(j);
    const std::shared_ptr<RemixResult> r = runRemix(s);
    const Warp::Folds f = r->warp->folds(r->lo, r->hi, 128);
    CHECK(f.folded == 0);
    CHECK(r->warp->steps() >= 4);
}

TEST_CASE("remix: heights and features go through the same map (a road on a ridge stays on it)")
{
    json j = base();
    j["regions"] = json::array({{{"id", "east"}, {"op", "move"}, {"polygon", box(45000, -30000, 100000, 30000)}, {"translate", {-20000, 25000}}}});
    j["pins"] = json::array({{{"id", "p"}, {"at", {0, 60000}}, {"to", {5000, 50000}}}});
    const RemixSpec s = specFromJson(j);
    const std::shared_ptr<RemixResult> r = runRemix(s);
    const OsmDoc::Way* road = wayNamed(r->osm, "Ridge Road");
    REQUIRE(road);
    const Heightfield& h = r->after;
    int onRidge = 0, total = 0;
    for (int64_t id : road->nodes)
    {
        const OsmDoc::Node* n = r->osm.node(id);
        const Vec2 p = r->geo.toLocal(n->at);
        // The ridge is strong only inland; near the coast it fades to nothing.
        if (p.x < 0.0)
            continue;
        const double d = 4.0 * h.cell;
        const double here = h.heightAt(p.x, p.y);
        ++total;
        if (here >= h.heightAt(p.x, p.y + d) && here >= h.heightAt(p.x, p.y - d))
            ++onRidge;
    }
    CHECK(total > 15);
    CHECK(onRidge >= total * 9 / 10);
    // Land cover masks went through the same map: the town stays under the cafe.
    for (const OsmDoc::Node& n : r->osm.nodes)
        if (tagOf(n.tags, "amenity") == "cafe")
        {
            const Vec2 p = r->geo.toLocal(n.at);
            CHECK(h.maskAt("landcover_urban", p.x, p.y) > 0.5);
        }
}

TEST_CASE("remix: cut roads are reconnected by the terrain router, keeping their class")
{
    json j = base();
    j["regions"] = json::array({{{"id", "mid"}, {"op", "cut"}, {"polygon", box(25000, -12000, 60000, 12000)}, {"translate", {10000, 40000}}}});
    j["seams"]["width"] = 1200;
    const RemixSpec s = specFromJson(j);
    const std::shared_ptr<RemixResult> r = runRemix(s);
    REQUIRE(r->roadEnds.size() >= 4);
    for (const CutEnd& e : r->roadEnds)
        CHECK(e.joined);
    int reconnects = 0;
    for (const OsmDoc::Way& w : r->osm.ways)
        if (tagOf(w.tags, "remix") == "reconnect")
        {
            ++reconnects;
            const std::string_view cls = tagOf(w.tags, "highway");
            CHECK((cls == "primary" || cls == "secondary"));
            // Joined at the cut ends: the reconnection shares its end nodes with the cut roads.
            int shared = 0;
            for (const OsmDoc::Way& o : r->osm.ways)
                if (&o != &w && tagOf(o.tags, "remix").empty())
                    for (int64_t id : o.nodes)
                        shared += id == w.nodes.front() || id == w.nodes.back();
            CHECK(shared >= 2);
        }
    CHECK(reconnects >= 2);
    CHECK(r->stats["holeSamples"].get<size_t>() > 0);
    const json c = checkRemix(s, *r);
    CHECK(countKind(c, "danglingRoad") == 0);

    // With reconnection off, the check catches every dangling end.
    j["seams"]["roads"] = false;
    const RemixSpec off = specFromJson(j);
    const json c2 = checkRemix(off, *runRemix(off));
    CHECK(countKind(c2, "danglingRoad") >= 4);
    CHECK_FALSE(c2["ok"].get<bool>());
}

TEST_CASE("remix: rivers are re-solved to drain past a pasted dam; the check catches it when off")
{
    json j = base();
    // A copy of the ridge dropped across the river valley.
    j["regions"] = json::array({{{"id", "dam"}, {"op", "duplicate"}, {"polygon", box(28000, -9000, 44000, 9000)}, {"translate", {0, -55400}}}});
    j["seams"]["width"] = 800;
    j["seams"]["roads"] = false;
    j["seams"]["rivers"] = false;
    const RemixSpec off = specFromJson(j);
    const std::shared_ptr<RemixResult> broken = runRemix(off);
    const json c = checkRemix(off, *broken);
    CHECK(countKind(c, "riverNoDrain") >= 1);

    j["seams"]["rivers"] = true;
    const RemixSpec on = specFromJson(j);
    const std::shared_ptr<RemixResult> r = runRemix(on);
    REQUIRE_FALSE(r->rivers.empty());
    double extended = 0.0;
    for (const RiverState& s : r->rivers)
    {
        if (s.drainedBefore)
            CHECK(s.drains);
        extended += s.extended;
    }
    CHECK(extended > 0.0);
    CHECK(countKind(checkRemix(on, *r), "riverNoDrain") == 0);
}

TEST_CASE("remix: the check catches overlapping regions, folds and unreadable sources")
{
    json j = base();
    j["regions"] = json::array({{{"id", "a"}, {"op", "cut"}, {"polygon", box(0, 0, 10000, 10000)}, {"translate", {30000, 0}}},
                                {{"id", "b"}, {"op", "cut"}, {"polygon", box(0, 20000, 10000, 30000)}, {"translate", {32000, -18000}}}});
    RemixSpec s = specFromJson(j);
    CHECK(countKind(checkRemix(s, *runRemix(s)), "overlap") == 1);

    json f = base();
    f["pins"] = json::array({{{"id", "a"}, {"at", {0, 0}}, {"to", {30000, 0}}}, {{"id", "b"}, {"at", {30000, 0}}, {"to", {0, 0}}}});
    f["warp"] = {{"falloff", 40000}, {"maxSteps", 2}};
    s = specFromJson(f);
    const std::shared_ptr<RemixResult> folded = runRemix(s);
    CHECK(countKind(checkRemix(s, *folded), "fold") > 0);

    json m = base();
    m["sources"].push_back({{"id", "gone"}, {"dem", data("missing.asc")}, {"at", {500000, 0}}});
    s = specFromJson(m);
    const std::shared_ptr<RemixResult> partial = runRemix(s);
    CHECK(partial->errors.size() == 1);
    CHECK(countKind(checkRemix(s, *partial), "source") == 1);
}

TEST_CASE("remix: two sources composed onto one map with a region moved between them")
{
    json j = base();
    j["sources"].push_back(inland({250000, -60000}));
    // The inland town lifted out and placed beside the coast.
    j["regions"] = json::array({{{"id", "town"}, {"op", "cut"}, {"polygon", box(230000, -80000, 270000, -40000)}, {"translate", {-140000, 60000}}}});
    const RemixSpec s = specFromJson(j);
    const std::shared_ptr<RemixResult> r = runRemix(s);
    CHECK(r->errors.empty());
    const OsmDoc::Way* hw = wayNamed(r->osm, "Highway");
    REQUIRE(hw);
    bool townhall = false;
    for (const OsmDoc::Node& n : r->osm.nodes)
        if (tagOf(n.tags, "amenity") == "townhall")
        {
            townhall = true;
            const Vec2 p = r->geo.toLocal(n.at);
            CHECK(glm::length(p - Vec2(250000 - 140000, -60000 + 60000)) < 5000.0);
        }
    CHECK(townhall);
    for (const CutEnd& e : r->roadEnds)
        CHECK(e.joined);
    CHECK(r->after.nx > 100);
}

TEST_CASE("remix: cut coastline is re-merged along sea level")
{
    json j = base();
    // A stretch of coast lifted and slid north along the shore.
    j["regions"] = json::array({{{"id", "bay"}, {"op", "cut"}, {"polygon", box(-60000, 20000, -25000, 50000)}, {"translate", {0, -60000}}}});
    j["seams"]["width"] = 1000;
    const RemixSpec s = specFromJson(j);
    const std::shared_ptr<RemixResult> r = runRemix(s);
    REQUIRE(r->coastEnds.size() >= 2);
    size_t joined = 0;
    for (const CutEnd& e : r->coastEnds)
        joined += e.joined;
    CHECK(joined >= 2);
    int merged = 0;
    for (const OsmDoc::Way& w : r->osm.ways)
        if (tagOf(w.tags, "remix") == "coast")
        {
            ++merged;
            CHECK(tagOf(w.tags, "natural") == "coastline");
            // The merged stretch runs along sea level.
            for (int64_t id : w.nodes)
            {
                const Vec2 p = r->geo.toLocal(r->osm.node(id)->at);
                CHECK(std::abs(r->after.heightAt(p.x, p.y)) < 800.0);
            }
        }
    CHECK(merged >= 1);
    const json c = checkRemix(s, *r);
    CHECK(countKind(c, "coastGap") == int(r->coastEnds.size() - joined));
}

TEST_CASE("remix: a tall region pasted onto low coast gets a seam wide enough to keep its slope")
{
    // A plateau 40 m up, as a source of its own, pasted onto the low coast.
    const std::filesystem::path dem = std::filesystem::temp_directory_path() / "lila_remix_plateau.asc";
    {
        std::ofstream f(dem);
        f << "ncols 81\nnrows 81\nxllcorner -107.400250\nyllcorner 24.999750\ncellsize 0.0005\nNODATA_value -9999\n";
        for (int r = 0; r < 81; ++r)
        {
            for (int c = 0; c < 81; ++c)
                f << "40 ";
            f << "\n";
        }
    }
    // Steepest ground in the seam around where the region lands.
    auto steepest = [](const RemixResult& r) {
        const auto& h = r.after;
        const auto it = h.masks.find("remixSeam");
        REQUIRE(it != h.masks.end());
        double worst = 0.0;
        for (int z = 1; z + 1 < h.nz; ++z)
            for (int x = 1; x + 1 < h.nx; ++x)
                if (const Vec2 p = h.samplePos(x, z); it->second[h.index(x, z)] > 0.0f && p.x > 55000 && p.x < 165000 && p.y > -55000 && p.y < 55000)
                {
                    const double gx = (h.heights[h.index(x + 1, z)] - h.heights[h.index(x - 1, z)]) / (2.0 * h.cell);
                    const double gz = (h.heights[h.index(x, z + 1)] - h.heights[h.index(x, z - 1)]) / (2.0 * h.cell);
                    worst = std::max(worst, std::hypot(gx, gz));
                }
        return worst;
    };
    json j = base();
    j["sources"].push_back({{"id", "plateau"}, {"dem", dem.generic_string()}, {"at", {250000, -60000}}});
    j["regions"] = json::array({{{"id", "city"}, {"op", "cut"}, {"polygon", box(200000, -110000, 300000, -10000)}, {"translate", {-140000, 60000}}}});
    j["seams"]["maxSlope"] = 0.0;
    const std::shared_ptr<RemixResult> fixed = runRemix(specFromJson(j));
    j["seams"]["maxSlope"] = 0.35;
    const std::shared_ptr<RemixResult> wide = runRemix(specFromJson(j));
    CHECK(fixed->errors.empty());
    CHECK(wide->errors.empty());
    const double before = steepest(*fixed), after = steepest(*wide);
    CAPTURE(before);
    CAPTURE(after);
    // The band is capped at a quarter of the region, so a big step still leaves a slope, but a gentler one.
    CHECK(before > 0.6);
    CHECK(after < before * 0.5);
    bool noted = false;
    for (const std::string& n : wide->notes)
        noted = noted || n.find("region city: seam widened") != std::string::npos;
    CHECK(noted);
    // A region with a seam of its own keeps it.
    j["regions"][0]["seam"] = 1500;
    const std::shared_ptr<RemixResult> own = runRemix(specFromJson(j));
    CHECK(steepest(*own) > 0.6);
    std::filesystem::remove(dem);
}

TEST_CASE("remix: a road crossing a delete strip narrower than the probe step is still cut")
{
    json j = base();
    // 4 m wide, far under half the 30 m seam the probe steps by.
    j["regions"] = json::array({{{"id", "strip"}, {"op", "delete"}, {"polygon", box(39800, -200000, 40200, 200000)}}});
    j["seams"]["width"] = 3000;
    j["seams"]["roads"] = false;
    const RemixSpec s = specFromJson(j);
    const std::shared_ptr<RemixResult> r = runRemix(s);
    CHECK(r->roadEnds.size() >= 2);
    CHECK(countKind(checkRemix(s, *r), "danglingRoad") >= 2);
}

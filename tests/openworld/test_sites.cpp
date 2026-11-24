// Site kits: every kind lays out deterministically with typed slots and styled envelopes, settings are checked, and the
// commands build each kind on a synthetic coast terrain, keep slot ids across edits, publish slots, grade the terrain and
// undo, while siteCheck catches overlaps, water, steep ground and real names.

#include "Sites.h"

#include "extensions/ExtensionManager.h"
#include "names/Names.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <set>

using namespace lila;
using json = nlohmann::json;
namespace s = openworld::sites;

namespace
{
s::Spec specOf(const std::string& kind, json params = json::object())
{
    return s::specFromJson({{"kind", kind}, {"id", kind + "1"}, {"params", params}});
}

std::vector<std::string> slotIds(const s::Layout& L)
{
    std::vector<std::string> ids;
    for (const s::SlotOut& sl : L.slots)
        ids.push_back(sl.path);
    return ids;
}

void loadSitePackages()
{
    if (!ops::find("terrainCreate"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
    if (!ops::find("siteCreate"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_OPENWORLD_PACKAGE) == "OpenWorld");
}

int issuesOf(const json& check, const std::string& kind, const std::string& site = {})
{
    int n = 0;
    for (const json& i : check["issues"])
        n += i["kind"] == kind && (site.empty() || i["site"] == site);
    return n;
}

/** A coast: land 3 m up for z > -60000, sea floor 15 m down past -61000, sea level 0; 10 m cells. */
std::string coastTerrain()
{
    const json t = ops::execute("terrainCreate", {{"name", "Coast"}, {"flat", true}, {"size", {500000, 500000}}, {"resolution", 501}, {"center", {0, 300, 0}}});
    const std::string mesh = t["mesh"].get<std::string>();
    ops::execute("terrainStampRegion", {{"args", {mesh}},
                                        {"id", "sea"},
                                        {"footprint", {{-260000, -260000}, {260000, -260000}, {260000, -61500}, {-260000, -61500}}},
                                        {"mode", "carve"},
                                        {"height", -1500.0},
                                        {"falloff", 0.0},
                                        {"layer", "Base"}});
    ops::execute("terrainSeaLevel", {{"args", {mesh}}, {"level", 0.0}, {"preview", false}});
    return mesh;
}
} // namespace

TEST_CASE("site kits: every kind lays out the same way twice, typed slots, styled envelopes, fictional names")
{
    const std::vector<std::string>& styles = s::styles();
    for (const s::Kind& k : s::kinds())
    {
        CAPTURE(k.id);
        const s::Layout a = s::build(specOf(k.id)), b = s::build(specOf(k.id));
        CHECK(!a.slots.empty());
        CHECK(!a.envelopes.empty());
        CHECK(!a.ground.empty());
        CHECK(slotIds(a) == slotIds(b));
        const std::vector<std::string> ids = slotIds(a);
        CHECK(std::set<std::string>(ids.begin(), ids.end()).size() == ids.size());
        for (const s::SlotOut& sl : a.slots)
        {
            CHECK(!sl.type.empty());
            CHECK(sl.path.find("//") == std::string::npos);
        }
        std::set<std::string> envIds;
        for (const s::EnvelopeOut& e : a.envelopes)
        {
            CAPTURE(e.path);
            CHECK(std::ranges::find(styles, e.style) != styles.end());
            CHECK(!e.program.empty());
            CHECK(e.footprint.size() >= 3);
            CHECK(e.height > 0.0);
            CHECK(envIds.insert(e.path).second);
        }
        // No real brand among the names a kit hands out.
        std::vector<names::Text> texts;
        for (const auto& [role, n] : a.names.items())
            texts.push_back({role, n.get<std::string>()});
        CHECK(names::findRealNames(texts, names::RealNames(), true).empty());
        CHECK(!a.names.value("site", std::string()).empty());
    }

    // Kind-specific parts are there.
    const s::Layout port = s::build(specOf("port"));
    int containers = 0, sts = 0, rtg = 0, reefers = 0;
    std::set<std::string> liveryTypes;
    for (const s::SlotOut& sl : port.slots)
    {
        containers += sl.type.starts_with("container.");
        sts += sl.type == "crane.sts";
        rtg += sl.type == "crane.rtg";
        reefers += sl.type.starts_with("container.reefer.");
        if (sl.type.starts_with("container.40ft."))
            liveryTypes.insert(sl.type);
    }
    CHECK(containers == port.stats["containers"].get<int>());
    CHECK(containers > 200);
    CHECK(sts == 6);
    CHECK(rtg > 0);
    CHECK(reefers > 0);
    CHECK(port.liveries.size() == 6);
    CHECK(liveryTypes.size() > 1);
    const s::Layout cem = s::build(specOf("cemetery"));
    CHECK(cem.stats["mausoleums"].get<int>() > 0);
    CHECK(cem.stats["graves"].get<int>() > 0);
    const s::Layout farm = s::build(specOf("farm"));
    CHECK(farm.stats["cropRows"].get<int>() > 0);
    bool canal = false;
    for (const s::GroundOp& g : farm.ground)
        canal = canal || (g.command == "terrainAddFeature" && g.flags.value("kind", std::string()) == "canal");
    CHECK(canal);
    const s::Layout air = s::build(specOf("airstrip"));
    CHECK(std::ranges::any_of(air.ground, [](const s::GroundOp& g) { return g.command == "terrainAddAirstrip"; }));
    CHECK(std::ranges::any_of(air.slots, [](const s::SlotOut& sl) { return sl.type == "windsock"; }));
}

TEST_CASE("site kits: an unrelated edit keeps every other part's id; settings are checked")
{
    const std::vector<std::string> a = slotIds(s::build(specOf("port", {{"warehouses", 2}})));
    const std::vector<std::string> b = slotIds(s::build(specOf("port", {{"warehouses", 3}})));
    CHECK(a == b);
    const std::vector<std::string> c = slotIds(s::build(specOf("farm", {{"truckYard", false}})));
    std::vector<std::string> d = slotIds(s::build(specOf("farm")));
    std::erase_if(d, [](const std::string& id) { return id.starts_with("truckYard/"); });
    CHECK(c == d);
    // A different seed moves things; the same seed never does.
    CHECK(slotIds(s::build(s::specFromJson({{"kind", "cemetery"}, {"id", "c"}, {"seed", 1}}))) !=
          slotIds(s::build(s::specFromJson({{"kind", "cemetery"}, {"id", "c"}, {"seed", 2}}))));

    CHECK_THROWS(specOf("port", {{"runwayLength", 50000}}));
    CHECK_THROWS(specOf("port", {{"tiers", 40}}));
    CHECK_THROWS(specOf("airstrip", {{"aircraft", "jumbo"}}));
    CHECK_THROWS(s::specFromJson({{"kind", "castle"}}));
    CHECK_THROWS(s::specFromJson({{"kind", "port"}, {"nonsense", 1}}));
    const s::Spec back = s::specFromJson(specOf("village").toJson());
    CHECK(back.toJson() == specOf("village").toJson());
}

TEST_CASE("site commands: every kind on a coast terrain, slots published, envelopes, ids stable, check, undo")
{
    loadSitePackages();
    ops::execute("newScene", json::object());
    const std::string terrain = coastTerrain();

    // The port on the shore line, the water at -z.
    const json port = ops::execute("siteCreate", {{"kind", "port"}, {"terrain", terrain}, {"at", {0, -61000}}, {"id", "port1"}});
    CHECK(port["id"] == "port1");
    CHECK(port["waterLevelFrom"] == "terrain sea level");
    CHECK(port["ground"]["errors"].empty());
    CHECK(port["ground"]["sent"].get<int>() > 3);
    CHECK(port["envelopes"].get<int>() >= 3);
    const std::string portName = port["names"]["site"].get<std::string>();
    CHECK(!portName.empty());

    // Slots are published under the site's owner id with its region.
    const json found = ops::execute("slotFind", {{"type", "container"}, {"region", "site:port1"}, {"limit", 0}});
    const json portInfo = ops::execute("siteInfo", {{"args", {"port1"}}, {"slots", true}});
    REQUIRE(found.is_object());
    CHECK(found["count"].get<int>() == portInfo["stats"]["containers"].get<int>());
    std::vector<std::string> before;
    for (const json& sl : portInfo["slotList"])
        before.push_back(sl["id"].get<std::string>());
    CHECK(before.front().starts_with("site:port1/"));

    // An unrelated edit keeps the ids; the terrain is graded again.
    const json edited = ops::execute("siteEdit", {{"args", {"port1"}}, {"warehouses", 3}});
    CHECK(edited["ground"]["errors"].empty());
    std::vector<std::string> after;
    const json afterInfo = ops::execute("siteInfo", {{"args", {"port1"}}, {"slots", true}});
    for (const json& sl : afterInfo["slotList"])
        after.push_back(sl["id"].get<std::string>());
    CHECK(after == before);

    // Envelopes carry style tags and stable ids.
    const json envs = ops::execute("siteEnvelopes", {{"sites", {"port1"}}});
    CHECK(envs["count"].get<int>() >= 4);
    CHECK(envs["styles"].contains("portShed"));
    CHECK(envs["styles"].contains("civic"));
    for (const json& e : envs["envelopes"])
    {
        CHECK(e["id"].get<std::string>().starts_with("site:port1/"));
        CHECK(e["top"].get<double>() > e["base"].get<double>());
    }

    // Every other kind on the land, each well apart.
    const std::vector<std::pair<std::string, json>> others = {{"cruise", {120000, -61000}},  {"lighthouse", {-120000, -55000}}, {"cemetery", {-120000, 40000}},
                                                              {"airstrip", {0, 100000}},     {"farm", {130000, 100000}},       {"village", {-120000, 140000}},
                                                              {"compound", {60000, 30000}}, {"island", {0, -170000}}};
    for (const auto& [kind, at] : others)
    {
        CAPTURE(kind);
        json call = {{"kind", kind}, {"terrain", terrain}, {"at", at}, {"id", kind + "1"}};
        if (kind == "island")
            call["raiseIsland"] = true;
        const json r = ops::execute("siteCreate", call);
        CHECK(r["slots"].get<int>() > 0);
        CHECK(r["ground"]["errors"].empty());
    }
    const json list = ops::execute("siteList", json::object());
    CHECK(list.size() == 9);

    // The check: the layout above is clean but for nothing standing in water.
    const json clean = ops::execute("siteCheck", json::object());
    CAPTURE(clean.dump(1).substr(0, 4000));
    CHECK(issuesOf(clean, "realName") == 0);
    CHECK(issuesOf(clean, "overlap") == 0);
    CHECK(issuesOf(clean, "water", "port1") == 0);
    CHECK(issuesOf(clean, "shallow", "port1") == 0);
    CHECK(issuesOf(clean, "water", "cemetery1") == 0);
    CHECK(issuesOf(clean, "water", "island1") == 0);
    CHECK(issuesOf(clean, "groundStale") == 0);

    // A cemetery out at sea, a compound on top of the farm, a farm on steep hills.
    ops::execute("siteCreate", {{"kind", "cemetery"}, {"terrain", terrain}, {"at", {-150000, -150000}}, {"id", "wet"}});
    ops::execute("siteCreate", {{"kind", "compound"}, {"terrain", terrain}, {"at", {130000, 100000}}, {"id", "clash"}});
    const json hills = ops::execute("terrainCreate", {{"name", "Hills"}, {"size", {120000, 120000}}, {"resolution", 129}, {"center", {600000, 0, 0}},
                                                      {"amplitude", 30000.0}, {"featureSize", 15000.0}, {"seed", 3}});
    ops::execute("siteCreate", {{"kind", "farm"}, {"terrain", hills["mesh"]}, {"at", {600000, 0}}, {"id", "steep"}, {"ground", false}});
    const json bad = ops::execute("siteCheck", json::object());
    CHECK(issuesOf(bad, "water", "wet") > 0);
    CHECK(issuesOf(bad, "overlap", "clash") > 0);
    CHECK(issuesOf(bad, "steepGround", "steep") > 0);
    CHECK(issuesOf(bad, "groundStale", "steep") > 0);

    // A real brand as a name is refused; generated names never are.
    CHECK_THROWS(ops::execute("siteCreate", {{"kind", "farm"}, {"name", "Starbucks"}, {"terrain", ""}}));

    // Undo takes a site and its grading away again.
    auto siteStamps = [&] {
        int n = 0;
        const json stamps = ops::execute("terrainStamps", {{"args", {terrain}}});
        for (const auto& [layer, items] : stamps["layers"].items())
            for (const json& it : items)
                n += it.dump().find("site:lastone/") != std::string::npos;
        return n;
    };
    ops::execute("siteCreate", {{"kind", "compound"}, {"terrain", terrain}, {"at", {-40000, 200000}}, {"id", "lastone"}});
    CHECK(siteStamps() > 0);
    ops::execute("undo", json::object());
    CHECK(siteStamps() == 0);
    CHECK(ops::execute("siteList", json::object()).size() == 12);

    // Removing a site takes its stamps and roads off the terrain.
    ops::execute("siteRemove", {{"args", {"clash"}}});
    CHECK(ops::execute("siteList", json::object()).size() == 11);
}

TEST_CASE("site kits: villages find every house a spot along their streets")
{
    for (const auto& [id, houses, radius] : {std::tuple{"habal", 10, 12000.0}, std::tuple{"noria", 12, 14000.0}, std::tuple{"quelite", 12, 14000.0},
                                             std::tuple{"v4", 16, 9000.0}, std::tuple{"v5", 20, 14000.0}})
        for (int seed : {0, 1, 2})
        {
            CAPTURE(id);
            CAPTURE(seed);
            const s::Layout L = s::build(s::specFromJson({{"kind", "village"}, {"id", id}, {"seed", seed}, {"params", {{"houses", houses}, {"villageRadius", radius}}}}));
            CHECK_MESSAGE(L.notes.empty(), (L.notes.empty() ? std::string() : L.notes[0]));
            CHECK(L.stats["houses"].get<int>() == houses);
        }
}

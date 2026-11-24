// worldBuild's Unreal export against a stand-in LilaLink that keeps a level's placement groups: the first build sends
// everything, a rebuild with a changed slot rule sends only what changed and imports nothing, and a rebuild without one
// of the sites removes that site's groups and their slot table rows, leaving no group the build did not send.

#include "World.h"

#include "../link/FakeServer.h"

#include "core/PrefStore.h"
#include "extensions/ExtensionManager.h"
#include "link/Exporters.h"
#include "link/UnrealLink.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
json run(const char* cmd, json call = json::object()) { return lila::ops::execute(cmd, call); }

void loadPackages()
{
    if (!lila::ops::find("terrainCreate"))
        REQUIRE(lila::extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
    if (!lila::ops::find("settlementCreate"))
        REQUIRE(lila::extensions::ExtensionManager::get().load(LILA_TEST_SETTLEMENT_PACKAGE) == "Settlement");
    if (!lila::ops::find("buildingCreate"))
        REQUIRE(lila::extensions::ExtensionManager::get().load(LILA_TEST_BUILDINGS_PACKAGE) == "Buildings");
    if (!lila::ops::find("worldBuild"))
        REQUIRE(lila::extensions::ExtensionManager::get().load(LILA_TEST_OPENWORLD_PACKAGE) == "OpenWorld");
}

/** A level as LilaLink keeps it: placement groups with their signature and actor count, the landscape's hash, imports. */
struct FakeLevel
{
    std::mutex m;
    std::map<std::string, json> groups;
    std::map<std::string, std::string> slotRows; // slot id -> group
    std::map<std::string, std::string> slotTypes; // slot id -> type
    std::string landscapeHash;
    int imports = 0;

    std::optional<json> handle(const json& req)
    {
        std::scoped_lock lock(m);
        const std::string method = req["method"];
        const json p = req.value("params", json::object());
        if (method == "hello")
            return FakeServer::reply(req, {{"project", "TestGame"},
                                           {"features", {"progress", "placements", "placementHashes", "levelInfo", "landscape", "zones", "buildings",
                                                         "buildingInteriors", "slots"}}});
        if (method == "levelInfo")
            return FakeServer::reply(req, {{"level", "/Game/Test/World"}, {"partitioned", false}});
        if (method == "setLandscape")
        {
            const bool same = !landscapeHash.empty() && p.value("hash", std::string()) == landscapeHash;
            landscapeHash = p.value("hash", std::string());
            return FakeServer::reply(req, {{"actor", p.value("label", std::string())}, {"created", false}, {"unchanged", same}, {"layers", json::array()}});
        }
        if (method == "importAsset")
        {
            ++imports;
            return FakeServer::reply(req, {{"assets", {p["contentPath"].get<std::string>() + ".Imported"}}, {"warnings", json::array()}, {"errors", json::array()}});
        }
        if (method == "placeInstances")
        {
            const bool holder = (p["mode"] == "ism" || p["mode"] == "hism") && !p.contains("actorClass");
            groups[p["group"].get<std::string>()] = {{"hash", p.value("hash", std::string())}, {"actors", holder ? size_t(1) : p["placements"].size()}};
            return FakeServer::reply(req, {{"spawned", p["placements"].size()}, {"updated", 0}, {"removed", 0}});
        }
        if (method == "placementHashes")
            return FakeServer::reply(req, {{"groups", json(groups)}});
        if (method == "removePlacements")
        {
            std::set<std::string> keep;
            for (const json& k : p.value("keep", json::array()))
                keep.insert(k.get<std::string>());
            int removed = 0;
            for (auto it = groups.begin(); it != groups.end();)
            {
                bool gone = false;
                for (const json& k : p.value("groups", json::array()))
                    gone = gone || k == it->first;
                for (const json& pre : p.value("prefixes", json::array()))
                    gone = gone || (it->first.starts_with(pre.get<std::string>()) && !keep.contains(it->first));
                if (gone)
                {
                    ++removed;
                    it = groups.erase(it);
                }
                else
                    ++it;
            }
            return FakeServer::reply(req, {{"removed", removed}});
        }
        if (method == "writeSlotTable")
        {
            for (const json& src : p.value("sources", json::array()))
                std::erase_if(slotRows, [&](const auto& r) { return r.second.starts_with(src.get<std::string>()); });
            for (const json& row : p.value("rows", json::array()))
            {
                slotRows[row.value("id", std::string())] = row.value("group", std::string());
                slotTypes[row.value("id", std::string())] = row.value("type", std::string());
            }
            return FakeServer::reply(req, {{"rows", slotRows.size()}});
        }
        if (method == "setZones")
            return FakeServer::reply(req, {{"volumes", 1}, {"created", 0}, {"updated", 0}, {"unchanged", 1}, {"removed", 0}});
        return FakeServer::reply(req, json::object());
    }
};

/** Buildings modifiers in the scene: a rebuild must not leave the replaced build's behind (they keep publishing slots). */
size_t buildingModifiers()
{
    lila::scene::Graph& g = lila::scene::Graph::current();
    size_t n = 0;
    for (lila::scene::NodeId id : g.nodes())
        if (const lila::scene::Node* node = g.node(id); node && node->name().starts_with("LilaBuildingsModifier"))
            ++n;
    return n;
}

json stepResult(const json& build, const std::string& name)
{
    for (const json& s : build["steps"])
        if (s["step"] == name)
            return s;
    return json();
}

std::string failedSteps(const json& r)
{
    std::string out;
    for (const json& s : r["steps"])
        if (!s.value("ok", true))
            out += s["step"].get<std::string>() + ": " + s.value("error", std::string()) + "\n";
    return out;
}

json spec(bool withCemetery, const std::string& ruleType)
{
    json s = json::parse(R"({
        "name": "LinkTiny",
        "culture": "mexican",
        "terrain": {"size": [200000, 200000], "resolution": 65, "seed": 5, "source": {"type": "noise", "amplitude": 1200, "featureSize": 90000}},
        "sites": [{"kind": "village", "id": "v1", "at": [40000, 40000], "params": {"houses": 4, "villageRadius": 9000}},
                  {"kind": "cemetery", "id": "c1", "at": [-50000, -40000], "params": {"cemeteryWidth": 9000, "cemeteryDepth": 7000}}],
        "slots": {"rules": []},
        "zones": {"spec": {"layers": [{"id": "police", "type": "police", "zones": [{"id": "all", "name": "Comandancia Norte",
                    "polygons": [[[-100000, -100000], [100000, -100000], [100000, 100000], [-100000, 100000]]]}]}]}},
        "check": false,
        "export": {"target": "unreal", "map": false}
    })");
    if (!withCemetery)
        s["sites"].erase(1);
    if (!ruleType.empty())
        s["slots"]["rules"].push_back({{"id", "swap"}, {"name", "Swap"}, {"type", ruleType}, {"payload", {{"unrealMesh", "/Engine/BasicShapes/Cube.Cube"}}}});
    return s;
}
} // namespace

TEST_CASE("world: Unreal export sends a rebuild's changes only and removes what the world no longer makes")
{
    loadPackages();
    if (!lila::ops::find("unrealPlaceInstances"))
        lila::link::registerCommands();
    lila::link::registerExporter("fbx:instanceSource", [](const std::vector<lila::scene::NodeId>&, const fs::path& path, std::string*) {
        std::ofstream(path) << "fbx";
        return true;
    });
    run("newScene");
    const fs::path cache = fs::temp_directory_path() / "lila_world_unreal";
    fs::remove_all(cache);
    lila::PrefStore::get().set("unreal", "/cacheDir", cache.generic_string());
    FakeLevel level;
    FakeServer server([&](const json& req) { return level.handle(req); });
    lila::link::Link::ConnectOptions o;
    o.port = server.port();
    lila::link::Link::get().connect(o);

    // First build: every group goes, all keyed by the world's link id prefix.
    const json r1 = run("worldBuild", {{"spec", spec(true, "")}, {"folder", (cache / "w1").generic_string()}});
    INFO(failedSteps(r1));
    REQUIRE(r1.value("failed", json()).is_null());
    const std::string prefix = r1["exports"]["unreal"]["prefix"];
    CHECK(prefix.size() == 9);
    for (const char* step : {"export.unreal.link", "export.unreal.terrain", "export.unreal.sites", "export.unreal.buildings", "export.unreal.zones", "export.unreal.stale"})
        CHECK_MESSAGE(stepResult(r1, step).value("ok", false), step);
    const int imports1 = level.imports;
    CHECK(imports1 > 0);
    size_t first;
    {
        std::scoped_lock lock(level.m);
        first = level.groups.size();
        for (const auto& [k, v] : level.groups)
            CHECK_MESSAGE(k.starts_with(prefix), k);
    }
    CHECK(first > 5);
    const size_t modifiers = buildingModifiers();
    CHECK(modifiers > 0);
    CHECK(stepResult(r1, "export.unreal.stale")["result"]["stale"] == 0);
    // Not a World Partition level: the step says why the proxies stayed home.
    CHECK(stepResult(r1, "export.unreal.buildings")["result"].contains("proxiesSkipped"));

    // Rebuilt with a slot rule swapping the commonest slot type for a project mesh: nothing imported, the landscape left
    // alone, that type's groups sent, every other group held unchanged.
    std::string common;
    {
        std::scoped_lock lock(level.m);
        std::map<std::string, int> byType;
        for (const auto& [id, type] : level.slotTypes)
            ++byType[type];
        int most = 0;
        for (const auto& [type, n] : byType)
            if (n > most)
                most = n, common = type;
    }
    REQUIRE_FALSE(common.empty());
    const json r2 = run("worldBuild", {{"spec", spec(true, common)}, {"replace", true}, {"folder", (cache / "w2").generic_string()}});
    INFO(failedSteps(r2));
    REQUIRE(r2.value("failed", json()).is_null());
    CHECK(level.imports == imports1);
    CHECK(buildingModifiers() == modifiers);
    CHECK(stepResult(r2, "export.unreal.terrain")["result"]["unchanged"] == true);
    const json sites2 = stepResult(r2, "export.unreal.sites")["result"];
    const json shells2 = stepResult(r2, "export.unreal.buildings")["result"]["shellsAndModules"];
    INFO(sites2.dump() << " " << shells2.dump());
    CHECK(sites2["unchanged"].get<int>() + shells2["unchanged"].get<int>() > 0);
    CHECK(sites2["sent"].get<int>() + shells2["sent"].get<int>() > 0);
    CHECK(sites2["imported"] == 0);
    CHECK(shells2["imported"] == 0);
    {
        std::scoped_lock lock(level.m);
        for (const auto& [k, v] : level.groups)
            CHECK_MESSAGE(k.starts_with(prefix), k);
    }

    // Rebuilt without the cemetery: its groups and slot rows leave the level; nothing else is imported again.
    size_t rowsBefore, groupsBefore;
    {
        std::scoped_lock lock(level.m);
        rowsBefore = level.slotRows.size();
        groupsBefore = level.groups.size();
    }
    const json r3 = run("worldBuild", {{"spec", spec(false, common)}, {"replace", true}, {"folder", (cache / "w3").generic_string()}});
    INFO(failedSteps(r3));
    REQUIRE(r3.value("failed", json()).is_null());
    const json stale = stepResult(r3, "export.unreal.stale")["result"];
    CHECK(stale["stale"].get<int>() > 0);
    CHECK(stale.contains("slotTable"));
    CHECK(level.imports == imports1);
    {
        std::scoped_lock lock(level.m);
        CHECK(level.slotRows.size() < rowsBefore);
        CHECK(level.groups.size() < groupsBefore);
        CHECK(level.groups.size() <= stale["sentThisBuild"].get<size_t>());
        for (const auto& [k, v] : level.groups)
            CHECK_MESSAGE(k.starts_with(prefix), k);
    }

    lila::link::unregisterExporter("fbx:instanceSource");
    lila::link::Link::get().disconnect();
    fs::remove_all(cache);
}

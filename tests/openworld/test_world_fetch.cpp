// World fetch: a spec's missing real data planned and sized, consent and key refusals before any download, downloads
// written straight to the spec's own paths through a stand-in network, retries and resume, and the commands
// (worldFetch, worldBuild estimate/fetch, terrainBathymetry from a bbox with no terrain). No network anywhere.

#include "World.h"
#include "WorldFetch.h"

#include "Coast.h"
#include "OsmDoc.h"

#include "extensions/ExtensionManager.h"
#include "io/SceneFile.h"
#include "ops/Command.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace openworld::world;

namespace
{
constexpr const char* kKey = "SECRETKEY12345";

fs::path scratch(const std::string& name)
{
    const fs::path d = fs::temp_directory_path() / ("lila_world_fetch_" + name);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

/** A geographic grid over [west, south] to [east, north], height h everywhere. */
terrain::Dem grid(double west, double south, double east, double north, int n, float h)
{
    terrain::Dem d;
    d.width = d.height = n;
    d.geographic = true;
    d.dx = (east - west) / (n - 1);
    d.dy = (north - south) / (n - 1);
    d.x0 = west;
    d.y0 = north;
    d.epsg = 4326;
    d.metres.assign(size_t(n) * size_t(n), h);
    return d;
}

/** The spec: two sources in one Copernicus tile, the capital's elevation already there, the coast's GEBCO named twice. */
fs::path writeSpec(const fs::path& dir, bool bathymetry = true)
{
    json coast = {{"id", "coast"}, {"bbox", {-106.52, 23.13, -106.50, 23.15}}, {"dem", "real/coast_dem.tif"}, {"osm", "real/coast.osm.pbf"},
                  {"landcover", "real/coast_wc.tif"}, {"at", {0, 0}}};
    if (bathymetry)
        coast["bathymetry"] = "real/gebco.tif";
    json spec = {{"name", "FetchTest"},
                 {"remix", {{"sources", {coast, {{"id", "capital"}, {"bbox", {-106.40, 23.30, -106.38, 23.32}}, {"dem", "real/capital_dem.tif"}, {"at", {100000, 0}}}}}}}};
    if (bathymetry)
        spec["terrain"] = {{"coast", {{"bathymetry", {{"file", "real/gebco.tif"}}}}}};
    fs::create_directories(dir / "real");
    terrain::writeGeoTiff(dir / "real" / "capital_dem.tif", grid(-106.40, 23.30, -106.38, 23.32, 8, 50.0f));
    const fs::path f = dir / "spec.json";
    std::ofstream(f) << spec.dump(1);
    return f;
}

json loadResolved(const fs::path& file)
{
    std::ifstream in(file);
    return resolvePaths(json::parse(in), file.parent_path().generic_string());
}

const FetchItem* item(const FetchPlan& p, const std::string& id)
{
    for (const FetchItem& it : p.items)
        if (it.id == id)
            return &it;
    return nullptr;
}

/** A stand-in network: answers Copernicus, Overpass and OpenTopography with small files and counts every call. */
struct StandIn
{
    int calls = 0;
    int overpassFailures = 0;
    bool landcoverFails = false;
    std::optional<bool> metered = false;
    std::vector<std::string> urls;
    std::vector<int> waits;

    FetchNet net()
    {
        FetchNet n;
        n.get = [this](const std::string& url, const fs::path& dest, std::string* why) {
            ++calls;
            urls.push_back(url);
            if (url.find("copernicus") != std::string::npos)
            {
                terrain::writeGeoTiff(dest, grid(-107.0, 23.0, -106.0, 24.0, 61, 120.0f));
                return 200;
            }
            if (url.find("overpass") != std::string::npos)
            {
                if (overpassFailures-- > 0)
                {
                    *why = "connection refused";
                    return 0;
                }
                std::ofstream(dest) << R"({"version": 0.6, "elements": [{"type": "node", "id": 1, "lat": 23.14, "lon": -106.51, "tags": {"place": "village", "name": "Aldea"}}]})";
                return 200;
            }
            if (url.find("opentopography") != std::string::npos)
            {
                if (url.find(kKey) == std::string::npos)
                {
                    *why = "invalid API key";
                    return 401;
                }
                terrain::writeGeoTiff(dest, grid(-106.6, 23.0, -106.3, 23.4, 40, -800.0f));
                return 200;
            }
            return 404;
        };
        n.landcover = [this](const terrain::GeoBox& b) {
            ++calls;
            if (landcoverFails)
                throw std::runtime_error("WorldCover tile read timed out");
            terrain::LandCoverFetch f;
            f.codes = grid(b.west, b.south, b.east, b.north, 10, 30.0f);
            f.tiles = 1;
            f.bytes = 1234;
            return f;
        };
        n.metered = [this] { return metered; };
        n.wait = [this](int ms) { waits.push_back(ms); };
        return n;
    }
};

std::string message(const std::function<void()>& fn)
{
    try
    {
        fn();
    }
    catch (const std::exception& e)
    {
        return e.what();
    }
    return {};
}

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

json run(const char* cmd, json call = json::object()) { return lila::ops::execute(cmd, call); }
} // namespace

TEST_CASE("world fetch: the plan lists each missing file with its size, skips present ones, and paths are the spec's own")
{
    const fs::path dir = scratch("plan");
    const json spec = loadResolved(writeSpec(dir));
    const FetchPlan p = planFetch(spec, dir / "cache");

    // The coast's GEBCO is named by its source and by the terrain's coast step: one item.
    CHECK(p.items.size() == 5);
    REQUIRE(item(p, "capital.dem"));
    CHECK(item(p, "capital.dem")->present);
    for (const char* id : {"coast.dem", "coast.osm", "coast.landcover", "coast.bathymetry"})
    {
        CAPTURE(id);
        REQUIRE(item(p, id));
        CHECK_FALSE(item(p, id)->present);
        CHECK(item(p, id)->bytes > 0);
        CHECK(item(p, id)->file.parent_path() == (dir / "real").lexically_normal());
    }
    CHECK(item(p, "coast.dem")->bytes == kCopernicusTileBytes);
    // GEBCO is fetched two cells wider than the source.
    CHECK(item(p, "coast.bathymetry")->box.west == doctest::Approx(-106.52 - 2.0 / terrain::kGebcoPerDegree));
    CHECK(p.missing == 4);
    CHECK(p.keyNeeded);

    FetchOptions o;
    const json j = planJson(p, o);
    CHECK(j["consentNeeded"] == true);
    CHECK(j["keyMissing"] == true);
    CHECK(j["megabytes"].get<double>() > 25.0);
    CHECK(j["summary"].get<std::string>().find("coast.dem 25.0 MB") != std::string::npos);
    CHECK(fetchSize(12000) == "12 KB");
    CHECK(fetchSize(5u << 20) == "5.0 MB");

    // A cached Copernicus tile costs nothing.
    fs::create_directories(dir / "cache" / "copernicus");
    terrain::writeGeoTiff(dir / "cache" / "copernicus" / (terrain::copernicusTileNames({-106.52, 23.13, -106.50, 23.15})[0] + ".tif"),
                          grid(-107.0, 23.0, -106.0, 24.0, 61, 120.0f));
    const FetchPlan cached = planFetch(spec, dir / "cache");
    CHECK(item(cached, "coast.dem")->cached);
    CHECK(item(cached, "coast.dem")->bytes == 0);
    CHECK(double(cached.bytes) / 1048576.0 < 25.0);

    // A coast grid of its own without a bbox takes the bathymetry source's box, else all sources' boxes together.
    json coastOnly = {{"remix", spec["remix"]}, {"terrain", {{"coast", {{"bathymetry", {{"file", (dir / "real" / "other.tif").generic_string()}}}}}}}};
    FetchPlan u = planFetch(coastOnly, dir / "cache");
    REQUIRE(item(u, "terrain.coast.bathymetry"));
    CHECK(item(u, "terrain.coast.bathymetry")->boxFrom == "bathymetry source");
    CHECK(item(u, "terrain.coast.bathymetry")->box.north < 23.2);
    coastOnly["remix"]["sources"][0].erase("bathymetry");
    u = planFetch(coastOnly, dir / "cache");
    CHECK(item(u, "terrain.coast.bathymetry")->boxFrom == "sources");
    CHECK(item(u, "terrain.coast.bathymetry")->box.north > 23.32);
    fs::remove_all(dir);
}

TEST_CASE("world fetch: over 25 MB without consent, or metered, is refused before any download")
{
    const fs::path dir = scratch("consent");
    const FetchPlan p = planFetch(loadResolved(writeSpec(dir, false)), dir / "cache");
    StandIn s;
    FetchOptions o;
    o.cache = dir / "cache";
    const std::string big = message([&] { runFetch(p, o, s.net()); });
    CHECK(big.find("MB to download") != std::string::npos);
    CHECK(big.find("consent=True") != std::string::npos);
    CHECK(big.find("coast.dem 25.0 MB") != std::string::npos);
    CHECK(s.calls == 0);

    // Under the limit, a metered connection still needs consent.
    fs::create_directories(dir / "cache" / "copernicus");
    terrain::writeGeoTiff(dir / "cache" / "copernicus" / (terrain::copernicusTileNames({-106.52, 23.13, -106.50, 23.15})[0] + ".tif"),
                          grid(-107.0, 23.0, -106.0, 24.0, 61, 120.0f));
    const FetchPlan small = planFetch(loadResolved(dir / "spec.json"), dir / "cache");
    s.metered = true;
    const std::string metered = message([&] { runFetch(small, o, s.net()); });
    CHECK(metered.find("metered") != std::string::npos);
    CHECK(s.calls == 0);
    fs::remove_all(dir);
}

TEST_CASE("world fetch: a missing OpenTopography key says how to pass and save one, and the key never shows")
{
    const fs::path dir = scratch("key");
    const FetchPlan p = planFetch(loadResolved(writeSpec(dir)), dir / "cache");
    StandIn s;
    FetchOptions o;
    o.cache = dir / "cache";
    o.consent = true;
    const std::string missing = message([&] { runFetch(p, o, s.net()); });
    CHECK(missing.find("coast.bathymetry") != std::string::npos);
    CHECK(missing.find("apiKey=") != std::string::npos);
    CHECK(missing.find("saveKey=True") != std::string::npos);
    CHECK(missing.find("opentopography.org") != std::string::npos);
    CHECK(s.calls == 0);

    // A wrong key fails at the server; the message names the item and hides the key.
    o.apiKey = "WRONGKEY99999";
    const std::string wrong = message([&] { runFetch(p, o, s.net()); });
    CHECK(wrong.find("coast.bathymetry") != std::string::npos);
    CHECK(wrong.find("401") != std::string::npos);
    CHECK(wrong.find("WRONGKEY99999") == std::string::npos);
    CHECK(wrong.find("fetched before it and kept: coast.dem, coast.osm, coast.landcover") != std::string::npos);

    // Commands never echo it either.
    CHECK(lila::ops::toPython("worldFetch", {{"file", "w.json"}, {"apiKey", kKey}}).find(kKey) == std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("world fetch: downloads go straight to the spec's paths, retry, resume after a failure, and carry the credits")
{
    const fs::path dir = scratch("run");
    const fs::path specFile = writeSpec(dir);
    StandIn s;
    FetchOptions o;
    o.cache = dir / "cache";
    o.consent = true;
    o.apiKey = kKey;

    // Land cover fails every attempt: the run stops there, naming it; what came before stays.
    s.landcoverFails = true;
    const std::string failed = message([&] { runFetch(planFetch(loadResolved(specFile), o.cache), o, s.net()); });
    CHECK(failed.find("coast.landcover") != std::string::npos);
    CHECK(failed.find("timed out") != std::string::npos);
    CHECK(failed.find("after 3 attempts") != std::string::npos);
    CHECK(failed.find("run again to resume") != std::string::npos);
    CHECK(fs::exists(dir / "real" / "coast_dem.tif"));
    CHECK(fs::exists(dir / "real" / "coast.osm.pbf"));
    CHECK_FALSE(fs::exists(dir / "real" / "coast_wc.tif"));
    CHECK_FALSE(fs::exists(dir / "real" / "coast_wc.tif.part"));

    // Run again: present files are not fetched again; Overpass is unreachable once and retried.
    s.landcoverFails = false;
    s.calls = 0;
    s.urls.clear();
    s.waits.clear();
    fs::remove(dir / "real" / "coast.osm.pbf");
    fs::remove_all(dir / "cache" / "overpass");
    s.overpassFailures = 1;
    const FetchPlan p = planFetch(loadResolved(specFile), o.cache);
    CHECK(item(p, "coast.dem")->present);
    const json r = runFetch(p, o, s.net());
    CHECK(r["fetched"] == 3);
    CHECK(r["present"] == 2);
    for (const std::string& u : s.urls)
        CHECK(u.find("copernicus") == std::string::npos);
    std::map<std::string, json> byId;
    for (const json& it : r["items"])
        byId[it["id"].get<std::string>()] = it;
    CHECK(byId["coast.osm"]["state"] == "fetched");
    CHECK(byId["coast.osm"]["attempts"] == 2);
    CHECK(s.waits.size() == 1);
    CHECK(byId["coast.dem"]["state"] == "present");
    CHECK(byId["capital.dem"]["state"] == "present");
    CHECK(byId["coast.bathymetry"]["state"] == "fetched");
    CHECK(r.dump().find(kKey) == std::string::npos);

    // Written where the spec says, and readable: Overpass JSON under the .pbf name reads by content.
    const openworld::OsmDoc osm = openworld::readOsmDoc(dir / "real" / "coast.osm.pbf");
    CHECK(osm.nodes.size() == 1);
    const terrain::Dem g = terrain::readDem(dir / "real" / "gebco.tif");
    CHECK(g.geographic);
    CHECK(g.metres[0] == doctest::Approx(-800.0f));
    CHECK(terrain::readDem(dir / "real" / "coast_wc.tif").width == 10);
    std::string text;
    {
        std::ifstream credits(dir / "real" / "ATTRIBUTION.txt");
        text.assign(std::istreambuf_iterator<char>(credits), {});
    }
    for (const char* who : {"OpenStreetMap", "GEBCO", "WorldCover", "Copernicus"})
        CHECK(text.find(who) != std::string::npos);

    // Everything there: nothing more to do and nothing downloaded.
    s.calls = 0;
    const FetchPlan done = planFetch(loadResolved(specFile), o.cache);
    CHECK(done.missing == 0);
    CHECK(runFetch(done, o, s.net())["fetched"] == 0);
    CHECK(s.calls == 0);
    fs::remove_all(dir);
}

TEST_CASE("world fetch: commands estimate and refuse without a terrain or the network; GEBCO from a bbox alone")
{
    loadPackages();
    run("newScene");
    const fs::path dir = scratch("commands");
    const fs::path withBathy = writeSpec(dir);
    const fs::path small = writeSpec(dir / "nobathy", false);
    const std::string file = withBathy.generic_string();

    // Estimate only: sizes per item and total, nothing built, no terrain needed.
    const json e = run("worldBuild", {{"file", file}, {"estimate", true}})["estimate"];
    CHECK(e["missing"] == 4);
    CHECK(e["consentNeeded"] == true);
    CHECK(e["keyMissing"] == true);
    CHECK(run("worldFetch", {{"file", file}, {"estimate", true}})["estimate"]["summary"] == e["summary"]);
    for (const json& it : e["items"])
        if (it["state"] == "missing")
            CHECK(fs::path(it["file"].get<std::string>()).parent_path() == (dir / "real").lexically_normal());

    // Missing data without fetch stops before the build, naming the file and the way out.
    const std::string noFetch = message([&] { run("worldBuild", {{"file", file}}); });
    CHECK(noFetch.find("coast.dem") != std::string::npos);
    CHECK(noFetch.find("fetch=True") != std::string::npos);
    // GEBCO without a key, then over 25 MB without consent: refused before any download.
    CHECK(message([&] { run("worldBuild", {{"file", file}, {"fetch", true}, {"consent", true}}); }).find("saveKey=True") != std::string::npos);
    const std::string big = message([&] { run("worldBuild", {{"file", small.generic_string()}, {"fetch", true}}); });
    CHECK(big.find("consent=True") != std::string::npos);
    CHECK(big.find("MB") != std::string::npos);
    // Offline with nothing cached names the item.
    CHECK(message([&] { run("worldFetch", {{"file", small.generic_string()}, {"offline", true}}); }).find("offline") != std::string::npos);

    // terrainBathymetry from a bbox with no terrain in the scene.
    const json box = {-106.53, 23.12, -106.49, 23.16};
    const json est = run("terrainBathymetry", {{"bbox", box}, {"estimate", true}});
    CHECK(est["estimate"]["megabytes"].get<double>() > 0.0);
    CHECK(est["consentNeeded"] == false);
    CHECK(message([&] { run("terrainBathymetry", {{"bbox", box}, {"fetch", true}}); }).find("saveKey=True") != std::string::npos);
    // A grid fetched before (in the download cache) writes to file= offline, without a terrain or a key.
    terrain::FetchRequest r;
    r.source = terrain::DemSource::OpenTopography;
    r.box = {-106.53, 23.12, -106.49, 23.16};
    r.dataset = "GEBCOIceTopo";
    r.cache = fs::temp_directory_path() / "lila_terrain" / "dem_cache";
    const fs::path cacheFile = terrain::openTopographyCacheFile(r);
    fs::create_directories(cacheFile.parent_path());
    terrain::writeGeoTiff(cacheFile, grid(-106.6, 23.0, -106.3, 23.4, 40, -500.0f));
    const fs::path out = dir / "real" / "from_bbox.tif";
    const json f = run("terrainBathymetry", {{"bbox", box}, {"fetch", true}, {"offline", true}, {"file", out.generic_string()}});
    CHECK(f["applied"] == false);
    CHECK(fs::exists(out));
    CHECK(terrain::readDem(out).metres[0] == doctest::Approx(-500.0f));
    std::error_code ec;
    fs::remove(cacheFile, ec);
    fs::remove_all(dir, ec);
}

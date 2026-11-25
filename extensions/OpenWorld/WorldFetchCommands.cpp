// worldFetch and the fetch half of worldBuild: a world spec's missing real data sized, allowed and downloaded over the
// network (Terrain's HTTP and WorldCover readers), into the paths the spec names.

#include "Http.h"
#include "WorldFetch.h"
#include "WorldScene.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Log.h>
#include <lila/Paths.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>
#include <lila/SceneFile.h>
#include <lila/Workspace.h>

#include <QNetworkInformation>

#include <chrono>
#include <format>
#include <fstream>
#include <thread>

using namespace lila;

namespace openworld::world
{
namespace fs = std::filesystem;
using T = ArgType;

namespace
{
/** Terrain keeps the OpenTopography key here; the same key serves terrainImportDem, terrainBathymetry and worldFetch. */
constexpr const char* kPrefsFile = "terrain";
constexpr const char* kKeyPref = "/openTopography/apiKey";

/** The download cache Terrain's fetches use: the workspace's terrain/dem_cache, else beside the scene, else temp. */
fs::path downloadCache()
{
    if (const fs::path& root = Workspace::get().root(); !root.empty())
        return root / "terrain" / "dem_cache";
    const fs::path& sceneFile = io::currentPath();
    if (!sceneFile.empty())
        return sceneFile.parent_path() / (sceneFile.stem().string() + "_terrain") / "dem_cache";
    return fs::temp_directory_path() / "lila_terrain" / "dem_cache";
}

/** True on a metered connection; nullopt when Qt has no network information backend here. */
std::optional<bool> meteredConnection()
{
    if (!QNetworkInformation::instance() && !QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Metered))
        return std::nullopt;
    QNetworkInformation* n = QNetworkInformation::instance();
    if (!n || !n->supports(QNetworkInformation::Feature::Metered))
        return std::nullopt;
    return n->isMetered();
}

FetchNet networkNet()
{
    FetchNet net;
    net.get = [](const std::string& url, const fs::path& dest, std::string* why) { return terrain::httpGet(url, dest, why); };
    net.landcover = [](const terrain::GeoBox& b) {
        return terrain::fetchWorldCover(b, [](const std::string& url, uint64_t at, uint64_t n, std::vector<uint8_t>* out, std::string* why) {
            return terrain::httpGetRange(url, at, n, out, why);
        });
    };
    net.metered = meteredConnection;
    net.wait = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    return net;
}

fs::path resolveRelative(const std::string& s) { return paths::resolvePath(s); }
} // namespace

json loadSpecFile(const std::string& file, const char* cmd)
{
    const fs::path p = paths::resolvePath(file);
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw CommandError(std::string(cmd) + ": can't read " + paths::toUtf8(p));
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    json j = json::parse(text, nullptr, false, true);
    if (j.is_discarded())
        throw CommandError(std::string(cmd) + ": " + paths::toUtf8(p) + " is not JSON");
    return resolvePaths(std::move(j), paths::toUtf8(p.parent_path()), [](const std::string& rel) {
        std::error_code ec;
        return fs::exists(paths::resolvePath(rel), ec);
    });
}

json fetchWorldData(const json& spec, const FetchFlags& flags, const char* cmd)
{
    LILA_ZONE();
    FetchOptions o;
    o.cache = downloadCache();
    o.offline = flags.offline;
    o.consent = flags.consent;
    o.apiKey = flags.apiKey;
    if (!o.apiKey.empty() && flags.saveKey)
        PrefStore::get().set(kPrefsFile, kKeyPref, o.apiKey);
    if (o.apiKey.empty())
        o.apiKey = PrefStore::get().value<std::string>(kPrefsFile, kKeyPref, std::string());
    const FetchPlan plan = planFetch(spec, o.cache, resolveRelative);
    json est = planJson(plan, o);
    if (flags.estimate)
        return {{"estimate", est}};
    if (plan.missing > 0)
        log::info("{}: {}", cmd, planSummary(plan));
    json report;
    try
    {
        report = runFetch(plan, o, networkNet());
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
    for (const json& it : report["items"])
        if (it["state"] == "fetched")
            log::info("{}: fetched {} ({:.1f} MB) to {}", cmd, it["id"].get<std::string>(), it.value("bytes", uint64_t(0)) / 1048576.0, it["file"].get<std::string>());
    report["estimate"] = est;
    return report;
}

std::string missingData(const json& spec)
{
    FetchOptions o;
    o.cache = downloadCache();
    const FetchPlan plan = planFetch(spec, o.cache, resolveRelative);
    if (plan.missing == 0)
        return {};
    std::string files;
    for (const FetchItem& it : plan.items)
        if (!it.present)
            files += (files.empty() ? "" : ", ") + it.id + " (" + paths::toUtf8(it.file) + ")";
    return std::format("missing data: {}; fetch=True downloads them ({}), estimate=True only reports the sizes", files, planSummary(plan));
}

void registerFetchCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "worldFetch",
        .doc = "Download a world spec's missing real data straight to the paths it names (relative to the spec file): every remix "
               "source's elevation (Copernicus), OSM (Overpass), land cover (WorldCover) and bathymetry (GEBCO through "
               "OpenTopography, with your API key), and the coast's bathymetry grid (its bbox, the bathymetry source's, or all "
               "sources' together). Files already there are left alone. estimate=True only lists each file with its size; a "
               "total over 25 MB, or any download on a metered connection, needs consent=True. Downloads retry and are written "
               "whole, so a run stopped by a failure resumes where it stopped; tiles and areas are kept in the workspace's "
               "terrain/dem_cache (offline=True uses only that). Returns every item fetched or present, bytes and the "
               "ATTRIBUTION.txt files written. worldBuild(fetch=True) runs this first.",
        .flags = {{"spec", "s", T::Any, nullptr, "The world spec, a dict."},
                  {"file", "f", T::String, nullptr, "A world spec JSON file; relative paths in it are from its folder."},
                  {"estimate", "es", T::Bool, false, "Only list what is missing and how big the downloads are."},
                  {"consent", "cn", T::Bool, false, "Allow more than 25 MB, or downloads on a metered connection."},
                  {"offline", "off", T::Bool, false, "Use the download cache only."},
                  {"apiKey", "ak", T::String, "", "OpenTopography API key (for GEBCO); default the saved one. Never logged."},
                  {"saveKey", "sk", T::Bool, false, "Save apiKey for later fetches."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            json spec;
            if (a.has("file"))
                spec = loadSpecFile(a.get<std::string>("file"), "worldFetch");
            else if (a.has("spec"))
                spec = a.flags["spec"].is_string() ? json::parse(a.get<std::string>("spec")) : a.flags["spec"];
            else
                throw CommandError("worldFetch: give spec= or file=");
            FetchFlags f;
            f.estimate = a.get<bool>("estimate", false);
            f.consent = a.get<bool>("consent", false);
            f.offline = a.get<bool>("offline", false);
            f.apiKey = a.get<std::string>("apiKey", "");
            f.saveKey = a.get<bool>("saveKey", false);
            return fetchWorldData(spec, f, "worldFetch");
        },
        .examples = {"e = worldFetch(file='worlds/sinaloa_real.json', estimate=True)['estimate']\ne['summary'], e['consentNeeded']",
                     "worldFetch(file='worlds/sinaloa_real.json', consent=True, apiKey='...', saveKey=True)"},
    });
}
} // namespace openworld::world

// Real data for a world spec in one call: the files a spec names, their download sizes, and the downloads themselves
// (Copernicus elevation, Overpass OSM, WorldCover land cover, GEBCO bathymetry) written straight to those paths.

#include "WorldFetch.h"

#include "Coast.h"
#include "Osm.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <set>
#include <stdexcept>

namespace openworld::world
{
namespace fs = std::filesystem;
using terrain::GeoBox;

namespace
{
/** Overpass gives no size before it answers; a city's JSON runs about this much per square kilometre. */
constexpr double kOsmBytesPerKm2 = 0.1 * 1048576.0;
/** WorldCover is 10 m (1/12000 degree) class codes, deflated in the cloud-optimised tiles to about this per sample. */
constexpr double kWorldCoverPerDegree = 12000.0;
constexpr double kWorldCoverBytesPerSample = 0.3;
constexpr const char* kGebcoDataset = "GEBCOIceTopo";

std::optional<GeoBox> boxOf(const json& b)
{
    if (!b.is_array() || b.size() != 4 || !std::all_of(b.begin(), b.end(), [](const json& x) { return x.is_number(); }))
        return std::nullopt;
    GeoBox g{b[0].get<double>(), b[1].get<double>(), b[2].get<double>(), b[3].get<double>()};
    if (!g.valid())
        return std::nullopt;
    return g;
}

/** GEBCO is fetched two of its cells wider than the place, so the blur at the edge has data (as terrainBathymetry does). */
GeoBox gebcoBox(GeoBox b)
{
    const double m = 2.0 / terrain::kGebcoPerDegree;
    return {std::max(-180.0, b.west - m), std::max(-90.0, b.south - m), std::min(180.0, b.east + m), std::min(90.0, b.north + m)};
}

std::string boxKey(const GeoBox& b) { return std::format("{:.5f}_{:.5f}_{:.5f}_{:.5f}", b.west, b.south, b.east, b.north); }

fs::path areaCacheFile(const fs::path& cache, const std::string& kind, const GeoBox& b)
{
    return kind == "osm" ? cache / "overpass" / (boxKey(b) + ".json") : cache / "worldcover" / (boxKey(b) + ".tif");
}

terrain::FetchRequest gebcoRequest(const GeoBox& b, const fs::path& cache)
{
    terrain::FetchRequest r;
    r.source = terrain::DemSource::OpenTopography;
    r.box = b;
    r.dataset = kGebcoDataset;
    r.cache = cache;
    return r;
}

bool nonEmpty(const fs::path& p)
{
    std::error_code ec;
    return fs::exists(p, ec) && fs::file_size(p, ec) > 0 && !ec;
}

std::string mb(uint64_t bytes) { return fetchSize(bytes); }

std::string utf8(const fs::path& p)
{
    const std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

/** Sizes the download of one missing item and whether the cache already holds it. */
void size(FetchItem& it, const fs::path& cache)
{
    const GeoBox& b = it.box;
    if (it.kind == "dem")
    {
        const std::vector<std::string> tiles = terrain::copernicusTileNames(b);
        if (tiles.size() > 64)
        {
            it.problem = std::format("{} Copernicus tiles (each 1 degree); take a smaller bbox", tiles.size());
            return;
        }
        int missing = 0;
        for (const std::string& t : tiles)
        {
            const fs::path f = cache / "copernicus" / (t + ".tif");
            fs::path none = f;
            none += ".none";
            if (!nonEmpty(f) && !fs::exists(none))
                ++missing;
        }
        it.cached = missing == 0;
        it.bytes = uint64_t(missing) * kCopernicusTileBytes;
        it.sizeFrom = std::format("{} of {} Copernicus tiles to download, about 25 MB each", missing, tiles.size());
    }
    else if (it.kind == "osm" || it.kind == "landcover")
    {
        it.cached = nonEmpty(areaCacheFile(cache, it.kind, b));
        if (it.kind == "osm")
        {
            it.bytes = uint64_t(b.widthMetres() * b.heightMetres() / 1e6 * kOsmBytesPerKm2);
            it.sizeFrom = "a guess from the area (Overpass gives no size first)";
        }
        else
        {
            const double samples = (b.east - b.west) * kWorldCoverPerDegree * (b.north - b.south) * kWorldCoverPerDegree;
            it.bytes = uint64_t(samples * kWorldCoverBytesPerSample);
            it.sizeFrom = "WorldCover 10 m samples over the area, compressed";
        }
        if (it.cached)
            it.bytes = 0;
    }
    else
    {
        it.cached = nonEmpty(terrain::openTopographyCacheFile(gebcoRequest(b, cache)));
        it.bytes = it.cached ? 0 : terrain::gebcoEstimateBytes(b.west, b.south, b.east, b.north);
        it.sizeFrom = "GEBCO 15 arc-second samples, 4 bytes each";
    }
    if (it.cached)
        it.sizeFrom = "in the download cache";
}

/** Writes a file whole: to file.part, then renamed over file, so a stopped run never leaves a file that looks present. */
void writeWhole(const fs::path& file, const std::function<void(const fs::path&)>& write)
{
    if (file.has_parent_path())
        fs::create_directories(file.parent_path());
    fs::path part = file;
    part += ".part";
    write(part);
    fs::rename(part, file);
}

bool transient(int status) { return status == 0 || status == 429 || status >= 500; }

/** msg without the key anywhere in it. */
std::string scrub(std::string msg, const std::string& key)
{
    if (key.size() >= 4)
        for (size_t at = msg.find(key); at != std::string::npos; at = msg.find(key, at))
            msg.replace(at, key.size(), "***");
    return msg;
}
} // namespace

std::string fetchSize(uint64_t bytes)
{
    if (bytes < 102400)
        return std::format("{} KB", (bytes + 1023) / 1024);
    return std::format("{:.1f} MB", double(bytes) / 1048576.0);
}

FetchPlan planFetch(const json& spec, const fs::path& cache, const std::function<fs::path(const std::string&)>& resolve)
{
    FetchPlan p;
    auto pathOf = [&](const std::string& s) {
        fs::path f(std::u8string(s.begin(), s.end()));
        return f.is_relative() && resolve ? resolve(s) : f;
    };
    std::vector<GeoBox> sourceBoxes;
    std::optional<GeoBox> bathymetrySource;
    std::set<fs::path> seen;
    auto add = [&](FetchItem it) {
        const fs::path key = it.file.lexically_normal();
        if (!seen.insert(key).second)
            return;
        it.present = nonEmpty(it.file);
        if (!it.present && it.problem.empty())
            size(it, cache);
        if (!it.present)
        {
            ++p.missing;
            p.bytes += it.bytes;
            if (it.kind == "bathymetry" && !it.cached && it.problem.empty())
                p.keyNeeded = true;
        }
        p.items.push_back(std::move(it));
    };

    const json remix = spec.value("remix", json::object());
    const json sources = remix.is_object() ? remix.value("sources", json::array()) : json::array();
    for (const json& src : sources)
    {
        if (!src.is_object())
            continue;
        const std::string id = src.value("id", std::string("source"));
        const std::optional<GeoBox> b = boxOf(src.value("bbox", json()));
        if (b)
            sourceBoxes.push_back(*b);
        for (const char* kind : {"dem", "osm", "landcover", "bathymetry"})
        {
            if (!src.contains(kind) || !src[kind].is_string() || src[kind].get<std::string>().empty())
                continue;
            FetchItem it;
            it.id = id + "." + kind;
            it.kind = kind;
            it.file = pathOf(src[kind].get<std::string>());
            it.boxFrom = "source";
            if (b)
                it.box = it.kind == "bathymetry" ? gebcoBox(*b) : *b;
            else
                it.problem = "source '" + id + "' has no bbox [west, south, east, north] to fetch it for";
            if (it.kind == "bathymetry" && b && !bathymetrySource)
                bathymetrySource = *b;
            add(std::move(it));
        }
    }

    const json terrainSpec = spec.value("terrain", json::object());
    const json coast = terrainSpec.is_object() ? terrainSpec.value("coast", json::object()) : json::object();
    const json bathy = coast.is_object() ? coast.value("bathymetry", json()) : json();
    if (bathy.is_object() && bathy.contains("file") && bathy["file"].is_string() && !bathy["file"].get<std::string>().empty())
    {
        FetchItem it;
        it.id = "terrain.coast.bathymetry";
        it.kind = "bathymetry";
        it.file = pathOf(bathy["file"].get<std::string>());
        if (const std::optional<GeoBox> own = boxOf(bathy.value("bbox", json())))
        {
            it.box = gebcoBox(*own);
            it.boxFrom = "coast";
        }
        else if (bathymetrySource)
        {
            it.box = gebcoBox(*bathymetrySource);
            it.boxFrom = "bathymetry source";
        }
        else if (!sourceBoxes.empty())
        {
            GeoBox u = sourceBoxes[0];
            for (const GeoBox& s : sourceBoxes)
                u = {std::min(u.west, s.west), std::min(u.south, s.south), std::max(u.east, s.east), std::max(u.north, s.north)};
            it.box = gebcoBox(u);
            it.boxFrom = "sources";
        }
        else
            it.problem = "terrain.coast.bathymetry has no bbox and no remix source gives one";
        add(std::move(it));
    }
    return p;
}

json planJson(const FetchPlan& p, const FetchOptions& o)
{
    json items = json::array();
    for (const FetchItem& it : p.items)
    {
        json j = {{"id", it.id}, {"kind", it.kind}, {"file", utf8(it.file)}, {"state", it.present ? "present" : "missing"}};
        if (!it.present)
        {
            j["bytes"] = it.bytes;
            j["megabytes"] = double(it.bytes) / 1048576.0;
            j["cached"] = it.cached;
            j["sizeFrom"] = it.sizeFrom;
            j["bbox"] = {it.box.west, it.box.south, it.box.east, it.box.north};
            j["bboxFrom"] = it.boxFrom;
            if (!it.problem.empty())
                j["problem"] = it.problem;
        }
        items.push_back(std::move(j));
    }
    const double total = double(p.bytes) / 1048576.0;
    return {{"items", items},
            {"missing", p.missing},
            {"bytes", p.bytes},
            {"megabytes", total},
            {"consentNeeded", total > o.consentMB},
            {"consentMB", o.consentMB},
            {"keyNeeded", p.keyNeeded},
            {"keyMissing", p.keyNeeded && o.apiKey.empty()},
            {"summary", planSummary(p)}};
}

std::string planSummary(const FetchPlan& p)
{
    if (p.missing == 0)
        return "all data present";
    std::string out;
    for (const FetchItem& it : p.items)
        if (!it.present)
            out += (out.empty() ? "" : ", ") + it.id + " " + (it.problem.empty() ? (it.cached ? std::string("cached") : mb(it.bytes)) : std::string("no bbox"));
    return out + "; about " + mb(p.bytes);
}

json runFetch(const FetchPlan& p, const FetchOptions& o, const FetchNet& net)
{
    for (const FetchItem& it : p.items)
        if (!it.present && !it.problem.empty())
            throw std::runtime_error(it.id + " (" + utf8(it.file) + "): " + it.problem);
    if (o.offline)
    {
        for (const FetchItem& it : p.items)
            if (!it.present && !it.cached)
                throw std::runtime_error(it.id + " (" + utf8(it.file) + "): offline and the download cache (" + utf8(o.cache) + ") doesn't hold this area");
    }
    else if (p.keyNeeded && o.apiKey.empty())
    {
        for (const FetchItem& it : p.items)
            if (!it.present && it.kind == "bathymetry" && !it.cached)
                throw std::runtime_error(it.id + " (" + utf8(it.file) + "): GEBCO bathymetry downloads through OpenTopography. " + terrain::kOpenTopographyKeyHelp);
    }
    const double total = double(p.bytes) / 1048576.0;
    if (!o.offline && total > o.consentMB && !o.consent)
        throw std::runtime_error(std::format("about {:.1f} MB to download ({}); over {:.0f} MB needs consent=True", total, planSummary(p), o.consentMB));
    std::optional<bool> metered;
    if (net.metered && p.bytes > 0 && !o.offline)
        metered = net.metered();
    if (metered.value_or(false) && !o.consent)
        throw std::runtime_error(std::format("the connection is metered: about {:.1f} MB to download ({}); pass consent=True to download on it", total, planSummary(p)));

    json items = json::array();
    std::vector<std::string> fetchedIds;
    std::map<fs::path, std::map<std::string, std::string>> credits;
    uint64_t written = 0;
    for (const FetchItem& it : p.items)
    {
        json j = {{"id", it.id}, {"kind", it.kind}, {"file", utf8(it.file)}};
        if (it.present)
        {
            j["state"] = "present";
            items.push_back(std::move(j));
            continue;
        }
        const auto start = std::chrono::steady_clock::now();
        int attempts = 0;
        // One download with retries on unreachable, 429 and 5xx; other answers return at once.
        const terrain::Downloader get = [&](const std::string& url, const fs::path& dest, std::string* why) {
            int status = 0;
            for (int n = 1; n <= std::max(1, o.attempts); ++n)
            {
                ++attempts;
                status = net.get ? net.get(url, dest, why) : 0;
                if (!transient(status) || n == o.attempts)
                    break;
                if (net.wait)
                    net.wait(2000 * n);
            }
            return status;
        };
        json notes = json::array();
        try
        {
            if (it.kind == "dem" || it.kind == "bathymetry")
            {
                terrain::FetchRequest r = it.kind == "dem" ? terrain::FetchRequest{} : gebcoRequest(it.box, o.cache);
                r.box = it.box;
                r.cache = o.cache;
                r.offline = o.offline;
                if (it.kind == "bathymetry")
                    r.apiKey = o.apiKey;
                const terrain::FetchResult res = terrain::fetchDem(r, get);
                writeWhole(it.file, [&](const fs::path& part) { terrain::writeGeoTiff(part, res.dem); });
                for (const std::string& n : res.notes)
                    notes.push_back(n);
                j["downloadedTiles"] = res.downloaded;
                j["cachedTiles"] = res.cached;
                credits[it.file.parent_path()][it.kind == "dem" ? "Copernicus elevation" : "GEBCO bathymetry"] =
                    it.kind == "dem" ? res.attribution : std::string(terrain::gebcoAttribution()) + "\n" + res.attribution;
            }
            else
            {
                const fs::path cached = areaCacheFile(o.cache, it.kind, it.box);
                const bool had = nonEmpty(cached);
                if (!had && o.offline)
                    throw std::runtime_error("offline and the download cache doesn't hold this area");
                if (!had && it.kind == "osm")
                    writeWhole(cached, [&](const fs::path& part) {
                        std::string why;
                        const int status = get(terrain::overpassUrl(it.box), part, &why);
                        if (status != 200)
                        {
                            std::error_code ec;
                            fs::remove(part, ec);
                            throw std::runtime_error("the Overpass API answered " + (status ? std::to_string(status) : std::string("nothing")) +
                                                     (why.empty() ? "" : ": " + why));
                        }
                        std::ifstream in(part, std::ios::binary);
                        char c = 0;
                        while (in.get(c) && std::isspace(static_cast<unsigned char>(c)))
                        {
                        }
                        if (c != '{')
                        {
                            in.close();
                            std::error_code ec;
                            fs::remove(part, ec);
                            throw std::runtime_error("the Overpass API sent something other than JSON (busy or the area is too large); try again later");
                        }
                    });
                else if (!had)
                {
                    terrain::LandCoverFetch res;
                    for (int n = 1;; ++n)
                    {
                        ++attempts;
                        try
                        {
                            if (!net.landcover)
                                throw std::runtime_error("no land cover download available");
                            res = net.landcover(it.box);
                            break;
                        }
                        catch (const std::exception&)
                        {
                            if (n >= o.attempts)
                                throw;
                            if (net.wait)
                                net.wait(2000 * n);
                        }
                    }
                    writeWhole(cached, [&](const fs::path& part) { terrain::writeGeoTiff(part, res.codes, true); });
                    j["downloadedBytes"] = res.bytes;
                }
                writeWhole(it.file, [&](const fs::path& part) { fs::copy_file(cached, part, fs::copy_options::overwrite_existing); });
                if (had)
                    notes.push_back("from the download cache");
                if (it.kind == "osm" && it.file.extension() == ".pbf")
                    notes.push_back("Overpass answers in JSON; written under the .pbf name, and Lila reads it by its content");
                credits[it.file.parent_path()][it.kind == "osm" ? "OpenStreetMap" : "ESA WorldCover"] =
                    it.kind == "osm" ? terrain::osmAttribution() : terrain::worldCoverAttribution();
            }
        }
        catch (const std::exception& e)
        {
            std::string msg = it.id + " (" + utf8(it.file) + "): " + scrub(e.what(), o.apiKey);
            if (attempts > 1)
                msg += std::format(" (after {} attempts)", attempts);
            if (!fetchedIds.empty())
            {
                msg += "; fetched before it and kept: ";
                for (size_t i = 0; i < fetchedIds.size(); ++i)
                    msg += (i ? ", " : "") + fetchedIds[i];
                msg += " (run again to resume)";
            }
            throw std::runtime_error(msg);
        }
        std::error_code ec;
        const uint64_t bytes = fs::file_size(it.file, ec);
        written += ec ? 0 : bytes;
        j["state"] = "fetched";
        j["bytes"] = ec ? 0 : bytes;
        j["attempts"] = attempts;
        j["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (!notes.empty())
            j["notes"] = notes;
        fetchedIds.push_back(it.id);
        items.push_back(std::move(j));
    }
    json attributionFiles = json::array();
    for (const auto& [dir, sections] : credits)
        for (const auto& [title, text] : sections)
        {
            const fs::path f = terrain::writeAttribution(dir, title, text);
            if (std::find(attributionFiles.begin(), attributionFiles.end(), utf8(f)) == attributionFiles.end())
                attributionFiles.push_back(utf8(f));
        }
    int fetched = 0, present = 0;
    for (const json& j : items)
        (j["state"] == "fetched" ? fetched : present)++;
    return {{"items", items},
            {"fetched", fetched},
            {"present", present},
            {"bytes", written},
            {"megabytes", double(written) / 1048576.0},
            {"metered", metered ? json(*metered) : json()},
            {"attributionFiles", attributionFiles}};
}
} // namespace openworld::world

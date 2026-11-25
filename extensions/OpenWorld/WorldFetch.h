#pragma once

#include "Fetch.h"
#include "LandCover.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

/**
 * Real data for a world spec in one call: every elevation, OSM, land cover and bathymetry file the spec names that is
 * missing is downloaded straight to that path, after the sizes are known and allowed. Pure apart from the network calls
 * in FetchNet, so tests hand in stand-ins.
 */
namespace openworld::world
{
using json = nlohmann::json;

/** One file a spec names. */
struct FetchItem
{
    /** "<source id>.<kind>", or "terrain.coast.bathymetry" for the terrain spec's coast grid. */
    std::string id;
    /** dem, osm, landcover or bathymetry. */
    std::string kind;
    /** The path as the spec gives it after its relative paths are resolved. */
    std::filesystem::path file;
    terrain::GeoBox box;
    /**
     * Where box came from: source (its bbox), coast (the coast step's own bbox), bathymetry source (the first source
     * carrying a bathymetry grid) or sources (all sources' bboxes together).
     */
    std::string boxFrom;
    bool present = false;
    /** The download cache already holds this area: nothing to download. */
    bool cached = false;
    /** Expected download, bytes; 0 when present or cached. */
    uint64_t bytes = 0;
    /** How bytes was worked out. */
    std::string sizeFrom;
    /** Why it can't be fetched (no bbox); empty when it can. */
    std::string problem;
};

struct FetchPlan
{
    std::vector<FetchItem> items;
    /** Sum over missing items. */
    uint64_t bytes = 0;
    int missing = 0;
    /** A missing bathymetry grid has to come from OpenTopography (not cached). */
    bool keyNeeded = false;
};

/** The network: whole-file GETs, WorldCover's ranged reads, and whether the connection is metered. */
struct FetchNet
{
    terrain::Downloader get;
    std::function<terrain::LandCoverFetch(const terrain::GeoBox&)> landcover;
    /** True on a metered connection, nullopt when unknown. */
    std::function<std::optional<bool>()> metered;
    /** Called between attempts with the pause, ms. */
    std::function<void(int)> wait;
};

struct FetchOptions
{
    /** Download cache (the workspace's terrain/dem_cache): tiles and whole areas are kept and reused. */
    std::filesystem::path cache;
    /** Use the cache only; a missing file that isn't there is an error. */
    bool offline = false;
    /** Allow more than consentMB, or any download on a metered connection. */
    bool consent = false;
    /** OpenTopography key for GEBCO; never logged or put in a message. */
    std::string apiKey;
    /** Tries per download when the server can't be reached or answers 429 or 5xx. */
    int attempts = 3;
    double consentMB = 25.0;
};

/** Copernicus GLO-30 tiles are about this size each. */
inline constexpr uint64_t kCopernicusTileBytes = 25ull << 20;

/**
 * Every file the spec names (remix sources' dem, osm, landcover, bathymetry; terrain.coast.bathymetry.file), whether it
 * is there and how much a download would be. A file named twice is one item. resolve maps a relative path (a spec given as a dict) to a real one.
 */
FetchPlan planFetch(const json& spec, const std::filesystem::path& cache, const std::function<std::filesystem::path(const std::string&)>& resolve = {});

/** The plan as JSON: items (id, kind, file, state present|missing, bytes, megabytes, sizeFrom, bbox, problem), totals. */
json planJson(const FetchPlan& p, const FetchOptions& o);

/** A download size for people: "12 KB" under 100 KB, else "4.7 MB". */
std::string fetchSize(uint64_t bytes);

/** One line naming the missing items and the total: "coast.dem 25.0 MB, coast.osm 4.7 MB; about 29.7 MB". */
std::string planSummary(const FetchPlan& p);

/**
 * Downloads every missing item to its file. Refuses before any download when an item can't be fetched, when GEBCO is
 * needed without a key, or when the total is over consentMB (or the connection is metered) without consent; the message
 * names the sizes. Each download is retried (attempts) and written whole (a .part file renamed), so a run stopped by a
 * failure resumes where it stopped. Returns items with state fetched|present, bytes written, attempts and notes, plus
 * the attribution files written. Throws std::runtime_error naming the item and why.
 */
json runFetch(const FetchPlan& p, const FetchOptions& o, const FetchNet& net);
} // namespace openworld::world

#pragma once

#include <glm/glm.hpp>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * The World Remix spec: real places (each a DEM, OSM data and land cover over a box) placed on one map, scaled, cut,
 * moved, duplicated and deleted, with pins warping the empty land between. Lengths cm on the map (+X east, -Z north),
 * angles degrees counter-clockwise seen from above. Every id is stable: the same id replaces.
 */
namespace openworld
{
using json = nlohmann::json;
using Vec2 = glm::dvec2;
using Polygon = std::vector<Vec2>;

/** Turn by angle about pivot, then move by translate. */
struct Rigid
{
    double angle = 0.0;
    Vec2 pivot{0.0};
    Vec2 translate{0.0};

    Vec2 apply(Vec2 p) const;
    Vec2 inverse(Vec2 p) const;
    /** The same motion t of the way (0 none, 1 all). */
    Rigid partial(double t) const { return {angle * t, pivot, translate * t}; }
    bool identity() const { return angle == 0.0 && translate == Vec2(0.0); }
};

/** Turns (x, z) by degrees counter-clockwise seen from above (+X east toward -Z north). */
Vec2 rotated(Vec2 v, double degrees);

enum class RegionOp : uint8_t
{
    /** Moved rigidly; the land around stretches and compresses to follow (no seam). */
    Move,
    /** Held rigid where it is while the land around warps. */
    Keep,
    /** Lifted out and placed at the target; the hole is filled and both edges become seams. */
    Cut,
    /** Copied to the target; the original stays. */
    Duplicate,
    /** Removed; the hole is filled as land. */
    Delete,
};
const std::vector<std::string>& regionOpNames();
bool parseRegionOp(std::string_view name, RegionOp* out);
/** Moves and keeps shape the warp field; cuts, duplicates and deletes paste or punch. */
inline bool warps(RegionOp op) { return op == RegionOp::Move || op == RegionOp::Keep; }
inline bool pastes(RegionOp op) { return op == RegionOp::Cut || op == RegionOp::Duplicate; }
inline bool punches(RegionOp op) { return op == RegionOp::Cut || op == RegionOp::Delete; }

struct Region
{
    std::string id;
    RegionOp op = RegionOp::Cut;
    /** On the base map (sources placed and scaled, before any region or pin). */
    Polygon polygon;
    Vec2 translate{0.0};
    double angle = 0.0;
    /** Turn pivot; the polygon's centroid when not given. */
    std::optional<Vec2> pivot;
    /** Seam band width, cm; the spec's seams.width when not given. */
    std::optional<double> seam;

    Rigid rigid() const;
    /** Where the region ends up (the polygon for keep and delete). */
    Polygon target() const;
};

struct Pin
{
    std::string id;
    Vec2 at{0.0};
    /** Where at goes; at itself for an anchor. */
    Vec2 to{0.0};
};

struct Source
{
    std::string id;
    /** Files, relative to the workspace or absolute: elevation (any Terrain reads), OSM (.osm, .json, .pbf), WorldCover GeoTIFF. */
    std::string dem;
    std::string osm;
    std::string landcover;
    /** A bathymetry grid (GEBCO GeoTIFF, or anything Terrain reads as elevation) around the place: shapes the sea floor
     * where this source's elevation leaves the sea flat and where no source covers, near it. */
    std::string bathymetry;
    /** [west, south, east, north] degrees: the area remixFetch downloads; also crops the data. Empty for the files' own. */
    std::vector<double> bbox;
    /** Where the source's centre lands on the base map, cm, and its turn. */
    Vec2 at{0.0};
    double angle = 0.0;
    /** Its own horizontal scale (a factor); 0 takes the spec's. */
    double scale = 0.0;
};

struct RemixSpec
{
    /** Distance factor (1/3 is 1:3) and height factor (1/1.5). */
    double horizontal = 1.0 / 3.0;
    double vertical = 1.0 / 1.5;
    /** Latitude and longitude the map's centre is given in the remixed OSM; the first source's centre when not given. */
    std::optional<glm::dvec2> origin;

    struct Map
    {
        /** 0 size fits everything placed. */
        Vec2 center{0.0};
        Vec2 size{0.0};
        /** 0 takes the finest source's cell times the horizontal scale. */
        double cell = 0.0;
        /** Sea floor height where no source covers the map, cm, used only with bathymetry off. */
        double seaDepth = -3000.0;
        int maxResolution = 2049;
    } map;

    std::vector<Source> sources;
    std::vector<Region> regions;
    std::vector<Pin> pins;

    struct Warp
    {
        /** How far a moved region or pin pulls the land around, cm; 0 is a quarter of the map's longer side. */
        double falloff = 0.0;
        /** Most integration steps; more resolve tighter squeezes. */
        int maxSteps = 256;
    } warp;

    struct Seams
    {
        double width = 1500.0;
        /** Erosion steps re-run in seam bands; 0 off. */
        int erosion = 20;
        /** Detail added in seam bands before erosion, 0 to 1. */
        double detail = 0.3;
        bool roads = true;
        /** Farthest two road ends are joined when nothing else pairs them, cm; 0 is four seam widths. */
        double roadReach = 0.0;
        bool rivers = true;
        bool coast = true;
        uint32_t seed = 0;
    } seams;

    /**
     * The sea floor (Terrain's bathymetry): {enabled (default true), shoreSlope, blendWidth, smoothing, depthScale, shelfDepth,
     * shelfWidth, dropDepth, slopeWidth, minDepth, landTolerance}, lengths cm on the map. Where no source covers, and where a
     * source's elevation leaves the sea flat, the floor is the sources' bathymetry grids blended in from a shelf profile.
     */
    json bathymetry = json::object();
    bool bathymetryOn() const { return bathymetry.value("enabled", true); }

    /** Overlay and preview settings the UI keeps with the spec: {overlay: bool, show: "after"|"before"}. */
    json view = json::object();

    const Source* source(std::string_view id) const;
    const Region* region(std::string_view id) const;
    double seamOf(const Region& r) const { return r.seam ? *r.seam : seams.width; }
};

json toJson(const RemixSpec& s);
/** Throws std::runtime_error naming the bad field. Scale fields take a factor or "1:N". */
RemixSpec specFromJson(const json& j);
/** JSON schema, served as lila://schema/remix-spec. */
json remixSpecSchema();

/** Parses 0.333 or "1:3" into a factor. Throws std::runtime_error. */
double parseScale(const json& v, const char* field);
std::string scaleText(double factor);

// ---------------------------------------------------------------------------------------------------------------------
// Polygon helpers on the map plane

bool inside(const Polygon& poly, Vec2 p);
/** Distance to the outline; 0 on it. */
double distanceTo(const Polygon& poly, Vec2 p);
/** Positive inside, negative outside. */
double signedDistance(const Polygon& poly, Vec2 p);
Vec2 centroid(const Polygon& poly);
double area(const Polygon& poly);
bool overlaps(const Polygon& a, const Polygon& b);
Polygon transformed(const Polygon& poly, const Rigid& r);
} // namespace openworld

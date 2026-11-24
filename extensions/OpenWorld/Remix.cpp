// World Remix: sources read and placed, heights composed through the warp and the pasted regions, holes filled, seams
// blended and eroded; then the features (Features.cpp) and their repairs (Repair.cpp).

#include "RemixWork.h"

#include "Erosion.h"
#include "Inputs.h"
#include "LandCover.h"
#include "Osm.h"

#include <lila/Log.h>
#include <lila/Paths.h>
#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <list>
#include <mutex>
#include <set>

namespace fs = std::filesystem;

namespace openworld
{
using namespace detail;

namespace
{
constexpr const char* kCodeMask = "worldcover";
constexpr double kRemixDeg = 0.017453292519943295;

double metresPerDegLat(double lat) { return 111132.92 - 559.82 * std::cos(2 * lat * kRemixDeg) + 1.175 * std::cos(4 * lat * kRemixDeg); }
double metresPerDegLon(double lat) { return 111412.84 * std::cos(lat * kRemixDeg) - 93.5 * std::cos(3 * lat * kRemixDeg); }

/** A Local georeference with latitude and longitude ll at local point at, as terrainImportOsm(origin=) builds it. */
terrain::Georef localGeoref(glm::dvec2 ll, Vec2 at)
{
    terrain::Georef g;
    g.kind = terrain::ProjectionKind::Local;
    g.latMid = ll.x;
    g.x0 = ll.y;
    g.y0 = ll.x;
    g.mx = metresPerDegLon(ll.x);
    g.my = metresPerDegLat(ll.x);
    g.origin = at;
    return g;
}

double smooth01(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

fs::path resolved(const std::string& f) { return lila::paths::resolvePath(f); }

int64_t stampOf(const std::string& f)
{
    if (f.empty())
        return 0;
    std::error_code ec;
    const auto t = fs::last_write_time(resolved(f), ec);
    return ec ? -1 : int64_t(t.time_since_epoch().count());
}

/** The window of a geographic Dem inside bbox. */
terrain::Dem cropDem(const terrain::Dem& d, const std::vector<double>& bbox)
{
    if (bbox.size() != 4 || !d.geographic || d.dx <= 0.0 || d.dy <= 0.0)
        return d;
    const int x0 = std::clamp(int(std::floor((bbox[0] - d.x0) / d.dx)), 0, d.width - 1);
    const int x1 = std::clamp(int(std::ceil((bbox[2] - d.x0) / d.dx)), 0, d.width - 1);
    const int y0 = std::clamp(int(std::floor((d.y0 - bbox[3]) / d.dy)), 0, d.height - 1);
    const int y1 = std::clamp(int(std::ceil((d.y0 - bbox[1]) / d.dy)), 0, d.height - 1);
    if (x1 - x0 < 1 || y1 - y0 < 1)
        throw std::runtime_error("bbox does not overlap the elevation data");
    terrain::Dem c = d;
    c.width = x1 - x0 + 1;
    c.height = y1 - y0 + 1;
    c.x0 = d.x0 + x0 * d.dx;
    c.y0 = d.y0 - y0 * d.dy;
    c.metres.resize(size_t(c.width) * c.height);
    c.filled.clear();
    for (int y = 0; y < c.height; ++y)
        for (int x = 0; x < c.width; ++x)
            c.metres[size_t(y) * c.width + x] = d.metres[size_t(y + y0) * d.width + x + x0];
    return c;
}
} // namespace

// ---------------------------------------------------------------------------------------------------- sources

std::shared_ptr<const SourceData> loadSource(const Source& s)
{
    LILA_ZONE();

    struct Key
    {
        std::string text;
        bool operator<(const Key& o) const { return text < o.text; }
    };
    static std::mutex lock;
    static std::map<Key, std::shared_ptr<const SourceData>> cache;
    static std::deque<Key> order;
    std::string key = s.dem + "|" + std::to_string(stampOf(s.dem)) + "|" + s.osm + "|" + std::to_string(stampOf(s.osm)) + "|" + s.landcover + "|" +
                      std::to_string(stampOf(s.landcover));
    for (double v : s.bbox)
        key += "|" + std::to_string(v);
    {
        std::lock_guard l(lock);
        if (auto it = cache.find({key}); it != cache.end())
        {
            auto copy = std::make_shared<SourceData>(*it->second);
            copy->id = s.id;
            return copy;
        }
    }

    auto out = std::make_shared<SourceData>();
    out->id = s.id;
    if (s.dem.empty() && s.osm.empty())
        throw std::runtime_error("source " + s.id + ": give dem= or osm= (or remixFetch with bbox=)");
    if (!s.osm.empty())
    {
        const fs::path p = resolved(s.osm);
        if (!fs::exists(p))
            throw std::runtime_error("source " + s.id + ": no OSM file " + lila::paths::toUtf8(p));
        out->osm = readOsmDoc(p, &out->notes);
    }
    if (!s.dem.empty())
    {
        const fs::path p = resolved(s.dem);
        if (!fs::exists(p))
            throw std::runtime_error("source " + s.id + ": no elevation file " + lila::paths::toUtf8(p));
        terrain::Dem dem = cropDem(terrain::readDem(p), s.bbox);
        if (!dem.geographic && dem.epsg == 0)
            out->notes.push_back("source " + s.id + ": elevation is not in degrees; its OSM data and land cover can't be placed on it");
        terrain::DemOptions o;
        out->hf = terrain::demToTerrain(std::move(dem), o, terrain::ProjectionKind::Local, 0, true, &out->geo);
        out->hasDem = true;
    }
    else
    {
        // No elevation: flat ground at 1 m over the bbox or the OSM data's extent.
        double w = 180, so = 90, e = -180, n = -90;
        if (s.bbox.size() == 4)
            std::tie(w, so, e, n) = std::tuple(s.bbox[0], s.bbox[1], s.bbox[2], s.bbox[3]);
        else
            for (const OsmDoc::Node& nd : out->osm.nodes)
            {
                w = std::min(w, nd.at.lon), e = std::max(e, nd.at.lon);
                so = std::min(so, nd.at.lat), n = std::max(n, nd.at.lat);
            }
        if (!(e > w && n > so))
            throw std::runtime_error("source " + s.id + ": no elevation and no area (give bbox=)");
        const glm::dvec2 centre((so + n) * 0.5, (w + e) * 0.5);
        const double width = (e - w) * metresPerDegLon(centre.x) * 100.0, depth = (n - so) * metresPerDegLat(centre.x) * 100.0;
        const double cell = 3000.0;
        out->hf = terrain::Heightfield();
        out->hf.cell = cell;
        out->hf.nx = std::max(2, int(std::ceil(width / cell)) + 1);
        out->hf.nz = std::max(2, int(std::ceil(depth / cell)) + 1);
        out->hf.origin = -out->hf.extent() * 0.5;
        out->hf.heights.assign(out->hf.size(), 100.0f);
        out->geo = localGeoref(centre, Vec2(0.0));
        out->notes.push_back("source " + s.id + ": no elevation file; flat ground at 1 m");
    }
    if (!s.landcover.empty())
    {
        const fs::path p = resolved(s.landcover);
        if (!fs::exists(p))
            throw std::runtime_error("source " + s.id + ": no land cover file " + lila::paths::toUtf8(p));
        terrain::rasterizeWorldCover(out->hf, terrain::readDem(p), out->geo, kCodeMask, true);
    }
    if (!out->osm.empty())
    {
        const terrain::OsmExtract x = terrain::extractOsm(toOsmData(out->osm));
        if (!x.areas.empty())
            terrain::rasterizeLandcover(out->hf, x.areas, out->geo);
    }

    std::lock_guard l(lock);
    cache[{key}] = out;
    order.push_back({key});
    while (order.size() > 8)
    {
        cache.erase(order.front());
        order.pop_front();
    }
    return out;
}

Vec2 placeOnBase(const Source& s, double horizontal, Vec2 local)
{
    const double k = s.scale > 0.0 ? s.scale : horizontal;
    return rotated(local * k, s.angle) + s.at;
}

Vec2 baseToSource(const Source& s, double horizontal, Vec2 base)
{
    const double k = s.scale > 0.0 ? s.scale : horizontal;
    return rotated(base - s.at, -s.angle) / k;
}

Polygon footprintOf(const Source& s, double horizontal, const Heightfield& hf)
{
    const Vec2 a = hf.origin, b = hf.origin + hf.extent();
    return {placeOnBase(s, horizontal, a), placeOnBase(s, horizontal, {b.x, a.y}), placeOnBase(s, horizontal, b), placeOnBase(s, horizontal, {a.x, b.y})};
}

// ---------------------------------------------------------------------------------------------------- work helpers

namespace detail
{
double Work::weight(size_t s, Vec2 base) const
{
    if (!data[s])
        return 0.0;
    const Heightfield& h = data[s]->hf;
    const Vec2 l = baseToSource(spec.sources[s], spec.horizontal, base) - h.origin, e = h.extent();
    if (l.x < 0.0 || l.y < 0.0 || l.x > e.x || l.y > e.y)
        return 0.0;
    const double d = std::min({l.x, l.y, e.x - l.x, e.y - l.y}) * scales[s];
    return smooth01(d / feather);
}

bool Work::owns(size_t s, Vec2 base) const
{
    if (!data[s])
        return false;
    // Inclusive of the edge: features drawn to the data's border stay.
    const Heightfield& h = data[s]->hf;
    const Vec2 l = baseToSource(spec.sources[s], spec.horizontal, base) - h.origin, e = h.extent();
    const double tol = 1e-6 * std::max(e.x, e.y);
    if (l.x < -tol || l.y < -tol || l.x > e.x + tol || l.y > e.y + tol)
        return false;
    for (size_t k = s + 1; k < data.size(); ++k)
        if (weight(k, base) >= 0.5)
            return false;
    return true;
}

bool Work::punched(Vec2 base) const
{
    for (const Region& r : spec.regions)
        if (punches(r.op) && inside(r.polygon, base))
            return true;
    return false;
}

bool Work::covered(Vec2 q, int above) const
{
    for (size_t i = size_t(above + 1); i < spec.regions.size(); ++i)
        if (pastes(spec.regions[i].op) && inside(targets[i], q))
            return true;
    return false;
}

double bilinear(const std::vector<float>& v, const Heightfield& h, Vec2 p)
{
    const double gx = std::clamp((p.x - h.origin.x) / h.cell, 0.0, double(h.nx - 1));
    const double gz = std::clamp((p.y - h.origin.y) / h.cell, 0.0, double(h.nz - 1));
    const int x0 = std::min(int(gx), h.nx - 2), z0 = std::min(int(gz), h.nz - 2);
    const double fx = gx - x0, fz = gz - z0;
    const auto at = [&](int x, int z) { return double(v[size_t(z) * h.nx + x]); };
    return (at(x0, z0) * (1 - fx) + at(x0 + 1, z0) * fx) * (1 - fz) + (at(x0, z0 + 1) * (1 - fx) + at(x0 + 1, z0 + 1) * fx) * fz;
}

size_t nearestIndex(const Heightfield& h, Vec2 p)
{
    const int x = std::clamp(int(std::lround((p.x - h.origin.x) / h.cell)), 0, h.nx - 1);
    const int z = std::clamp(int(std::lround((p.y - h.origin.y) / h.cell)), 0, h.nz - 1);
    return h.index(x, z);
}

int64_t Features::addNode(Vec2 at, OsmTags tags)
{
    const int64_t id = nextId++;
    nodeAt[id] = nodes.size();
    nodes.push_back({id, at, std::move(tags)});
    return id;
}
} // namespace detail

// ---------------------------------------------------------------------------------------------------- composing

namespace
{
/** Every mask the sources carry, the code mask (nearest, never blended) marked. */
struct MaskSet
{
    std::vector<std::string> names;
    std::vector<bool> code;
    /** [source][mask] the source's values, or null. */
    std::vector<std::vector<const std::vector<float>*>> of;
};

MaskSet masksOf(const Work& w)
{
    MaskSet m;
    std::set<std::string> names;
    for (const auto& d : w.data)
        if (d)
            for (const auto& [n, v] : d->hf.masks)
                if (n != "voids")
                    names.insert(n);
    m.names.assign(names.begin(), names.end());
    for (const std::string& n : m.names)
        m.code.push_back(n == kCodeMask);
    m.of.resize(w.data.size());
    for (size_t s = 0; s < w.data.size(); ++s)
        for (const std::string& n : m.names)
            m.of[s].push_back(w.data[s] ? w.data[s]->hf.findMask(n) : nullptr);
    return m;
}

struct Sample
{
    double height = 0.0;
    double seam = 0.0;
    bool covered = false;
};

/** The base map at b: sources blended in order (later on top) over the sea floor. */
Sample baseSample(const Work& w, const MaskSet& m, Vec2 b, float* masks)
{
    Sample out;
    out.height = w.spec.map.seaDepth;
    for (size_t k = 0; k < m.names.size(); ++k)
        masks[k] = 0.0f;
    bool under = false;
    for (size_t s = 0; s < w.data.size(); ++s)
    {
        const double wt = w.weight(s, b);
        if (wt <= 0.0)
            continue;
        const Heightfield& h = w.data[s]->hf;
        const Vec2 l = baseToSource(w.spec.sources[s], w.spec.horizontal, b);
        const double hs = h.heightAt(l.x, l.y) * w.spec.vertical;
        out.height += (hs - out.height) * wt;
        for (size_t k = 0; k < m.names.size(); ++k)
        {
            const std::vector<float>* v = m.of[s][k];
            if (m.code[k])
            {
                if (wt >= 0.5)
                    masks[k] = v ? (*v)[nearestIndex(h, l)] : 0.0f;
                continue;
            }
            const double value = v ? bilinear(*v, h, l) : 0.0;
            masks[k] = float(masks[k] + (value - masks[k]) * wt);
        }
        if (wt < 1.0 && under)
            out.seam = std::max(out.seam, 1.0 - std::abs(2.0 * wt - 1.0));
        under = true;
        out.covered = true;
    }
    return out;
}

/** Push-pull: unknown samples (known 0) filled smoothly from the known ones, then relaxed. */
void fillHoles(std::vector<float>& v, const std::vector<uint8_t>& hole, int nx, int nz)
{
    struct Level
    {
        int nx, nz;
        std::vector<double> sum, weight;
    };
    std::vector<Level> levels;
    levels.push_back({nx, nz, std::vector<double>(size_t(nx) * nz), std::vector<double>(size_t(nx) * nz)});
    for (size_t i = 0; i < v.size(); ++i)
        if (!hole[i])
        {
            levels[0].sum[i] = v[i];
            levels[0].weight[i] = 1.0;
        }
    while (levels.back().nx > 2 || levels.back().nz > 2)
    {
        const Level& f = levels.back();
        Level c{(f.nx + 1) / 2, (f.nz + 1) / 2, {}, {}};
        c.sum.assign(size_t(c.nx) * c.nz, 0.0);
        c.weight.assign(size_t(c.nx) * c.nz, 0.0);
        for (int z = 0; z < f.nz; ++z)
            for (int x = 0; x < f.nx; ++x)
            {
                const size_t fi = size_t(z) * f.nx + x, ci = size_t(z / 2) * c.nx + x / 2;
                c.sum[ci] += f.sum[fi];
                c.weight[ci] += f.weight[fi];
            }
        levels.push_back(std::move(c));
    }
    for (Level& l : levels)
        for (size_t i = 0; i < l.sum.size(); ++i)
            if (l.weight[i] > 0.0)
            {
                l.sum[i] /= l.weight[i];
                l.weight[i] = std::min(1.0, l.weight[i]);
            }
    for (size_t k = levels.size() - 1; k-- > 0;)
    {
        Level& f = levels[k];
        const Level& c = levels[k + 1];
        for (int z = 0; z < f.nz; ++z)
            for (int x = 0; x < f.nx; ++x)
            {
                const size_t i = size_t(z) * f.nx + x;
                if (f.weight[i] >= 1.0)
                    continue;
                const double gx = std::clamp((x - 0.5) * 0.5, 0.0, double(c.nx - 1)), gz = std::clamp((z - 0.5) * 0.5, 0.0, double(c.nz - 1));
                const int x0 = std::min(int(gx), std::max(0, c.nx - 2)), z0 = std::min(int(gz), std::max(0, c.nz - 2));
                const int x1 = std::min(x0 + 1, c.nx - 1), z1 = std::min(z0 + 1, c.nz - 1);
                const double fx = gx - x0, fz = gz - z0;
                const auto at = [&](int xx, int zz) { return c.sum[size_t(zz) * c.nx + xx]; };
                const double up = (at(x0, z0) * (1 - fx) + at(x1, z0) * fx) * (1 - fz) + (at(x0, z1) * (1 - fx) + at(x1, z1) * fx) * fz;
                f.sum[i] = f.sum[i] * f.weight[i] + up * (1.0 - f.weight[i]);
                f.weight[i] = 1.0;
            }
    }
    for (size_t i = 0; i < v.size(); ++i)
        if (hole[i])
            v[i] = float(levels[0].sum[i]);
    // Relaxed toward a membrane so the fill meets its rim without a crease.
    std::vector<float> next = v;
    for (int it = 0; it < 40; ++it)
    {
        tbb::parallel_for(1, nz - 1, [&](int z) {
            for (int x = 1; x < nx - 1; ++x)
            {
                const size_t i = size_t(z) * nx + x;
                if (hole[i])
                    next[i] = 0.25f * (v[i - 1] + v[i + 1] + v[i - nx] + v[i + nx]);
            }
        });
        std::swap(v, next);
        for (size_t i = 0; i < v.size(); ++i)
            if (!hole[i])
                next[i] = v[i];
    }
}

/** Code masks into holes: each hole sample takes its nearest known sample's code. */
void fillNearest(std::vector<float>& v, const std::vector<uint8_t>& hole, int nx, int nz)
{
    std::vector<uint8_t> done(v.size());
    std::deque<size_t> q;
    for (size_t i = 0; i < v.size(); ++i)
        if (!hole[i])
        {
            done[i] = 1;
            q.push_back(i);
        }
    while (!q.empty())
    {
        const size_t i = q.front();
        q.pop_front();
        const int x = int(i % nx), z = int(i / nx);
        for (auto [dx, dz] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}})
        {
            const int xx = x + dx, zz = z + dz;
            if (xx < 0 || zz < 0 || xx >= nx || zz >= nz)
                continue;
            const size_t j = size_t(zz) * nx + xx;
            if (!done[j])
            {
                done[j] = 1;
                v[j] = v[i];
                q.push_back(j);
            }
        }
    }
}

/** Chamfer distance in cells from the nearest set sample (0 on them). */
std::vector<float> distanceFrom(const std::vector<uint8_t>& set, int nx, int nz)
{
    const float big = 1e9f, a = 1.0f, d = 1.41421356f;
    std::vector<float> dist(set.size(), big);
    for (size_t i = 0; i < set.size(); ++i)
        if (set[i])
            dist[i] = 0.0f;
    auto relax = [&](int x, int z, int dx, int dz, float c) {
        const int xx = x + dx, zz = z + dz;
        if (xx < 0 || zz < 0 || xx >= nx || zz >= nz)
            return;
        float& t = dist[size_t(z) * nx + x];
        t = std::min(t, dist[size_t(zz) * nx + xx] + c);
    };
    for (int z = 0; z < nz; ++z)
        for (int x = 0; x < nx; ++x)
        {
            relax(x, z, -1, 0, a), relax(x, z, 0, -1, a), relax(x, z, -1, -1, d), relax(x, z, 1, -1, d);
        }
    for (int z = nz - 1; z >= 0; --z)
        for (int x = nx - 1; x >= 0; --x)
        {
            relax(x, z, 1, 0, a), relax(x, z, 0, 1, a), relax(x, z, 1, 1, d), relax(x, z, -1, 1, d);
        }
    return dist;
}

/** Fractal noise about -1 to 1, featureSize cm. */
double fbm(Vec2 p, double featureSize, uint32_t seed)
{
    double sum = 0.0, amp = 1.0, norm = 0.0, f = 1.0 / featureSize;
    for (int o = 0; o < 4; ++o)
    {
        sum += terrain::gradientNoise(p.x * f, p.y * f, seed + uint32_t(o) * 7919u) * amp;
        norm += amp;
        amp *= 0.5;
        f *= 2.0;
    }
    return sum / norm;
}

/**
 * Seam bands get detail and Terrain's erosion re-run there, held outside: heights outside the bands are kept exactly.
 * Returns the erosion steps run.
 */
int erodeSeams(const RemixSpec& spec, Heightfield& h)
{
    LILA_ZONE();

    const std::vector<float>* seam = h.findMask("remixSeam");
    if (!seam || (spec.seams.erosion <= 0 && spec.seams.detail <= 0.0))
        return 0;
    int x0 = h.nx, z0 = h.nz, x1 = -1, z1 = -1;
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
            if ((*seam)[h.index(x, z)] > 0.0f)
            {
                x0 = std::min(x0, x), x1 = std::max(x1, x);
                z0 = std::min(z0, z), z1 = std::max(z1, z);
            }
    if (x1 < 0)
        return 0;
    const int margin = 4;
    x0 = std::max(0, x0 - margin), z0 = std::max(0, z0 - margin), x1 = std::min(h.nx - 1, x1 + margin), z1 = std::min(h.nz - 1, z1 + margin);
    if (x1 - x0 < 2 || z1 - z0 < 2)
        return 0;
    Heightfield c;
    c.nx = x1 - x0 + 1;
    c.nz = z1 - z0 + 1;
    c.cell = h.cell;
    c.origin = h.samplePos(x0, z0);
    c.heights.resize(c.size());
    std::vector<float>& hold = c.mask("remixHold");
    std::vector<float>& holdHeight = c.mask("remixHoldHeight");
    std::vector<float> s(c.size());
    for (int z = 0; z < c.nz; ++z)
        for (int x = 0; x < c.nx; ++x)
        {
            const size_t i = c.index(x, z), j = h.index(x + x0, z + z0);
            s[i] = (*seam)[j];
            c.heights[i] = h.heights[j];
            hold[i] = 1.0f - s[i];
            holdHeight[i] = h.heights[j];
        }
    if (spec.seams.detail > 0.0)
        for (int z = 1; z < c.nz - 1; ++z)
            for (int x = 1; x < c.nx - 1; ++x)
            {
                const size_t i = c.index(x, z);
                if (s[i] <= 0.0f)
                    continue;
                const double gx = (c.heights[i + 1] - c.heights[i - 1]) / (2.0 * c.cell), gz = (c.heights[i + c.nx] - c.heights[i - c.nx]) / (2.0 * c.cell);
                const double slope = std::sqrt(gx * gx + gz * gz);
                const double amp = spec.seams.detail * 0.5 * c.cell * (1.0 + 4.0 * slope);
                c.heights[i] += float(s[i] * amp * fbm(c.samplePos(x, z), c.cell * 8.0, spec.seams.seed + 101u));
            }
    if (spec.seams.erosion > 0)
    {
        terrain::ErosionParams p;
        p.iterations = spec.seams.erosion;
        p.constraintMask = "remixHold";
        p.constraintHeightMask = "remixHoldHeight";
        terrain::erode(c, p);
    }
    for (int z = 0; z < c.nz; ++z)
        for (int x = 0; x < c.nx; ++x)
        {
            const size_t i = c.index(x, z), j = h.index(x + x0, z + z0);
            h.heights[j] = float(h.heights[j] + (c.heights[i] - h.heights[j]) * s[i]);
        }
    return spec.seams.erosion;
}
} // namespace

// ---------------------------------------------------------------------------------------------------- run

std::shared_ptr<RemixResult> runRemix(const RemixSpec& spec, const RunOptions& o)
{
    LILA_ZONE();

    const auto t0 = std::chrono::steady_clock::now();
    auto ms = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
    if (spec.sources.empty())
        throw std::runtime_error("the remix has no sources (remixSource adds one)");
    Work w(spec);
    w.out = std::make_shared<RemixResult>();
    RemixResult& out = *w.out;
    json timing = json::object();

    // Sources.
    double finest = 1e300;
    for (const Source& s : spec.sources)
    {
        std::shared_ptr<const SourceData> d;
        try
        {
            d = loadSource(s);
        }
        catch (const std::exception& e)
        {
            out.errors.push_back(e.what());
            lila::log::warn("remix: {}", e.what());
        }
        w.data.push_back(d);
        const double k = s.scale > 0.0 ? s.scale : spec.horizontal;
        w.scales.push_back(k);
        w.footprints.push_back(d ? footprintOf(s, spec.horizontal, d->hf) : Polygon{});
        if (d)
        {
            for (const std::string& n : d->notes)
                out.notes.push_back(n);
            finest = std::min(finest, d->hf.cell * k);
        }
    }
    if (finest >= 1e300)
        throw std::runtime_error("no source could be read: " + (out.errors.empty() ? std::string() : out.errors.front()));
    timing["sources"] = ms();

    // The output grid.
    Vec2 lo(1e300), hi(-1e300);
    auto grow = [&](Vec2 p) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    };
    for (size_t r = 0; r < spec.regions.size(); ++r)
        w.targets.push_back(spec.regions[r].target());
    if (spec.map.size.x > 0.0 && spec.map.size.y > 0.0)
    {
        lo = spec.map.center - spec.map.size * 0.5;
        hi = spec.map.center + spec.map.size * 0.5;
    }
    else
    {
        for (const Polygon& f : w.footprints)
            for (Vec2 p : f)
                grow(p);
        for (const Polygon& t : w.targets)
            for (Vec2 p : t)
                grow(p);
        for (const Pin& p : spec.pins)
            grow(p.to);
    }
    double cell = spec.map.cell > 0.0 ? spec.map.cell : finest;
    const Vec2 size = hi - lo;
    if (!(size.x > 0.0 && size.y > 0.0))
        throw std::runtime_error("the remix covers no area");
    if (std::max(size.x, size.y) / cell + 1.0 > spec.map.maxResolution)
    {
        const double was = cell;
        cell = std::max(size.x, size.y) / double(spec.map.maxResolution - 1);
        out.notes.push_back("cell " + std::to_string(int(std::round(was))) + " cm would pass map.maxResolution; using " + std::to_string(int(std::round(cell))) + " cm");
    }
    Heightfield& h = out.after;
    h.cell = cell;
    h.nx = std::max(2, int(std::ceil(size.x / cell - 1e-6)) + 1);
    h.nz = std::max(2, int(std::ceil(size.y / cell - 1e-6)) + 1);
    const Vec2 centre = (lo + hi) * 0.5;
    h.origin = centre - h.extent() * 0.5;
    out.lo = h.origin;
    out.hi = h.origin + h.extent();
    w.feather = spec.seams.width;
    for (size_t s = 0; s < w.data.size(); ++s)
        if (w.data[s])
        {
            const Vec2 e = w.data[s]->hf.extent() * w.scales[s];
            w.feather = std::min(w.feather, 0.25 * std::min(e.x, e.y));
        }
    w.feather = std::max(w.feather, cell);

    // Output georeference: the first readable source's centre (or the spec's origin) at map (0, 0).
    glm::dvec2 ll0(0.0);
    if (spec.origin)
        ll0 = *spec.origin;
    else
        for (size_t s = 0; s < w.data.size(); ++s)
            if (w.data[s])
            {
                const terrain::Heightfield& sh = w.data[s]->hf;
                const LatLon c = w.data[s]->geo.toGeographic(sh.origin + sh.extent() * 0.5);
                ll0 = {c.lat, c.lon};
                // The source's centre sits at its placement, not at (0, 0); shift so (0, 0) is where that latitude lands.
                const terrain::Georef g = localGeoref(ll0, spec.sources[s].at);
                const LatLon at0 = g.toGeographic(Vec2(0.0));
                ll0 = {at0.lat, at0.lon};
                break;
            }
    const LatLon llc = localGeoref(ll0, Vec2(0.0)).toGeographic(h.origin + h.extent() * 0.5);
    out.originLatLon = {llc.lat, llc.lon};
    out.geo = localGeoref(out.originLatLon, h.origin + h.extent() * 0.5);

    // The warp.
    std::vector<Warp::Handle> handles, rigid;
    for (const Region& r : spec.regions)
        if (warps(r.op))
        {
            handles.push_back({r.polygon, r.rigid()});
            rigid.push_back({r.target(), r.rigid()});
        }
    const double falloff = spec.warp.falloff > 0.0 ? spec.warp.falloff : 0.25 * std::max(size.x, size.y);
    out.footprints = w.footprints;
    w.warp = std::make_shared<Warp>(handles, spec.pins, falloff, spec.warp.maxSteps, out.lo, out.hi);
    out.warp = w.warp;
    w.inverse = InverseGrid(*w.warp, out.lo, out.hi, std::min(std::max(h.nx, h.nz), 257), rigid);
    timing["warp"] = ms();

    // Background and base map.
    const MaskSet m = masksOf(w);
    const size_t nm = m.names.size();
    h.heights.assign(h.size(), 0.0f);
    std::vector<std::vector<float>> masks(nm, std::vector<float>(h.size(), 0.0f));
    Heightfield& before = out.before;
    before.nx = h.nx, before.nz = h.nz, before.cell = h.cell, before.origin = h.origin;
    before.heights.assign(h.size(), 0.0f);
    std::vector<std::vector<float>> beforeMasks(nm, std::vector<float>(h.size(), 0.0f));
    std::vector<uint8_t> hole(h.size(), 0);
    std::vector<float> seam(h.size(), 0.0f);
    tbb::parallel_for(0, h.nz, [&](int z) {
        std::vector<float> mk(nm);
        for (int x = 0; x < h.nx; ++x)
        {
            const size_t i = h.index(x, z);
            const Vec2 q = h.samplePos(x, z);
            const Sample sb = baseSample(w, m, q, mk.data());
            before.heights[i] = float(sb.height);
            for (size_t k = 0; k < nm; ++k)
                beforeMasks[k][i] = mk[k];
            const Vec2 b = w.inverse(q);
            if (w.punched(b))
            {
                hole[i] = 1;
                continue;
            }
            const Sample s = sb.covered && b == q ? sb : baseSample(w, m, b, mk.data());
            h.heights[i] = float(s.height);
            seam[i] = float(s.seam);
            for (size_t k = 0; k < nm; ++k)
                masks[k][i] = mk[k];
        }
    });
    timing["background"] = ms();

    // Holes where regions were cut out or deleted: filled as land and made seams.
    size_t holes = 0;
    for (uint8_t v : hole)
        holes += v;
    if (holes)
    {
        fillHoles(h.heights, hole, h.nx, h.nz);
        for (size_t k = 0; k < nm; ++k)
            if (m.code[k])
                fillNearest(masks[k], hole, h.nx, h.nz);
            else
                fillHoles(masks[k], hole, h.nx, h.nz);
        const std::vector<float> d = distanceFrom(hole, h.nx, h.nz);
        const double band = std::max(1.0, spec.seams.width / h.cell);
        for (size_t i = 0; i < h.size(); ++i)
            seam[i] = std::max(seam[i], float(1.0 - std::min(1.0, d[i] / band)));
    }
    timing["holes"] = ms();

    // Pasted regions, in order, blended in over their seam band.
    size_t pasted = 0;
    for (size_t r = 0; r < spec.regions.size(); ++r)
    {
        const Region& reg = spec.regions[r];
        if (!pastes(reg.op))
            continue;
        const Polygon& t = w.targets[r];
        const Rigid motion = reg.rigid();
        const double band = std::max(spec.seamOf(reg), h.cell);
        Vec2 tlo(1e300), thi(-1e300);
        for (Vec2 p : t)
            tlo = glm::min(tlo, p), thi = glm::max(thi, p);
        tlo -= Vec2(band), thi += Vec2(band);
        const int xa = std::max(0, int(std::floor((tlo.x - h.origin.x) / h.cell))), xb = std::min(h.nx - 1, int(std::ceil((thi.x - h.origin.x) / h.cell)));
        const int za = std::max(0, int(std::floor((tlo.y - h.origin.y) / h.cell))), zb = std::min(h.nz - 1, int(std::ceil((thi.y - h.origin.y) / h.cell)));
        tbb::parallel_for(za, zb + 1, [&](int z) {
            std::vector<float> mk(nm);
            for (int x = xa; x <= xb; ++x)
            {
                const size_t i = h.index(x, z);
                const Vec2 q = h.samplePos(x, z);
                const double sd = signedDistance(t, q);
                if (std::abs(sd) < band)
                    seam[i] = std::max(seam[i], float(1.0 - std::abs(sd) / band));
                if (sd <= 0.0)
                    continue;
                const double wt = smooth01(sd / band);
                const Sample s = baseSample(w, m, motion.inverse(q), mk.data());
                h.heights[i] = float(h.heights[i] + (s.height - h.heights[i]) * wt);
                for (size_t k = 0; k < nm; ++k)
                    masks[k][i] = m.code[k] ? (wt >= 0.5 ? mk[k] : masks[k][i]) : float(masks[k][i] + (mk[k] - masks[k][i]) * wt);
            }
        });
        ++pasted;
    }
    timing["paste"] = ms();

    for (size_t k = 0; k < nm; ++k)
    {
        h.masks[m.names[k]] = std::move(masks[k]);
        before.masks[m.names[k]] = std::move(beforeMasks[k]);
    }
    h.masks["remixSeam"] = seam;
    std::vector<float>& holeMask = h.mask("remixHole");
    for (size_t i = 0; i < h.size(); ++i)
        holeMask[i] = hole[i] ? 1.0f : 0.0f;
    const int eroded = erodeSeams(spec, h);
    timing["seams"] = ms();

    // Features and their repairs.
    Features f;
    if (o.features)
    {
        moveFeatures(w, f);
        timing["features"] = ms();
        if (spec.seams.roads)
            reconnectRoads(w, f);
        timing["roads"] = ms();
        if (spec.seams.rivers)
            repairRivers(w, f);
        timing["rivers"] = ms();
        if (spec.seams.coast)
            mergeCoast(w, f);
        timing["coast"] = ms();
        assessRivers(w, f);
    }
    finishFeatures(w, f);
    timing["total"] = ms();

    size_t seamSamples = 0;
    for (float v : seam)
        seamSamples += v > 0.0f;
    const glm::dvec2 range = h.range();
    out.stats["grid"] = {{"resolution", {h.nx, h.nz}}, {"cell", h.cell}, {"min", {out.lo.x, out.lo.y}}, {"max", {out.hi.x, out.hi.y}}, {"heights", {range.x, range.y}}};
    out.stats["sources"] = spec.sources.size() - out.errors.size();
    out.stats["regions"] = spec.regions.size();
    out.stats["pasted"] = pasted;
    out.stats["pins"] = spec.pins.size();
    out.stats["warp"] = {{"steps", w.warp->steps()}, {"minStepJacobian", w.warp->minStepJacobian()}, {"falloff", falloff}};
    out.stats["holeSamples"] = holes;
    out.stats["seamSamples"] = seamSamples;
    out.stats["seamErosion"] = eroded;
    out.stats["masks"] = m.names;
    out.stats["origin"] = {out.originLatLon.x, out.originLatLon.y};
    out.stats["ms"] = timing;
    return w.out;
}

std::shared_ptr<const RemixResult> cachedRemix(const RemixSpec& spec)
{
    static std::mutex lock;
    static std::list<std::pair<std::string, std::shared_ptr<const RemixResult>>> cache;
    std::string key = toJson(spec).dump();
    for (const Source& s : spec.sources)
        key += "|" + std::to_string(stampOf(s.dem)) + "|" + std::to_string(stampOf(s.osm)) + "|" + std::to_string(stampOf(s.landcover));
    {
        std::lock_guard l(lock);
        for (auto it = cache.begin(); it != cache.end(); ++it)
            if (it->first == key)
            {
                cache.splice(cache.begin(), cache, it);
                return cache.front().second;
            }
    }
    std::shared_ptr<const RemixResult> r = runRemix(spec);
    std::lock_guard l(lock);
    cache.emplace_front(key, r);
    while (cache.size() > 4)
        cache.pop_back();
    return r;
}

// ---------------------------------------------------------------------------------------------------- contours

std::vector<std::vector<Vec2>> contourLines(const Heightfield& h, double level, Vec2 lo, Vec2 hi)
{
    LILA_ZONE();

    const int x0 = std::clamp(int(std::floor((lo.x - h.origin.x) / h.cell)), 0, h.nx - 1);
    const int x1 = std::clamp(int(std::ceil((hi.x - h.origin.x) / h.cell)), 0, h.nx - 1);
    const int z0 = std::clamp(int(std::floor((lo.y - h.origin.y) / h.cell)), 0, h.nz - 1);
    const int z1 = std::clamp(int(std::ceil((hi.y - h.origin.y) / h.cell)), 0, h.nz - 1);
    // A crossing lives on a grid edge: horizontal edges keyed 2 * index, vertical ones 2 * index + 1.
    auto val = [&](int x, int z) { return double(h.at(x, z)) - level; };
    std::unordered_map<int64_t, Vec2> point;
    std::unordered_map<int64_t, std::vector<int64_t>> link;
    auto crossing = [&](int xa, int za, int xb, int zb, int64_t key) {
        if (point.count(key))
            return;
        const double a = val(xa, za), b = val(xb, zb);
        const double t = a / (a - b);
        point[key] = h.samplePos(xa, za) + (h.samplePos(xb, zb) - h.samplePos(xa, za)) * t;
    };
    for (int z = z0; z < z1; ++z)
        for (int x = x0; x < x1; ++x)
        {
            const double c[4] = {val(x, z), val(x + 1, z), val(x + 1, z + 1), val(x, z + 1)};
            std::vector<int64_t> keys;
            const int64_t top = 2 * int64_t(h.index(x, z)), bottom = 2 * int64_t(h.index(x, z + 1));
            const int64_t left = 2 * int64_t(h.index(x, z)) + 1, right = 2 * int64_t(h.index(x + 1, z)) + 1;
            if ((c[0] > 0) != (c[1] > 0))
                crossing(x, z, x + 1, z, top), keys.push_back(top);
            if ((c[1] > 0) != (c[2] > 0))
                crossing(x + 1, z, x + 1, z + 1, right), keys.push_back(right);
            if ((c[3] > 0) != (c[2] > 0))
                crossing(x, z + 1, x + 1, z + 1, bottom), keys.push_back(bottom);
            if ((c[0] > 0) != (c[3] > 0))
                crossing(x, z, x, z + 1, left), keys.push_back(left);
            if (keys.size() == 2)
            {
                link[keys[0]].push_back(keys[1]);
                link[keys[1]].push_back(keys[0]);
            }
            else if (keys.size() == 4)
            {
                // Saddle: pair by the centre's side.
                const bool centre = (c[0] + c[1] + c[2] + c[3]) * 0.25 > 0;
                const bool pairTopRight = (c[0] > 0) != centre;
                const int64_t a = keys[0], b = keys[1], cc = keys[2], d = keys[3];
                if (pairTopRight)
                    link[a].push_back(b), link[b].push_back(a), link[cc].push_back(d), link[d].push_back(cc);
                else
                    link[a].push_back(d), link[d].push_back(a), link[b].push_back(cc), link[cc].push_back(b);
            }
        }
    std::vector<std::vector<Vec2>> lines;
    std::set<int64_t> used;
    auto walk = [&](int64_t start) {
        std::vector<Vec2> line{point[start]};
        used.insert(start);
        int64_t at = start;
        for (;;)
        {
            int64_t next = -1;
            for (int64_t n : link[at])
                if (!used.count(n))
                {
                    next = n;
                    break;
                }
            if (next < 0)
            {
                for (int64_t n : link[at])
                    if (n == start && line.size() > 2)
                        line.push_back(point[start]);
                break;
            }
            used.insert(next);
            line.push_back(point[next]);
            at = next;
        }
        return line;
    };
    for (const auto& [k, l] : link)
        if (l.size() == 1 && !used.count(k))
            lines.push_back(walk(k));
    for (const auto& [k, l] : link)
        if (!used.count(k))
            lines.push_back(walk(k));
    return lines;
}
} // namespace openworld

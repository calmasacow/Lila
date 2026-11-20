#include "MaskCompute.h"

#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <limits>
#include <queue>

namespace weathering::masks
{
using namespace lila;
using geo::Index;

namespace
{
/** Corner names, render::kGrungeAttrs order. */
constexpr std::array<const char*, kMaskCount> kCornerAttrs = {"grungeEdge", "grungeCavity", "grungeAo", "grungeDust", "grungeStreak", "grungeFade"};

uint32_t maskHash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

double maskUnit(uint32_t seed, uint32_t a, uint32_t b) { return double(maskHash(seed * 0x9e3779b9u ^ maskHash(a * 0x85ebca6bu + b))) / 4294967296.0; }

geo::Mesh toWorld(const geo::Mesh& m, const glm::dmat4& world)
{
    geo::Mesh w = m;
    for (glm::dvec3& q : w.positions())
        q = glm::dvec3(world * glm::dvec4(q, 1.0));
    return w;
}

/** Self and neighbours as world-space BVHs: rays see both, contacts only the others. */
struct Scene
{
    geo::Mesh self, others;
    geo::MeshBvh selfBvh, othersBvh;
    bool hasOthers = false;

    /** Nearest hit of either, and the hit face's normal. */
    bool cast(const glm::dvec3& o, const glm::dvec3& d, double tMax, bool backfaces, double& t, glm::dvec3& n) const
    {
        geo::BvhHit a, b;
        const bool ha = !selfBvh.empty() && selfBvh.raycast(o, d, a, tMax, backfaces);
        const bool hb = hasOthers && othersBvh.raycast(o, d, b, tMax, backfaces);
        if (!ha && !hb)
            return false;
        const bool useA = ha && (!hb || a.t <= b.t);
        t = useA ? a.t : b.t;
        n = useA ? self.faceNormal(a.face) : others.faceNormal(b.face);
        return true;
    }

    bool blocked(const glm::dvec3& o, const glm::dvec3& d, double tMax) const
    {
        return (!selfBvh.empty() && selfBvh.occluded(o, d, tMax)) || (hasOthers && othersBvh.occluded(o, d, tMax));
    }
};

/** A sample: one point under one smoothing group's normal. */
struct Sample
{
    Index point = 0;
    glm::dvec3 normal{0.0};
};

/** A streak source: a horizontal-ish edge water runs down the wall from, with the wall's normal. */
struct StreakSource
{
    glm::dvec3 a{0.0}, b{0.0};
    glm::dvec3 wallNormal{0.0};
};

/** Geodesic distance over the edges from the given seed points, capped at limit. */
std::vector<double> edgeDistance(const geo::Mesh& w, const std::vector<char>& seed, double limit)
{
    std::vector<double> d(w.pointCapacity(), limit);
    using Item = std::pair<double, Index>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> q;
    for (Index p = 0; p < w.pointCapacity(); ++p)
        if (seed[p])
        {
            d[p] = 0.0;
            q.push({0.0, p});
        }
    while (!q.empty())
    {
        auto [dist, p] = q.top();
        q.pop();
        if (dist > d[p])
            continue;
        w.forEachPointEdge(p, [&](Index e) {
            const Index o = w.edgeOtherPoint(e, p);
            const double nd = dist + glm::length(w.position(o) - w.position(p));
            if (nd < d[o])
            {
                d[o] = nd;
                q.push({nd, o});
            }
        });
    }
    return d;
}

double heightOf(const glm::dvec3& p, const glm::dvec3& up) { return glm::dot(p, up); }
} // namespace

Result compute(const geo::Mesh& mesh, const glm::dmat4& world, std::span<const Neighbour> neighbours, const Settings& s)
{
    LILA_ZONE();

    Result r;
    for (std::vector<float>& v : r.corners)
        v.assign(mesh.cornerCapacity(), 0.0f);
    if (mesh.numFaces() == 0)
        return r;

    Scene sc;
    sc.self = toWorld(mesh, world);
    sc.selfBvh.build(sc.self);
    for (const Neighbour& n : neighbours)
        if (n.mesh && n.mesh->numFaces() > 0)
        {
            sc.others.append(toWorld(*n.mesh, n.world));
            sc.hasOthers = true;
        }
    if (sc.hasOthers)
        sc.othersBvh.build(sc.others);

    const geo::Mesh& w = sc.self;
    const glm::dvec3 up = glm::normalize(s.up);
    const geo::Bounds box = w.bounds();
    const double eps = std::max(0.02, 1e-4 * glm::length(box.max - box.min));
    const double cosFeature = std::cos(glm::radians(std::clamp(s.edgeAngle, 1.0, 179.0)));

    std::vector<glm::dvec3> faceN(w.faceCapacity(), glm::dvec3(0.0));
    for (Index f = 0; f < w.faceCapacity(); ++f)
        if (w.faceValid(f))
            faceN[f] = w.faceNormal(f);

    // Feature edges: convex ones seed wear, concave ones dirt. Sills and overhang roots seed streaks.
    std::vector<char> convexPt(w.pointCapacity(), 0), concavePt(w.pointCapacity(), 0);
    std::vector<StreakSource> sources;
    auto faceBelow = [&](Index f, double h) {
        bool below = false;
        w.forEachFaceCorner(f, [&](Index c) { below = below || heightOf(w.position(w.cornerPoint(c)), up) < h - 1e-3; });
        return below;
    };
    for (Index e = 0; e < w.edgeCapacity(); ++e)
    {
        if (!w.edgeValid(e) || w.edgeFaceCount(e) != 2)
            continue;
        const Index c0 = w.edgeCorner(e);
        const Index f0 = w.cornerFace(c0), f1 = w.cornerFace(w.cornerRadial(c0));
        const glm::dvec3 n0 = faceN[f0], n1 = faceN[f1];
        if (glm::dot(n0, n1) > cosFeature)
            continue;
        const bool convex = glm::dot(w.faceCentroid(f1) - w.faceCentroid(f0), n0) < 0.0;
        const Index pa = w.edgePoint(e, 0), pb = w.edgePoint(e, 1);
        (convex ? convexPt : concavePt)[pa] = 1;
        (convex ? convexPt : concavePt)[pb] = 1;
        const glm::dvec3 a = w.position(pa), b = w.position(pb);
        const glm::dvec3 along = b - a;
        if (std::abs(glm::dot(along, up)) > 0.2 * glm::length(along))
            continue;
        const double h = std::min(heightOf(a, up), heightOf(b, up));
        for (int k = 0; k < 2; ++k)
        {
            const Index wall = k == 0 ? f0 : f1, other = k == 0 ? f1 : f0;
            const glm::dvec3 wn = faceN[wall], on = faceN[other];
            if (std::abs(glm::dot(wn, up)) > 0.3 || !faceBelow(wall, h))
                continue;
            // A sill: the wall turns into an up-facing ledge that something covers (an opening's top), so water pools.
            if (convex && glm::dot(on, up) > 0.7)
            {
                double t;
                glm::dvec3 hn;
                if (sc.cast(w.faceCentroid(other) + up * eps, up, s.coverHeight, true, t, hn))
                    sources.push_back({a, b, wn});
            }
            // An overhang's root: the wall meets a down-facing underside, so drips run down from it.
            if (!convex && glm::dot(on, up) < -0.7)
                sources.push_back({a, b, wn});
        }
    }
    const std::vector<double> convexD = edgeDistance(w, convexPt, std::max(s.edgeWidth, 1e-6) * 2.0);
    const std::vector<double> concaveD = edgeDistance(w, concavePt, std::max(s.cavityWidth, 1e-6) * 2.0);

    // Samples: each point once per smoothing group of the faces round it, so a hard-edged block's sides, top and
    // bottom each read their own values at a shared corner.
    std::vector<Sample> samples;
    std::vector<Index> cornerSample(w.cornerCapacity(), geo::kInvalid);
    const double cosSmooth = std::cos(glm::radians(40.0));
    for (Index p = 0; p < w.pointCapacity(); ++p)
    {
        if (!w.pointValid(p))
            continue;
        const size_t first = samples.size();
        w.forEachPointCorner(p, [&](Index c) {
            const glm::dvec3 fn = faceN[w.cornerFace(c)];
            glm::dvec3 sum(0.0);
            w.forEachPointCorner(p, [&](Index o) {
                if (glm::dot(faceN[w.cornerFace(o)], fn) > cosSmooth)
                    sum += w.faceAreaVector(w.cornerFace(o));
            });
            const glm::dvec3 n = glm::length(sum) > 1e-12 ? glm::normalize(sum) : fn;
            for (size_t k = first; k < samples.size(); ++k)
                if (glm::dot(samples[k].normal, n) > 0.999)
                {
                    cornerSample[c] = Index(k);
                    return;
                }
            cornerSample[c] = Index(samples.size());
            samples.push_back({p, n});
        });
    }
    r.samples = samples.size();

    // Hemisphere directions shared by every sample, turned per sample by a seeded angle.
    const int rays = std::clamp(s.aoRays, 1, 256);
    std::vector<glm::dvec3> hemiDirs(static_cast<size_t>(rays));
    for (int k = 0; k < rays; ++k)
    {
        const double u = (double(k) + 0.5) / double(rays);
        const double phi = double(k) * std::numbers::pi * (3.0 - std::sqrt(5.0));
        const double sinT = std::sqrt(u), cosT = std::sqrt(1.0 - u);
        hemiDirs[size_t(k)] = {std::cos(phi) * sinT, std::sin(phi) * sinT, cosT};
    }

    std::array<std::vector<float>, kMaskCount> values;
    for (std::vector<float>& v : values)
        v.assign(samples.size(), 0.0f);
    tbb::parallel_for(size_t(0), samples.size(), [&](size_t i) {
        const Sample& smp = samples[i];
        const glm::dvec3 p = w.position(smp.point);
        const glm::dvec3 n = smp.normal;
        const glm::dvec3 o = p + n * eps;
        const double facing = glm::dot(n, up);
        const double height = heightOf(p, up) - s.groundHeight;

        values[kEdge][i] = float(std::clamp(1.0 - convexD[smp.point] / std::max(s.edgeWidth, 1e-6), 0.0, 1.0));

        double contact = concaveD[smp.point];
        if (sc.hasOthers)
        {
            geo::BvhClosest c;
            if (sc.othersBvh.closestPoint(p, c, s.cavityWidth))
                contact = std::min(contact, c.distance);
        }
        if (s.groundPlane && height > -eps && facing < 0.5)
            contact = std::min(contact, std::max(height, 0.0));
        values[kCavity][i] = float(std::clamp(1.0 - contact / std::max(s.cavityWidth, 1e-6), 0.0, 1.0));

        // Occlusion over a cosine-weighted hemisphere, turned per sample so neighbours don't band.
        const glm::dvec3 t0 = glm::normalize(glm::cross(std::abs(n.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0), n));
        const glm::dvec3 t1 = glm::cross(n, t0);
        const double turn = maskUnit(s.seed, uint32_t(smp.point), uint32_t(i)) * 2.0 * std::numbers::pi;
        const double ct = std::cos(turn), st = std::sin(turn);
        int hits = 0;
        for (const glm::dvec3& h : hemiDirs)
        {
            const double x = h.x * ct - h.y * st, y = h.x * st + h.y * ct;
            const glm::dvec3 d = t0 * x + t1 * y + n * h.z;
            bool hit = sc.blocked(o, d, s.aoRadius);
            if (!hit && s.groundPlane && glm::dot(d, up) < -1e-6 && height > -eps)
                hit = (heightOf(o, up) - s.groundHeight) / -glm::dot(d, up) < s.aoRadius;
            hits += hit ? 1 : 0;
        }
        values[kAo][i] = float(double(hits) / double(rays));

        if (facing > 0.3)
        {
            const double open = sc.blocked(o, up, s.dustCover) ? 0.0 : 1.0;
            values[kDust][i] = float(std::clamp((facing - 0.3) / 0.7, 0.0, 1.0) * open);
        }

        if (std::abs(facing) < 0.5)
        {
            double streak = 0.0;
            for (const StreakSource& src : sources)
            {
                if (glm::dot(src.wallNormal, n) < 0.8 || std::abs(glm::dot(p - src.a, src.wallNormal)) > std::max(2.0, s.edgeWidth))
                    continue;
                glm::dvec3 along = (src.b - src.a) - up * glm::dot(src.b - src.a, up);
                const double len = glm::length(along);
                if (len < 1e-6)
                    continue;
                along /= len;
                const double x = glm::dot(p - src.a, along);
                const double spread = std::max(s.streakSpread, 1e-6);
                if (x < -spread || x > len + spread)
                    continue;
                const double u = std::clamp(x / len, 0.0, 1.0);
                const double drop = heightOf(src.a + (src.b - src.a) * u, up) - heightOf(p, up);
                if (drop < -1e-3 || drop > s.streakLength)
                    continue;
                const double ends = x < 0.0 ? 1.0 + x / spread : x > len ? 1.0 - (x - len) / spread : 1.0;
                streak = std::max(streak, (1.0 - std::max(drop, 0.0) / s.streakLength) * ends);
            }
            // Under an overhang, own or another object's: the first thing straight up is an underside.
            double t;
            glm::dvec3 hn;
            if (sc.cast(o, up, s.streakLength, false, t, hn) && glm::dot(hn, up) < -0.5)
                streak = std::max(streak, 1.0 - t / s.streakLength);
            values[kStreak][i] = float(std::clamp(streak, 0.0, 1.0));
        }

        if (facing < 0.5)
        {
            double h = std::numeric_limits<double>::max();
            double t;
            glm::dvec3 hn;
            if (sc.cast(o, -up, s.fadeHeight, false, t, hn))
                h = t;
            if (s.groundPlane && height > -eps)
                h = std::min(h, std::max(height, 0.0));
            values[kFade][i] = float(std::clamp(1.0 - h / std::max(s.fadeHeight, 1e-6), 0.0, 1.0));
        }
    });

    for (Index c = 0; c < w.cornerCapacity(); ++c)
        if (w.cornerValid(c) && cornerSample[c] != geo::kInvalid)
            for (int k = 0; k < kMaskCount; ++k)
                r.corners[size_t(k)][c] = values[size_t(k)][cornerSample[c]];
    return r;
}

void writeAttributes(geo::Mesh& mesh, const Result& r)
{
    for (int k = 0; k < kMaskCount; ++k)
    {
        auto v = mesh.cornerAttrs().getOrAdd<float>(kCornerAttrs[size_t(k)]);
        const std::vector<float>& src = r.corners[size_t(k)];
        for (size_t c = 0; c < v.size() && c < src.size(); ++c)
            v[c] = src[c];
    }
}
} // namespace weathering::masks

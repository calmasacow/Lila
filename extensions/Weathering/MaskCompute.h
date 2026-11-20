#pragma once

#include <lila/Mesh.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

/**
 * The geometric half of the live grunge masks: per face corner values 0 to 1 found from the mesh and the objects around
 * it, with no UVs and no bake. The look (level, softness, bands, colours) is the material's, per pixel (render::GrungeLook).
 */
namespace weathering::masks
{
enum Mask
{
    kEdge,
    kCavity,
    kAo,
    kDust,
    kStreak,
    kFade,
    kMaskCount,
};

/** The masks' attribute names, render::kGrungeAttrs order. */
inline constexpr std::array<const char*, kMaskCount> kMaskNames = {"edge", "cavity", "ao", "dust", "streak", "fade"};

struct Settings
{
    /** Wear runs this far from a convex edge, cm. */
    double edgeWidth = 4.0;
    /** Edges whose faces turn more than this, degrees, are convex (wear) or concave (dirt) edges. */
    double edgeAngle = 30.0;
    /** Dirt runs this far from a concave edge or a touching object, cm. */
    double cavityWidth = 10.0;
    /** Occlusion rays reach this far, cm. */
    double aoRadius = 80.0;
    int aoRays = 16;
    /** Dust settles on surfaces open to the sky this far up, cm. */
    double dustCover = 1000.0;
    /** Streaks run this far down from a sill or an overhang, cm. */
    double streakLength = 150.0;
    /** Streaks reach this far past the ends of their source edge, cm. */
    double streakSpread = 5.0;
    /** A sill counts as an opening's when something covers it within this height, cm. */
    double coverHeight = 400.0;
    /** Sides fade this high above the ground or the object below, cm. */
    double fadeHeight = 60.0;
    /** The ground plane's height on the up axis, cm, when groundPlane is on. */
    double groundHeight = 0.0;
    bool groundPlane = true;
    glm::dvec3 up{0.0, 1.0, 0.0};
    uint32_t seed = 0;
};

/** A nearby object: its evaluated mesh and world matrix. */
struct Neighbour
{
    const lila::geo::Mesh* mesh = nullptr;
    glm::dmat4 world{1.0};
};

/** Per mask, one value per corner (corner capacity; dead corners 0). */
struct Result
{
    std::array<std::vector<float>, kMaskCount> corners;
    /** Distinct (point, normal) samples evaluated. */
    size_t samples = 0;
};

/** The six masks over a mesh placed at world among its neighbours. Deterministic for a seed. */
Result compute(const lila::geo::Mesh& mesh, const glm::dmat4& world, std::span<const Neighbour> neighbours, const Settings& s);

/** Writes a result as float corner attributes named render::kGrungeAttrs ("grungeEdge", ...). */
void writeAttributes(lila::geo::Mesh& mesh, const Result& r);
} // namespace weathering::masks

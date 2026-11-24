#pragma once

#include "OsmDoc.h"

#include <nlohmann/json.hpp>

#include <glm/glm.hpp>

#include <functional>

#include <string>
#include <vector>

namespace lila::ext
{
class Context;
}

/**
 * The world spec: one JSON that builds a whole open-world map from its parts (World Remix, terrain spec, settlements,
 * Buildings, slot rules, site kits, zones, export) with worldBuild, reads back with worldSpec and is checked as one with
 * worldCheck. A built world is a group node holding the spec it was built from and what each step made.
 */
namespace openworld::world
{
using json = nlohmann::json;

/** The node attribute holding the spec a world was built from. */
inline constexpr const char* kSpecAttr = "worldSpec";
/** The node attribute holding what worldBuild made: terrain, remix, settlement meshes, zone set, site ids, files. */
inline constexpr const char* kBuiltAttr = "worldBuilt";

/** JSON schema of the world spec (lila://schema/world-spec). */
const json& worldSpecSchema();

/**
 * Checks a spec's shape: known top-level keys, the types of each section, settlement and site entries. Throws
 * std::runtime_error naming the first bad field.
 */
void validateSpec(const json& spec);

/** Relative file paths in the spec (remix sources, terrain source file, settlement OSM files) made absolute against dir. */
json resolvePaths(json spec, const std::string& dir);

/**
 * The part of an OSM document inside an area, for one settlement of a larger map: ways with any node inside, the
 * nodes they use, tagged nodes inside, and relations with a member kept (members dropped elsewhere stay out).
 * inside(lat, lon) decides; it is asked once per node, in document order.
 */
OsmDoc clipOsm(const OsmDoc& d, const std::function<bool(const terrain::LatLon&)>& inside);

/** Commands worldBuild, worldSpec, worldCheck, worldMap; resources and the make_open_world prompt. */
void registerWorld(lila::ext::Context& ctx);
/** Level > Open World entries and the toolkit's World group. */
void registerWorldUi(lila::ext::Context& ctx);
} // namespace openworld::world

#pragma once

#include "World.h"

#include <lila/Heightfield.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/** What the world commands share: finding a built world and its parts in the scene, the terrain's heights, output folders. */
namespace openworld::world
{
/** A built world: its node, the spec it was built from and what each step made. */
struct WorldRef
{
    std::string node;
    json spec = json::object();
    json built = json::object();
};

/** The world node a name means (its node, or the world's name), else the only world; nullopt when there is none. */
std::optional<WorldRef> findWorld(const std::string& name);
/** Every world node in the scene. */
std::vector<WorldRef> allWorlds();
/** The node name a world called name is kept on. */
std::string worldNodeName(const std::string& name);

/** Every mesh whose node group makes a heightfield: the terrains, World Remix maps included. */
std::vector<std::string> terrainMeshes();
/** The terrain a world stands on: the built one, else the only terrain; empty when none. */
std::string worldTerrain(const WorldRef* w);
/** A terrain's heightfield as its node group last made it (world XZ, cm), evaluating the scene first; null when none. */
std::shared_ptr<const lila::geo::Heightfield> terrainHeights(const std::string& mesh);

/** Every settlement mesh (meshes with a Settlement modifier). */
std::vector<std::string> settlementMeshes();
/** Every zone set node. */
std::vector<std::string> zoneSetNodes();
/** Every Buildings group (a node holding building meshes): group path -> {building id: edit}, orphan edits included. */
std::vector<std::pair<std::string, json>> buildingEdits();

/** The workspace's openworld/<kind>/<name>, else a temp folder (with a note). */
std::filesystem::path outputFolder(const std::string& kind, const std::string& name, json* notes);

/** The process's working set and peak, MB (0 where the platform can't say). */
double memoryMB();
double peakMemoryMB();

/** worldCheck as a function (the command adds the Validation panel report). */
json checkWorld(const WorldRef* w, const json& options);
/** worldMap as a function. */
json writeWorldMap(const WorldRef* w, const json& options);
/** Registers worldCheck and worldMap (WorldCheck.cpp). */
void registerCheckCommands(lila::ext::Context& ctx);
} // namespace openworld::world

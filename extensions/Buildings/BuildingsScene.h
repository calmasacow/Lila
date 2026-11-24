#pragma once

#include "Buildings.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lila::ext
{
class Context;
}

/** The scene side of Buildings: the style library, the live node and what it published, commands and UI. */
namespace buildings
{
// Style library ---------------------------------------------------------------------------------------------------------

/** Shipped styles (resources/styles), then the user's (<config>/building-styles), then the workspace's (BuildingStyles); later ones of an id win. */
void loadStyles(const std::filesystem::path& shippedDir);
void reloadStyles();
std::shared_ptr<const Style> findStyle(const std::string& id);
std::vector<std::shared_ptr<const Style>> allStyles();
/** Bumped whenever a style is added, saved or reloaded. Any thread. */
uint64_t stylesRevision();
std::filesystem::path userStyleDir();
std::filesystem::path workspaceStyleDir();
/** Writes a style to the user's (scope "user") or workspace's folder and loads it. Returns the file. */
std::filesystem::path saveStyle(const Style& s, const std::string& scope);

// The live node ---------------------------------------------------------------------------------------------------------

/** The geometry node type and its group. */
inline constexpr const char* kNodeType = "Buildings";
inline constexpr const char* kGroupName = "LilaBuildings";
/** Mesh node string attribute marking a building mesh (its kind: "cell" or "standalone") and its settlement. */
inline constexpr const char* kMeshMark = "buildingsNode";
inline constexpr const char* kMeshSettlement = "buildingsSettlement";

/** What a Buildings node made on its last evaluation, kept per modifier for commands, checks and the paint tool. */
struct Published
{
    uint64_t modifier = 0;
    std::string cell, lod;
    std::vector<Spec> specs;
    std::vector<std::shared_ptr<const Result>> results;
    std::map<std::string, int> byId;
    std::vector<std::string> warnings;
    double ms = 0.0;      // last evaluation, milliseconds
    int generated = 0;    // buildings generated (cache misses) in it
    size_t slots = 0, faces = 0;
};
std::shared_ptr<const Published> published(uint64_t modifier);
std::vector<std::shared_ptr<const Published>> allPublished();
void forgetPublished(uint64_t modifier);

void registerNode(lila::ext::Context& ctx);
/** Drops cached generation results (after a style edit). */
void clearGenerationCache();

/** The material name a building face uses in the scene (palette-indexed render, trim, roof). */
std::string materialFor(const Result& r, int mat);
/** Every material name a style's palette can produce, with its linear colour, for creating the Lila materials. */
std::vector<std::pair<std::string, glm::vec3>> styleMaterials(const Style& s);

void registerCommands(lila::ext::Context& ctx);
void registerCheck(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
void registerAgent(lila::ext::Context& ctx);
void registerUnreal(lila::ext::Context& ctx);

/** The shipped LilaBuilding.lilatrim the shell's uv0 follows. */
void setShippedTrimSheet(const std::filesystem::path& p);

/** buildingCheck as a function: issues for the given modifiers (all when empty). */
nlohmann::json checkBuildings(const std::vector<uint64_t>& modifiers, double tolerance);
} // namespace buildings

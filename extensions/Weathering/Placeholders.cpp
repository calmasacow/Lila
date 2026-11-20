#include "Placeholders.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Log.h>
#include <lila/MeshOps.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>

#include <filesystem>
#include <fstream>

using namespace lila;
namespace fs = std::filesystem;

namespace weathering
{
namespace
{
constexpr int kVersion = 1;

std::vector<Placeholder>& registry()
{
    static std::vector<Placeholder> list;
    return list;
}

fs::path libraryDir() { return PrefStore::get().userDir() / "weathering-library"; }

fs::path markerOf(const fs::path& dir) { return dir / "placeholders.json"; }

/** What a built folder must match: the version and every builder's id, so a new builder triggers a rebuild. */
json signature()
{
    json ids = json::array();
    for (const Placeholder& p : registry())
        ids.push_back(p.collection + "/" + p.name);
    return {{"version", kVersion}, {"assets", ids}};
}

bool current(const fs::path& dir)
{
    try
    {
        std::ifstream f(markerOf(dir));
        return f && json::parse(f) == signature();
    }
    catch (const std::exception&)
    {
        return false;
    }
}

glm::dvec3 sizeOf(const geo::Mesh& m)
{
    glm::dvec3 lo(1e300), hi(-1e300);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            lo = glm::min(lo, m.position(p));
            hi = glm::max(hi, m.position(p));
        }
    return lo.x > hi.x ? glm::dvec3(0.0) : hi - lo;
}

void addRoot()
{
    try
    {
        ops::execute("libraryRoot", {{"args", {libraryDir().generic_string()}}, {"tier", "placeholder"}});
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Warning, std::string("Weathering: placeholder library not added: ") + e.what());
    }
}
} // namespace

void addPlaceholder(Placeholder p)
{
    for (Placeholder& x : registry())
        if (x.collection == p.collection && x.name == p.name)
        {
            x = std::move(p);
            return;
        }
    registry().push_back(std::move(p));
}

const std::vector<Placeholder>& placeholders() { return registry(); }

json buildPlaceholders(bool force)
{
    LILA_ZONE();

    const fs::path dir = libraryDir();
    json ids = json::array();
    for (const Placeholder& p : registry())
        ids.push_back(p.collection + "/" + p.name);
    if (!force && current(dir))
        return {{"dir", dir.generic_string()}, {"built", false}, {"assets", ids}};
    std::error_code ec;
    fs::remove_all(dir, ec);
    for (const Placeholder& p : registry())
    {
        const geo::Mesh m = p.build();
        const fs::path folder = dir / p.collection / p.name;
        fs::create_directories(folder, ec);
        std::ofstream(folder / (p.name + ".obj"), std::ios::binary) << geo::writeObj(m);
        json tags = json::array({"weathering", "placeholder", p.role, p.style});
        for (const std::string& t : p.tags)
            tags.push_back(t);
        const glm::dvec3 size = sizeOf(m);
        const json meta = {{"name", p.name},
                           {"model", p.name + ".obj"},
                           {"tags", tags},
                           {"description", p.description},
                           {"size", json::array({size.x, size.y, size.z})},
                           {"physics", {{"role", p.physicsRole}, {"material", p.physicsMaterial}, {"shape", p.physicsShape}}}};
        std::ofstream(folder / "asset.json") << meta.dump(2);
    }
    std::ofstream(markerOf(dir)) << signature().dump(2);
    addRoot();
    try
    {
        ops::execute("libraryReload", json::object());
    }
    catch (const std::exception&)
    {
    }
    log::info("Weathering: placeholder pieces written to {}", dir.generic_string());
    return {{"dir", dir.generic_string()}, {"built", true}, {"assets", ids}};
}

void registerPlaceholderCommands(ext::Context& ctx)
{
    addRoot();
    // Built at load (only when missing or out of date), so presets run from Stagehand's library find them by tag.
    buildPlaceholders(false);
    ctx.registerCommand({
        .name = "weatheringPlaceholders",
        .doc = "Write Weathering's placeholder pieces (rubble, planks, plants, leaves, roof sheets, tiles, frames, insulators, "
               "rooftop units, in realistic and toon versions) into the library's placeholder tier, where every preset finds them "
               "until you tag your own assets with the same roles. Built on first use; force=True rebuilds. Returns dir, built "
               "and assets (ids).",
        .flags = {{"force", "f", ArgType::Bool, false, "Rebuild even when they are up to date."}},
        .undoable = false,
        .run = [](const lila::Args& a) -> json { return buildPlaceholders(a.get<bool>("force", false)); },
        .examples = {"weatheringPlaceholders()", "[a['id'] for a in librarySearch(tags=['weathering', 'rubble'])]"},
    });
}
} // namespace weathering

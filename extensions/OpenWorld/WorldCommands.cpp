// worldBuild and worldSpec: a whole map from one JSON through the parts' own commands (remix, terrain spec, settlements,
// Buildings, site kits, slots, zones), timed and measured per step, and read back from the scene as one spec.

#include "OpenWorld.h"
#include "Remix.h"
#include "RemixSpec.h"
#include "WorldScene.h"
#include "Zones.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mcp.h>
#include <lila/Node.h>
#include <lila/Paths.h>
#include <lila/Profile.h>
#include <lila/Slots.h>
#include <lila/Workspace.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>

using namespace lila;

namespace openworld::world
{
namespace fs = std::filesystem;
using scene::Graph;
using scene::Node;
using T = ArgType;

// Scene helpers ---------------------------------------------------------------------------------------------------------

std::string worldNodeName(const std::string& name) { return name + "_World"; }

namespace
{
json parsed(const json& v, json fallback)
{
    if (!v.is_string() || v.get<std::string>().empty())
        return fallback;
    json j = json::parse(v.get<std::string>(), nullptr, false);
    return j.is_discarded() ? fallback : j;
}

json attrOf(const std::string& node, const char* attr, json fallback = json::object())
{
    Node* n = Graph::current().find(node);
    if (!n || n->type().attrIndex(attr) < 0)
        return fallback;
    return parsed(ops::execute("getAttr", {{"args", {node + "." + attr}}}), std::move(fallback));
}

void setStringAttr(const std::string& node, const char* attr, const std::string& value, const char* doc)
{
    Node* n = Graph::current().find(node);
    if (!n)
        return;
    if (n->type().attrIndex(attr) < 0)
        ops::execute("addAttr", {{"args", {node}}, {"longName", attr}, {"dataType", "string"}, {"doc", doc}});
    ops::execute("setAttr", {{"args", {node + "." + attr, value}}});
}

WorldRef readWorld(const Node& n)
{
    WorldRef w;
    w.node = n.path();
    w.spec = attrOf(w.node, kSpecAttr);
    w.built = attrOf(w.node, kBuiltAttr);
    return w;
}

bool isGroupOf(Graph& g, const Node& mod, std::string_view groupName)
{
    if (!gn::isGnModifier(mod))
        return false;
    Node* grp = gn::groupOfModifier(g, mod);
    return grp && grp->name() == groupName;
}
} // namespace

std::vector<WorldRef> allWorlds()
{
    std::vector<WorldRef> out;
    Graph& g = Graph::current();
    for (scene::NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().attrIndex(kSpecAttr) >= 0)
            out.push_back(readWorld(*n));
    return out;
}

std::optional<WorldRef> findWorld(const std::string& name)
{
    Graph& g = Graph::current();
    if (!name.empty())
    {
        for (const std::string& candidate : {name, worldNodeName(name)})
            if (Node* n = g.find(candidate); n && n->type().attrIndex(kSpecAttr) >= 0)
                return readWorld(*n);
        return std::nullopt;
    }
    std::vector<WorldRef> all = allWorlds();
    if (all.size() == 1)
        return all[0];
    return std::nullopt;
}

std::vector<std::string> terrainMeshes()
{
    std::vector<std::string> out;
    Graph& g = Graph::current();
    for (scene::NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || !gn::isGnModifier(*n) || !gn::modifierHeightfield(id))
            continue;
        const scene::NodeId mesh = scene::modifiers::meshOf(g, id);
        if (mesh != scene::kNoNode && g.node(mesh))
            out.push_back(g.node(mesh)->path());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string worldTerrain(const WorldRef* w)
{
    Graph& g = Graph::current();
    if (w)
        if (const std::string t = w->built.value("terrain", std::string()); !t.empty() && g.find(t))
            return g.find(t)->path();
    g.evaluate();
    const std::vector<std::string> all = terrainMeshes();
    return all.size() == 1 ? all[0] : std::string();
}

std::shared_ptr<const geo::Heightfield> terrainHeights(const std::string& mesh)
{
    Graph& g = Graph::current();
    Node* n = g.find(mesh);
    if (!n)
        return nullptr;
    g.evaluate();
    std::shared_ptr<const geo::Heightfield> found;
    for (scene::NodeId m : scene::modifiers::stack(g, n->id()))
        if (Node* mod = g.node(m); mod && gn::isGnModifier(*mod))
            if (auto h = gn::modifierHeightfield(m))
                found = h;
    return found;
}

std::vector<std::string> settlementMeshes()
{
    std::vector<std::string> out;
    Graph& g = Graph::current();
    for (scene::NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || !gn::isGnModifier(*n) || n->type().attrIndex("walkable") < 0 || n->type().attrIndex("ground") < 0 || n->type().attrIndex("edits") < 0)
            continue;
        const scene::NodeId mesh = scene::modifiers::meshOf(g, id);
        if (mesh != scene::kNoNode && g.node(mesh))
            out.push_back(g.node(mesh)->path());
    }
    return out;
}

std::vector<std::string> zoneSetNodes()
{
    std::vector<std::string> out;
    Graph& g = Graph::current();
    for (scene::NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA("zoneSet"))
            out.push_back(n->path());
    return out;
}

std::vector<std::pair<std::string, json>> buildingEdits()
{
    Graph& g = Graph::current();
    std::map<std::string, json> byGroup;
    for (scene::NodeId id : g.nodes())
    {
        Node* mod = g.node(id);
        if (!mod || !isGroupOf(g, *mod, "LilaBuildings"))
            continue;
        const scene::NodeId mesh = scene::modifiers::meshOf(g, id);
        if (mesh == scene::kNoNode)
            continue;
        const scene::NodeId parent = g.parent(mesh);
        if (parent == scene::kNoNode || !g.node(parent))
            continue;
        json& edits = byGroup[g.node(parent)->path()];
        if (edits.is_null())
            edits = json::object();
        const json own = attrOf(mod->path(), "edits");
        for (auto& [k, v] : own.items())
            edits[k] = v;
    }
    for (auto& [grp, edits] : byGroup)
    {
        const json orphans = attrOf(grp, "buildingsOrphanEdits");
        for (auto& [k, v] : orphans.items())
            if (!edits.contains(k))
                edits[k] = v;
    }
    return {byGroup.begin(), byGroup.end()};
}

fs::path outputFolder(const std::string& kind, const std::string& name, json* notes)
{
    const fs::path& root = Workspace::get().root();
    if (!root.empty())
        return root / "openworld" / kind / name;
    const fs::path tmp = fs::temp_directory_path() / ("lila_" + kind) / name;
    if (notes)
        notes->push_back("no workspace open; files went to " + paths::toUtf8(tmp));
    return tmp;
}

namespace
{
// Build -----------------------------------------------------------------------------------------------------------------

/** One step of a build: what ran, how long, the memory after it and its result (big lists counted) or error. */
class Steps
{
public:
    explicit Steps(bool stopOnError) : mStop(stopOnError) {}

    bool stopped() const { return mStopped; }
    bool failed() const { return mFailed; }
    const json& list() const { return mSteps; }
    std::string firstError() const { return mFirstError; }

    /** Runs fn as step name unless an earlier failure stopped the build; false on failure. */
    bool run(const std::string& name, const std::function<json()>& fn, json* result = nullptr)
    {
        if (mStopped)
            return false;
        LILA_ZONE_N("world.step");
        LILA_ZONE_TEXT(name.c_str(), name.size());
        const auto t0 = std::chrono::steady_clock::now();
        json step = {{"step", name}};
        bool ok = true;
        try
        {
            json r = fn();
            if (result)
                *result = r;
            step["result"] = summarize(r);
        }
        catch (const std::exception& e)
        {
            ok = false;
            step["error"] = e.what();
            if (mFirstError.empty())
                mFirstError = name + ": " + e.what();
            mFailed = true;
            if (mStop)
                mStopped = true;
        }
        step["ok"] = ok;
        step["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        step["memoryMB"] = std::round(memoryMB());
        LILA_PLOT("world.memoryMB", memoryMB());
        if (ok)
            log::info("worldBuild: {} done in {:.1f} s ({:.0f} MB)", name, step["seconds"].get<double>(), step["memoryMB"].get<double>());
        else
            log::warn("worldBuild: {} failed: {}", name, step["error"].get<std::string>());
        mSteps.push_back(std::move(step));
        return ok;
    }

    static json summarize(const json& j, int depth = 0)
    {
        if (j.is_array())
        {
            if (j.size() > 12 || depth > 2)
                return {{"count", j.size()}};
            json out = json::array();
            for (const json& v : j)
                out.push_back(summarize(v, depth + 1));
            return out;
        }
        if (j.is_object())
        {
            if (depth > 3)
                return {{"keys", j.size()}};
            json out = json::object();
            for (const auto& [k, v] : j.items())
                out[k] = summarize(v, depth + 1);
            return out;
        }
        if (j.is_string() && j.get<std::string>().size() > 400)
            return j.get<std::string>().substr(0, 400) + "...";
        return j;
    }

private:
    bool mStop = true, mStopped = false, mFailed = false;
    std::string mFirstError;
    json mSteps = json::array();
};

json run(const char* cmd, json flags) { return ops::execute(cmd, std::move(flags)); }

std::string pathOfResult(const json& made)
{
    if (made.is_string())
        return made.get<std::string>();
    if (made.is_array() && !made.empty() && made[0].is_string())
        return made[0].get<std::string>();
    return {};
}

/** The OSM nodes' world positions on the terrain, through Terrain's own georeference (origin given for a remix). */
std::vector<glm::dvec2> worldPositions(const OsmDoc& d, const std::string& terrain, const json& origin)
{
    std::vector<glm::dvec2> out;
    out.reserve(d.nodes.size());
    const size_t chunk = 20000;
    for (size_t i = 0; i < d.nodes.size(); i += chunk)
    {
        json ll = json::array();
        for (size_t k = i; k < std::min(d.nodes.size(), i + chunk); ++k)
            ll.push_back({d.nodes[k].at.lat, d.nodes[k].at.lon});
        json call = {{"args", {terrain}}, {"latLon", ll}};
        if (!origin.is_null())
            call["origin"] = origin;
        const json r = run("terrainGeoref", call);
        for (const json& p : r.value("points", json::array()))
            out.push_back(p.size() >= 3 ? glm::dvec2(p[0].get<double>(), p[2].get<double>()) : glm::dvec2(p[0].get<double>(), p[1].get<double>()));
    }
    if (out.size() != d.nodes.size())
        throw std::runtime_error("terrainGeoref gave " + std::to_string(out.size()) + " points for " + std::to_string(d.nodes.size()) + " OSM nodes");
    return out;
}

/** Seeds a Buildings group with edits before it is built, so buildingCreate hands them to the buildings they name. */
void seedEdits(const std::string& groupName, const json& edits)
{
    if (!edits.is_object() || edits.empty())
        return;
    Graph& g = Graph::current();
    std::string grp = g.find(groupName) ? g.find(groupName)->path() : std::string();
    if (grp.empty())
        grp = pathOfResult(run("group", {{"empty", true}, {"name", groupName}}));
    json merged = attrOf(grp, "buildingsOrphanEdits");
    for (auto& [k, v] : edits.items())
        merged[k] = v;
    setStringAttr(grp, "buildingsOrphanEdits", merged.dump(), "Buildings edits waiting for the buildings they name.");
}

/** The level of detail a Buildings group's new cells start at (near, hlod or far). */
void seedLod(const std::string& groupName, const std::string& lod)
{
    if (lod != "near" && lod != "hlod" && lod != "far")
        throw std::runtime_error("buildings lod is near, hlod or far, not " + lod);
    Graph& g = Graph::current();
    std::string grp = g.find(groupName) ? g.find(groupName)->path() : std::string();
    if (grp.empty())
        grp = pathOfResult(run("group", {{"empty", true}, {"name", groupName}}));
    setStringAttr(grp, "buildingsLod", lod, "The level of detail new building cells start at.");
}

struct BuildOptions
{
    bool replace = false;
    bool stopOnError = true;
    std::string folder;
    std::vector<std::string> skip;
};

/**
 * Fills what a must-cover layer leaves of the play area with one painted zone, so a district layer made from
 * settlements also covers the countryside between them. The play area is the set's, else the terrain's extent; the
 * zone gets a fictional district name. Returns what was painted.
 */
json fillLayerGaps(const std::string& set, const std::string& terrain, const std::string& layerId)
{
    zones::ZoneSpec spec = zones::zonesFromJson(run("zoneSpec", {{"set", set}}));
    zones::Layer* layer = nullptr;
    for (zones::Layer& l : spec.layers)
        if (l.id == layerId)
            layer = &l;
    if (!layer)
        throw std::runtime_error("no layer " + layerId + " in " + set);
    const std::string zoneId = "outlying";
    std::erase_if(layer->zones, [&](const zones::Zone& z) { return z.id == zoneId; });
    Vec2 lo, hi;
    if (!spec.playArea.empty())
    {
        lo = Vec2(1e300), hi = Vec2(-1e300);
        for (const Polygon& p : spec.playArea)
            for (const Vec2& q : p)
                lo = glm::min(lo, q), hi = glm::max(hi, q);
    }
    else if (auto h = terrainHeights(terrain))
        lo = h->origin, hi = h->origin + h->extent();
    else
        throw std::runtime_error("no play area and no terrain to fill " + layerId + " over");
    const double cell = spec.maskCell;
    const Vec2 origin = glm::floor(lo / cell) * cell;
    const zones::Raster r = zones::rasterize(*layer, origin, hi, cell);
    auto inPlay = [&](Vec2 p) {
        if (spec.playArea.empty())
            return true;
        for (const Polygon& poly : spec.playArea)
        {
            bool in = false;
            for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
                if ((poly[i].y > p.y) != (poly[j].y > p.y) && p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x)
                    in = !in;
            if (in)
                return true;
        }
        return false;
    };
    zones::Zone z;
    z.id = zoneId;
    z.mask.cell = cell;
    const glm::ivec2 base(int(std::lround(origin.x / cell)), int(std::lround(origin.y / cell)));
    size_t painted = 0;
    for (int zz = 0; zz < r.nz; ++zz)
        for (int x = 0; x < r.nx; ++x)
            if (r.winner[size_t(zz) * r.nx + x] < 0 && inPlay(r.at(x, zz)))
            {
                z.mask.setCell(base.x + x, base.y + zz, 255);
                ++painted;
            }
    if (painted == 0)
        return {{"layer", layerId}, {"cells", 0}};
    z.priority = -1;
    try
    {
        const json n = run("fictionalName", {{"culture", spec.culture}, {"kind", "district"}, {"key", set + "/" + layerId + "/" + zoneId}});
        z.name = n.is_string() ? n.get<std::string>() : n.value("name", std::string());
        z.nameSource = "fictionalName";
    }
    catch (const std::exception&)
    {
    }
    if (z.name.empty())
    {
        z.name = "Outlying " + layerId;
        z.nameSource = "given";
    }
    layer->zones.push_back(std::move(z));
    run("zoneSpec", {{"set", set}, {"spec", zones::toJson(spec)}});
    return {{"layer", layerId}, {"zone", zoneId}, {"cells", painted}, {"areaM2", double(painted) * cell * cell * 1e-4}};
}

/** Deletes what an earlier build of this world made, so a rebuild starts clean. */
void removeBuilt(const WorldRef& w)
{
    Graph& g = Graph::current();
    for (const json& s : w.built.value("sites", json::array()))
        if (s.is_string())
            try
            {
                run("siteRemove", {{"args", {s}}});
            }
            catch (const std::exception&)
            {
            }
    std::vector<std::string> gone;
    for (const char* k : {"nodes"})
        for (const json& n : w.built.value(k, json::array()))
            if (n.is_string() && g.find(n.get<std::string>()))
                gone.push_back(g.find(n.get<std::string>())->path());
    if (g.find(w.node))
        gone.push_back(w.node);
    // A parent and its child both listed: delete the parents only.
    std::sort(gone.begin(), gone.end());
    gone.erase(std::unique(gone.begin(), gone.end()), gone.end());
    std::vector<std::string> roots;
    for (const std::string& p : gone)
        if (std::none_of(roots.begin(), roots.end(), [&](const std::string& r) { return p.size() > r.size() && p.compare(0, r.size(), r) == 0 && p[r.size()] == '|'; }))
            roots.push_back(p);
    if (!roots.empty())
        run("delete", {{"args", roots}});
}

json buildWorld(json spec, const BuildOptions& o)
{
    LILA_ZONE();
    try
    {
        validateSpec(spec);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("worldBuild: ") + e.what());
    }
    Graph& g = Graph::current();
    const std::string name = spec.value("name", std::string("World"));
    const std::string culture = spec.value("culture", std::string("mexican"));
    if (std::optional<WorldRef> old = findWorld(name))
    {
        if (!o.replace)
            throw CommandError("worldBuild: a world named " + name + " exists (" + old->node + "); replace=True rebuilds it");
        removeBuilt(*old);
    }
    auto skipped = [&](const char* step) { return std::find(o.skip.begin(), o.skip.end(), step) != o.skip.end(); };
    json notes = json::array();
    const fs::path folder = o.folder.empty() ? outputFolder("world", name, &notes) : paths::fromUtf8(o.folder);
    std::error_code ec;
    fs::create_directories(folder, ec);

    const auto t0 = std::chrono::steady_clock::now();
    const double mem0 = memoryMB();
    Steps st(o.stopOnError);
    json built = {{"nodes", json::array()}, {"settlements", json::array()}, {"sites", json::array()}, {"buildingGroups", json::array()}};
    auto made = [&](const std::string& path) {
        if (!path.empty())
            built["nodes"].push_back(path);
    };

    // Rules and overrides first, so every generator's slots resolve through them as they are made.
    if (spec.contains("slots") && !skipped("slots"))
        st.run("slots", [&] {
            const json& sl = spec["slots"];
            std::vector<slots::Rule> rules;
            for (const json& r : sl.value("rules", json::array()))
                rules.push_back(slots::Rule::fromJson(r));
            slots::setRules(std::move(rules));
            int overrides = 0;
            const json overrideList = sl.value("overrides", json::object());
            for (const auto& [id, p] : overrideList.items())
            {
                slots::setOverride(id, slots::Payload::fromJson(p), true);
                ++overrides;
            }
            return json{{"rules", slots::state()->rules.size()}, {"overrides", overrides}};
        });

    // Terrain: the remix makes it (and the remixed OSM), the terrain spec's steps run on it; without a remix the spec makes it.
    std::string terrain, remixOsm;
    json origin;
    std::shared_ptr<const RemixResult> remixResult;
    if (spec.contains("remix") && !spec["remix"].is_null())
    {
        json r;
        if (st.run("remix", [&] { return run("remixCreate", {{"name", name}, {"spec", spec["remix"]}}); }, &r))
        {
            terrain = r.value("mesh", std::string());
            made(terrain);
            built["remix"] = terrain;
            built["terrain"] = terrain;
            json applied;
            if (st.run("remix.apply", [&] { return run("remixApply", {{"args", {terrain}}, {"folder", paths::toUtf8(folder / "remix")}}); }, &applied))
            {
                remixOsm = applied["files"].value("osm", std::string());
                origin = applied.value("origin", json());
                built["remixOsm"] = remixOsm;
                built["origin"] = origin;
                if (applied.contains("features") && applied["features"].contains("group"))
                    built["remixFeatures"] = applied["features"]["group"];
            }
        }
        if (!terrain.empty() && spec.contains("terrain") && spec["terrain"].is_object() && !skipped("terrain"))
            st.run("terrain", [&] {
                json t = spec["terrain"];
                t["name"] = g.find(terrain)->name();
                t["source"] = {{"type", "existing"}, {"mesh", terrain}};
                if (!t.contains("check"))
                    t["check"] = false;
                json r = run("terrainSpec", {{"spec", t}, {"stopOnError", o.stopOnError}});
                if (!r.value("ok", false) && r.contains("failed"))
                    throw std::runtime_error(r["failed"].dump());
                return r;
            });
    }
    else
    {
        json r;
        if (st.run("terrain", [&] {
                json t = spec["terrain"];
                if (!t.contains("name"))
                    t["name"] = name;
                if (!t.contains("check"))
                    t["check"] = false;
                json res = run("terrainSpec", {{"spec", t}, {"replace", o.replace}, {"stopOnError", o.stopOnError}});
                if (!res.value("ok", false) && res.contains("failed"))
                    throw std::runtime_error(res["failed"].dump());
                return res;
            }, &r))
        {
            terrain = r.value("terrain", std::string());
            made(terrain);
            built["terrain"] = terrain;
        }
    }
    if (terrain.empty() && !st.stopped())
        st.run("terrain", [] () -> json { throw std::runtime_error("no terrain was made"); });

    // Site kits first: their grading settles the ground the settlements are laid on, and their outlines keep the
    // settlements clear. Their buildings come after the settlements, in one Buildings pass.
    json siteIds = json::array();
    json siteOutlines = json::array();
    for (const json& s : spec.value("sites", json::array()))
    {
        if (skipped("sites") || terrain.empty())
            break;
        const std::string id = s.value("id", std::string());
        json made1;
        if (!st.run("site." + (id.empty() ? s["kind"].get<std::string>() : id), [&] {
                // Graded all together after the loop: one terrain change instead of one per site.
                json call = {{"kind", s["kind"]}, {"terrain", terrain}, {"culture", s.value("culture", culture)}, {"ground", false}};
                for (const char* k : {"id", "name", "seed", "at", "heading", "waterLevel", "params"})
                    if (s.contains(k) && !s[k].is_null())
                        call[k] = s[k];
                return run("siteCreate", call);
            }, &made1))
            continue;
        const std::string sid = made1.value("id", id);
        built["sites"].push_back(sid);
        try
        {
            const json info = run("siteInfo", {{"args", {sid}}});
            if (info.contains("boundary") && info["boundary"].size() >= 3)
                siteOutlines.push_back(info["boundary"]);
        }
        catch (const std::exception&)
        {
        }
        if (s.value("buildings", true))
            siteIds.push_back(sid);
    }
    if (!built["sites"].empty())
        st.run("sites.grade", [&] { return run("siteGround", {{"sites", built["sites"]}}); });

    // Settlements: each from OSM (the remix's or a file, clipped to its area) or laid out by Settlement, then its buildings.
    const json editsByGroup = spec.value("buildings", json::object()).value("edits", json::object());
    std::optional<OsmDoc> remixDoc;
    std::vector<glm::dvec2> remixWorld;
    // Site outlines a settlement must keep clear: only those reaching into its box (Settlement makes every exclusion a
    // district of its own, so far ones would turn up as stray districts).
    auto outlinesIn = [&](double x0, double z0, double x1, double z1) {
        json out = json::array();
        for (const json& o : siteOutlines)
        {
            double lx = 1e300, lz = 1e300, hx = -1e300, hz = -1e300;
            for (const json& p : o)
                lx = std::min(lx, p[0].get<double>()), hx = std::max(hx, p[0].get<double>()), lz = std::min(lz, p[1].get<double>()), hz = std::max(hz, p[1].get<double>());
            if (hx >= x0 && lx <= x1 && hz >= z0 && lz <= z1)
                out.push_back(o);
        }
        return out;
    };
    for (const json& s : spec.value("settlements", json::array()))
    {
        if (skipped("settlements") || terrain.empty())
            break;
        const std::string sname = s["name"].get<std::string>();
        json made1;
        const bool ok = st.run("settlement." + sname, [&] {
            if (Node* old = g.find(sname); old && o.replace)
                run("delete", {{"args", {old->path()}}});
            if (s.contains("create"))
            {
                json call = s["create"];
                call["name"] = sname;
                if (call.contains("outline") && call["outline"].is_array())
                {
                    double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
                    for (const json& p : call["outline"])
                        x0 = std::min(x0, p[0].get<double>()), x1 = std::max(x1, p[0].get<double>()), z0 = std::min(z0, p[1].get<double>()), z1 = std::max(z1, p[1].get<double>());
                    json ex = call.value("exclusions", json::array());
                    for (const json& outline : outlinesIn(x0, z0, x1, z1))
                        ex.push_back(outline);
                    if (!ex.empty())
                        call["exclusions"] = ex;
                }
                if (!call.contains("ground"))
                    call["ground"] = terrain;
                return run("settlementCreate", call);
            }
            std::string file = s["osm"].get<std::string>() == "remix" ? remixOsm : s["osm"].get<std::string>();
            if (file.empty())
                throw std::runtime_error("osm 'remix' but the remix wrote no OSM");
            if (s.contains("area"))
            {
                const json& a = s["area"];
                const double x0 = a[0].get<double>(), z0 = a[1].get<double>(), x1 = a[2].get<double>(), z1 = a[3].get<double>();
                OsmDoc local;
                std::vector<glm::dvec2> world;
                if (file == remixOsm)
                {
                    if (!remixDoc)
                    {
                        remixDoc = readOsmDoc(paths::fromUtf8(remixOsm));
                        remixWorld = worldPositions(*remixDoc, terrain, origin);
                    }
                    local = *remixDoc;
                    world = remixWorld;
                }
                else
                {
                    local = readOsmDoc(paths::fromUtf8(file));
                    world = worldPositions(local, terrain, origin);
                }
                const OsmDoc clipped = clipOsm(local, world, {x0, z0}, {x1, z1});
                const fs::path f = folder / "settlements" / (sname + ".osm.json");
                fs::create_directories(f.parent_path());
                std::ofstream(f, std::ios::binary) << writeOsmDocJson(clipped);
                file = paths::toUtf8(f);
            }
            json call = {{"args", {file}}, {"ground", terrain}, {"name", sname}, {"culture", s.value("culture", culture)}};
            if (!origin.is_null())
                call["origin"] = origin;
            if (s.contains("area"))
            {
                const json near = outlinesIn(s["area"][0].get<double>(), s["area"][1].get<double>(), s["area"][2].get<double>(), s["area"][3].get<double>());
                if (!near.empty())
                    call["exclusions"] = near;
            }
            for (const char* k : {"profile", "fill", "coverage", "margin"})
                if (s.contains(k))
                    call[k] = s[k];
            return run("settlementFromOsm", call);
        }, &made1);
        if (!ok)
            continue;
        const std::string mesh = made1.value("mesh", sname);
        made(mesh);
        built["settlements"].push_back(mesh);
        if (made1.contains("import") && made1["import"].contains("curves") && made1["import"]["curves"].contains("group"))
            made(made1["import"]["curves"]["group"].get<std::string>());
        const json b = s.value("buildings", json(false));
        if (!(b.is_object() || (b.is_boolean() && b.get<bool>())) || skipped("buildings"))
            continue;
        const std::string groupName = g.find(mesh) ? g.find(mesh)->name() + "_Buildings" : sname + "_Buildings";
        st.run("buildings." + sname, [&] {
            seedEdits(groupName, editsByGroup.value(groupName, json::object()));
            const std::string lod = b.is_object() && b.contains("lod") ? b["lod"].get<std::string>() : spec.value("buildings", json::object()).value("lod", std::string());
            if (!lod.empty())
                seedLod(groupName, lod);
            json call = {{"args", {mesh}}, {"route", "buildings"}};
            if (b.is_object())
            {
                if (b.contains("style"))
                    call["style"] = b["style"];
                if (b.contains("cell"))
                    call["cell"] = b["cell"];
            }
            json r = run("settlementBuildings", call);
            json out = r.value("buildings", r);
            if (b.is_object() && b.contains("districts") && b["districts"].is_object() && !b["districts"].empty())
            {
                const json names = run("settlementNames", {{"args", {mesh}}});
                json styled = json::object();
                for (const json& d : names.value("districts", json::array()))
                {
                    const std::string dn = d.value("name", std::string());
                    const std::string key = b["districts"].contains(dn) ? dn : std::to_string(d.value("id", -1));
                    if (!b["districts"].contains(key) || !d.contains("outline"))
                        continue;
                    run("buildingStyle", {{"style", b["districts"][key]}, {"polygon", d["outline"]}});
                    styled[dn] = b["districts"][key];
                }
                out["districtStyles"] = styled;
            }
            return out;
        });
        if (Node* grp = g.find(groupName))
        {
            built["buildingGroups"].push_back(grp->path());
            made(grp->path());
        }
        if (s.contains("furniture") && !(s["furniture"].is_boolean() && !s["furniture"].get<bool>()))
            st.run("furniture." + sname, [&] {
                json call = s["furniture"].is_object() ? s["furniture"] : json::object();
                call["args"] = {mesh};
                return run("settlementFurniture", call);
            });
    }

    if (!siteIds.empty() && !skipped("buildings") && ops::find("buildingCreate"))
        st.run("buildings.sites", [&] {
            seedEdits("SiteBuildings", editsByGroup.value("SiteBuildings", json::object()));
            if (const std::string lod = spec.value("buildings", json::object()).value("lod", std::string()); !lod.empty())
                seedLod("SiteBuildings", lod);
            json call = {{"args", siteIds}};
            json r = run("siteBuildings", call);
            if (r.contains("group") && r["group"].is_string())
            {
                built["buildingGroups"].push_back(r["group"]);
                made(r["group"].get<std::string>());
            }
            return r;
        });
    else if (!siteIds.empty() && !ops::find("buildingCreate"))
        notes.push_back("the Buildings extension is not loaded: site buildings stay greybox massing");

    // Style calls over everything built.
    for (const json& c : spec.value("buildings", json::object()).value("styles", json::array()))
        if (!skipped("buildings"))
            st.run("buildings.style", [&] { return run("buildingStyle", c); });

    // Zones: the set's spec, then the generators (districts, water, terrain masks), then the play area.
    if (spec.contains("zones") && spec["zones"].is_object() && !skipped("zones") && !terrain.empty())
    {
        const json& z = spec["zones"];
        const std::string setName = z.value("name", std::string("Zones"));
        auto setFlags = [&](json call) {
            if (Node* n = g.find(setName))
                call["set"] = n->path();
            else
            {
                call["setName"] = setName;
                call["ground"] = terrain;
            }
            return call;
        };
        json spec1 = z.value("spec", json());
        if (spec1.is_object() && !spec1.contains("culture"))
            spec1["culture"] = culture;
        if (spec1.is_object())
            st.run("zones.spec", [&] { return run("zoneSpec", setFlags({{"spec", spec1}})); });
        else
            st.run("zones.set", [&] { return run("zoneSpec", setFlags({{"spec", {{"culture", culture}, {"layers", json::array()}}}})); });
        const json fd = z.value("fromDistricts", json(false));
        if (fd.is_array() || (fd.is_boolean() && fd.get<bool>()))
            for (const json& sm : built["settlements"])
            {
                const std::string sn = g.find(sm.get<std::string>()) ? g.find(sm.get<std::string>())->name() : sm.get<std::string>();
                if (fd.is_array() && std::find(fd.begin(), fd.end(), json(sn)) == fd.end())
                    continue;
                st.run("zones.districts." + sn, [&] { return run("zoneFromDistricts", setFlags({{"settlement", sm}})); });
            }
        json fw = z.value("fromWater", json(false));
        if (fw.is_boolean())
            fw = fw.get<bool>() ? json::array({json::object()}) : json::array();
        else if (fw.is_object())
            fw = json::array({fw});
        for (const json& c : fw)
            st.run("zones.water", [&] {
                json call = setFlags(c);
                call["terrain"] = terrain;
                return run("zoneFromWater", call);
            });
        for (const json& c : z.value("fromTerrain", json::array()))
            st.run("zones.terrain", [&] {
                json call = setFlags(c);
                call["terrain"] = terrain;
                return run("zoneFromTerrain", call);
            });
        if (z.contains("playArea"))
            st.run("zones.playArea", [&] { return run("zonePlayArea", setFlags({{"polygon", z["playArea"]}})); });
        // Must-cover layers (districts, police) filled out to the play area unless the spec says otherwise.
        const json fg = z.value("fillGaps", json(true));
        if (Node* setNode = g.find(setName); setNode && !(fg.is_boolean() && !fg.get<bool>()))
        {
            const zones::ZoneSpec now = zones::zonesFromJson(run("zoneSpec", {{"set", setNode->path()}}));
            for (const zones::Layer& l : now.layers)
                if (l.mustCover && (fg.is_boolean() || std::find(fg.begin(), fg.end(), json(l.id)) != fg.end()))
                    st.run("zones.fill." + l.id, [&] { return fillLayerGaps(setNode->path(), terrain, l.id); });
        }
        if (Node* n = g.find(setName))
        {
            built["zones"] = n->path();
            made(n->path());
        }
    }

    // Pins last: they freeze what the build made.
    if (spec.contains("slots") && spec["slots"].contains("pins") && !skipped("slots"))
        st.run("slots.pins", [&] {
            int n = 0;
            json missed = json::array();
            for (const json& k : spec["slots"]["pins"])
                if (k.is_string())
                {
                    if (slots::pin(g, k.get<std::string>()))
                        ++n;
                    else
                        missed.push_back(k);
                }
            return json{{"pinned", n}, {"nothingMatched", missed}};
        });

    // The world node: the spec it was built from and what was made, for worldSpec, worldCheck and a rebuild.
    const std::string node = pathOfResult(run("group", {{"empty", true}, {"name", worldNodeName(name)}}));
    built["folder"] = paths::toUtf8(folder);
    setStringAttr(node, kSpecAttr, spec.dump(), "The world spec this world was built from (worldSpec reads it back).");
    setStringAttr(node, kBuiltAttr, built.dump(), "What worldBuild made: terrain, remix, settlements, sites, zones, building groups, files.");
    WorldRef w{node, spec, built};

    json check;
    const json checkFlags = spec.value("check", json(true));
    if (!st.stopped() && !(checkFlags.is_boolean() && !checkFlags.get<bool>()))
        st.run("check", [&] { return checkWorld(&w, checkFlags.is_object() ? checkFlags : json::object()); }, &check);

    json exports = json::object();
    const json ex = spec.value("export", json::object());
    const std::string target = ex.value("target", std::string("none"));
    if (target != "none" && !st.stopped())
    {
        const fs::path out = ex.contains("folder") ? paths::fromUtf8(ex["folder"].get<std::string>()) : folder / "export";
        st.run("export.terrain", [&] {
            json r = run("terrainExport", {{"args", {terrain}}, {"target", target == "unreal" ? "unreal" : "files"}, {"folder", paths::toUtf8(out / "terrain")}});
            exports["terrain"] = r.value("files", json());
            return r;
        });
        if (built.contains("zones"))
            st.run("export.zones", [&] {
                json r = run("zoneExport", {{"set", built["zones"]}, {"folder", paths::toUtf8(out / "zones")}});
                exports["zones"] = r.value("json", json());
                return r;
            });
        const json mapFlags = ex.value("map", json(true));
        if (!(mapFlags.is_boolean() && !mapFlags.get<bool>()))
            st.run("export.map", [&] {
                json f = mapFlags.is_object() ? mapFlags : json::object();
                f["folder"] = paths::toUtf8(out / "map");
                json r = writeWorldMap(&w, f);
                exports["map"] = {{"image", r["image"]}, {"json", r["json"]}};
                return r;
            });
        if (target == "unreal")
        {
            for (const json& grp : built["buildingGroups"])
                st.run("export.buildings", [&] { return run("buildingSend", {{"args", {grp}}}); });
            if (built.contains("zones"))
                st.run("export.zonesToUnreal", [&] { return run("zoneSendToUnreal", {{"set", built["zones"]}}); });
        }
    }

    json out = {{"world", node},
                {"terrain", terrain},
                {"steps", st.list()},
                {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()},
                {"memoryMB", {{"start", std::round(mem0)}, {"end", std::round(memoryMB())}, {"peak", std::round(peakMemoryMB())}}},
                {"built", built},
                {"notes", notes}};
    if (!check.is_null())
        out["check"] = {{"ok", check.value("ok", false)}, {"counts", check.value("counts", json::object())}, {"issues", Steps::summarize(check.value("issues", json::array()))}};
    if (!exports.empty())
        out["exports"] = exports;
    out["ok"] = !st.failed() && (check.is_null() || check.value("ok", false));
    if (st.failed())
        out["failed"] = st.firstError();
    return out;
}

// Read back -------------------------------------------------------------------------------------------------------------

json readBack(const std::optional<WorldRef>& w)
{
    Graph& g = Graph::current();
    json spec = w ? w->spec : json::object();
    json from = json::object();
    json notes = json::array();
    if (!w)
        notes.push_back("no world node in the scene: the spec is read from the parts alone (settlements and export are not known)");
    if (!spec.contains("name"))
        spec["name"] = "World";

    const std::string remix = w ? w->built.value("remix", std::string()) : std::string();
    if (!remix.empty() && g.find(remix))
    {
        spec["remix"] = run("remixSpec", {{"args", {remix}}});
        from["remix"] = "scene";
    }
    const std::string terrain = worldTerrain(w ? &*w : nullptr);
    if (!terrain.empty())
        try
        {
            const json t = run("terrainSpec", {{"args", {terrain}}});
            if (t.value("from", std::string()) == "spec")
            {
                json ts = t["spec"];
                if (!remix.empty())
                    ts.erase("source");
                spec["terrain"] = ts;
                from["terrain"] = "scene";
            }
            else
            {
                from["terrain"] = "stored";
                notes.push_back("the terrain changed after it was built from its spec; terrainSpec('" + terrain + "') gives its node group");
            }
        }
        catch (const std::exception& e)
        {
            notes.push_back(std::string("terrain not read: ") + e.what());
        }

    // Slots: what the scene holds now.
    const slots::StatePtr st = slots::state();
    json rules = json::array(), overrides = json::object(), pins = json::array();
    for (const slots::Rule& r : st->rules)
        rules.push_back(r.toJson());
    for (const auto& [id, ov] : st->overrides)
        overrides[id] = ov.payload.toJson();
    for (const auto& [k, p] : st->pins)
        pins.push_back(k);
    if (!rules.empty() || !overrides.empty() || !pins.empty() || spec.contains("slots"))
    {
        spec["slots"] = {{"rules", rules}, {"overrides", overrides}, {"pins", pins}};
        from["slots"] = "scene";
    }

    // Sites as they stand, with whether the Buildings generator builds them.
    if (ops::find("siteList"))
    {
        json sites = json::array();
        for (const json& s : run("siteList", json::object()))
        {
            json info = run("siteInfo", {{"args", {s["mesh"]}}});
            json e = info.value("spec", json::object());
            e["buildings"] = false;
            if (Node* mesh = g.find(s.value("mesh", std::string())))
                for (scene::NodeId m : scene::modifiers::stack(g, mesh->id()))
                    if (Node* mod = g.node(m); mod && mod->type().attrIndex("massing") >= 0 && mod->type().attrIndex("siteSpec") >= 0)
                    {
                        const json massing = run("getAttr", {{"args", {mod->path() + ".massing"}}});
                        e["buildings"] = massing.is_boolean() && !massing.get<bool>();
                    }
            sites.push_back(std::move(e));
        }
        if (!sites.empty() || spec.contains("sites"))
        {
            spec["sites"] = sites;
            from["sites"] = "scene";
        }
    }

    // Zones: the set's spec; the generators stay as stored (re-running them replaces only what they made).
    const std::string zset = w && w->built.contains("zones") ? w->built["zones"].get<std::string>() : (zoneSetNodes().size() == 1 ? zoneSetNodes()[0] : std::string());
    if (!zset.empty() && g.find(zset))
    {
        json z = spec.value("zones", json::object());
        z["name"] = g.find(zset)->name();
        z["spec"] = run("zoneSpec", {{"set", zset}});
        spec["zones"] = z;
        from["zones"] = "scene";
    }

    // Buildings edits by group.
    json edits = json::object();
    for (const auto& [grp, e] : buildingEdits())
        if (!e.empty() && g.find(grp))
            edits[g.find(grp)->name()] = e;
    if (!edits.empty() || spec.contains("buildings"))
    {
        json b = spec.value("buildings", json::object());
        b["edits"] = edits;
        spec["buildings"] = b;
        from["buildings"] = "scene";
    }
    for (const char* k : {"settlements", "export", "check", "culture"})
        if (spec.contains(k))
            from[k] = "stored";
    return {{"spec", spec}, {"from", from}, {"notes", notes}, {"world", w ? json(w->node) : json()}};
}

json loadSpecFile(const std::string& file, const char* cmd)
{
    const fs::path p = paths::resolvePath(file);
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw CommandError(std::string(cmd) + ": can't read " + paths::toUtf8(p));
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    json j = json::parse(text, nullptr, false, true);
    if (j.is_discarded())
        throw CommandError(std::string(cmd) + ": " + paths::toUtf8(p) + " is not JSON");
    return resolvePaths(std::move(j), paths::toUtf8(p.parent_path()));
}

std::string worldArg(const Args& a) { return a.positional.empty() || !a.positional[0].is_string() ? std::string() : a.positional[0].get<std::string>(); }

const char* kGuide = R"(# Open World in Lila

A whole open-world map from one JSON: a World Remix of real places (fictional names everywhere), the terrain with its coast and water, settlements from the remixed OpenStreetMap with procedural buildings, site kits (ports, cemeteries, airstrips, farms, villages, compounds, islands, lighthouses), slot rules that swap every placed part, and zones for gameplay. Lengths cm, Y up, positions [x, z].

## Recipe

1. Read lila://schema/world-spec (and the section schemas it names: lila://schema/remix-spec, lila://schema/terrain-spec, lila://schema/site-kits, lila://schema/zones).
2. Write the spec. Start small: one remix source, one settlement, two sites. `worldSpec(spec=s)` checks it without building.
3. Build: `r = worldBuild(spec=s)` (or `worldBuild(file='world.json')`, relative paths from the file's folder). Read `r['steps']`: every step's ok, seconds, memoryMB and error. `replace=True` rebuilds a world of the same name.
4. Check: `c = worldCheck(report=True)`. Fix by kind (below) and rebuild, until `c['ok']`. `c['placeholders']` is swap progress: slots still on placeholders by type.
5. Overview map: `worldMap(resolution=2048)` writes a PNG and JSON (districts, zones, settlements, POIs, roads, water) for an in-game map.
6. Read back after hand edits: `worldSpec(file='world.json')` writes the spec the scene now holds (remix, terrain, slots, sites, zones and Buildings edits from the scene; settlements and export as built).

## Sections in build order

- `slots`: rules (`slotRule(rule=)` dicts), overrides `{slot id: payload}`, pins (applied last).
- `remix`: makes the terrain and the remixed OSM; `terrain` steps (water, coast, biomes, groundCover) then run on it.
- `settlements`: `{name, osm: 'remix', area: [x0, z0, x1, z1], profile, fill, buildings: {style, districts: {name: style}}, furniture}` or `{name, create: {settlementCreate flags}}`.
- `sites`: `{kind, id, at: [x, z], heading, params, buildings: true}`; site buildings come from the Buildings generator (group SiteBuildings).
- `buildings`: `edits` `{group: {building id: edit}}` (seeded before each group builds) and `styles` (buildingStyle calls).
- `zones`: `{name, spec, fromDistricts: true, fromWater: true, playArea}`.
- `export`: `{target: files | unreal, map: true}`.

## worldCheck kinds

- realName: output text matching a real name (imported or the brand blocklist). Rename it (siteEdit name=, zoneEdit name='', settlementNames) or let the generator pick again.
- terrain / water / unreachableWater: terrainCheck and waterCheck (channels too shallow or narrow, lanes disconnected, harbours unreachable).
- remix: rivers that no longer drain, dangling roads, coast gaps, folds.
- settlement, building, site, zone (gaps, overlaps, numbered names), slots (orphans, duplicates).
- buildingOverWater / buildingOverRoad: a building's footprint on water or a road surface.
- propFloating / propBuried: a ground slot off the terrain by more than the tolerance.
- buildingsStale: a settlement changed after its buildings were made; `buildingRefresh()` rebuilds them keeping edits.
- interiorNoEntry: an enterable building without an entry (from buildingCheck).
- placeholders (info): slots still drawn as placeholders, by type.

## Settlement changes

Settlement edits do not rebuild buildings live. `buildingRefresh()` (Level > Buildings > Refresh Buildings) rebuilds every group whose settlement changed; edits, overrides and pins carry over by building id. worldCheck lists stale groups.
)";

const char* kPrompt = R"(Build an open-world map in Lila from this description, check it and export it.

Description: {{description}}
Size: {{size}}. Real places to remix: {{places}}. Target engine: {{engine}}.

1. Read lila://guide/open-world, then lila://schema/world-spec and the section schemas it names.
2. Real data: remixSource(fetch=True) downloads DEM, OSM and land cover for a bbox and shows the size first; ask the user before any large download. Without data, build from files the user gives or a noise terrain.
3. Write one world spec: remix (sources placed, the inland city cut and moved toward the coast), terrain steps (sea, coast: bathymetry, masks, mangroves, channels, reservoirs, routes, airstrips; biomes, groundCover), settlements with osm 'remix' and an area each, buildings with styles, sites (port, cruise, lighthouse, cemetery, airstrip, farm, village, compound, island), slot rules, zones (fromDistricts, fromWater).
4. Check the spec with worldSpec(spec=s), then r = worldBuild(spec=s). Read every step in r['steps']; fix and rebuild with replace=True.
5. c = worldCheck(report=True). Fix each kind (lila://guide/open-world lists them) until c['ok']. Report c['placeholders'] as swap progress.
6. worldMap(resolution=2048) for the overview, then export={'target': '{{engine}}'} in the spec (or terrainExport, zoneExport, buildingSend).
7. Report the steps' times and memory, the check, the files and what is still on placeholders.
)";

std::string fillPrompt(std::string text, const json& args)
{
    const std::pair<const char*, const char*> keys[] = {{"description", "a coastal crime-game region"},
                                                        {"size", "about 12 km by 12 km of land plus sea"},
                                                        {"places", "none given (ask, or build from noise)"},
                                                        {"engine", "unreal"}};
    for (const auto& [key, fallback] : keys)
    {
        const char* argName = std::string(key) == "engine" ? "target" : key;
        const std::string value = args.contains(argName) && args[argName].is_string() && !args[argName].get<std::string>().empty() ? args[argName].get<std::string>() : fallback;
        const std::string tag = std::string("{{") + key + "}}";
        for (size_t at = text.find(tag); at != std::string::npos; at = text.find(tag, at + value.size()))
            text.replace(at, tag.size(), value);
    }
    return text;
}
} // namespace

void registerWorld(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "worldBuild",
        .doc = "Build a whole open-world map from one world spec (lila://schema/world-spec): slot rules and overrides, the World "
               "Remix and its remixed OSM, the terrain spec's steps (water, coast, biomes, ground cover), settlements from the "
               "remix's OSM clipped to each one's area (or laid out by Settlement), their procedural buildings with style "
               "assignments and edits, site kits with their buildings, zones (spec, districts, water), pins, worldCheck and the "
               "export. Each step is timed and measured (seconds, memoryMB) in a Tracy zone. The world node keeps the spec and "
               "what was made, so worldSpec reads it back and replace=True rebuilds it. Returns ok, every step's result or error, "
               "the check, the exports, total time and memory.",
        .flags = {{"spec", "s", T::Any, nullptr, "The world spec, a dict."},
                  {"file", "f", T::String, nullptr, "A world spec JSON file; relative paths in it are from its folder."},
                  {"replace", "r", T::Bool, false, "Delete an earlier build of this world first."},
                  {"stopOnError", "soe", T::Bool, true, "Stop at the first failed step."},
                  {"folder", "fo", T::String, nullptr, "Where files go; default the workspace's openworld/world/<name>."},
                  {"skip", "sk", T::List, nullptr, "Sections to leave out: slots, terrain, settlements, buildings, sites, zones."},
                  {"check", "ch", T::Bool, nullptr, "Run worldCheck at the end (default the spec's check, else true)."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            json spec;
            if (a.has("file"))
                spec = loadSpecFile(a.get<std::string>("file"), "worldBuild");
            else if (a.has("spec"))
                spec = a.flags["spec"].is_string() ? json::parse(a.get<std::string>("spec")) : a.flags["spec"];
            else
                throw CommandError("worldBuild: give spec= or file=");
            if (a.has("check"))
                spec["check"] = a.get<bool>("check");
            BuildOptions o;
            o.replace = a.get<bool>("replace", false);
            o.stopOnError = a.get<bool>("stopOnError", true);
            o.folder = a.get<std::string>("folder", "");
            if (a.has("skip"))
                for (const json& k : a.flags["skip"])
                    o.skip.push_back(k.get<std::string>());
            return buildWorld(std::move(spec), o);
        },
        .examples = {"r = worldBuild(file='worlds/sinaloa.json')\nr['ok'], [(s['step'], s['seconds'], s['memoryMB']) for s in r['steps']]",
                     "worldBuild(spec={'name': 'Tiny', 'terrain': {'size': [400000, 400000], 'resolution': 257, 'source': {'type': 'noise'}},\n"
                     "    'sites': [{'kind': 'village', 'id': 'v1', 'at': [0, 0]}]}, replace=True)"},
    });

    ctx.registerCommand({
        .name = "worldSpec",
        .doc = "Read a built world back as one world spec, or check a spec without building it. With a world (or the only one): the "
               "spec it was built from with the remix, terrain, slot rules/overrides/pins, sites, zones and Buildings edits as the "
               "scene holds them now (from says which sections came from the scene and which are as built); file= writes it. "
               "With spec= or specFile=: checks the spec's shape and returns it with relative paths resolved. Returns spec, from "
               "and notes.",
        .positionalDoc = "world node or name (default the only world)",
        .flags = {{"spec", "s", T::Any, nullptr, "A spec to check."},
                  {"specFile", "sf", T::String, nullptr, "A spec file to check."},
                  {"file", "f", T::String, nullptr, "Write the read-back spec to this JSON file."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const char* cmd = "worldSpec";
            if (a.has("spec") || a.has("specFile"))
            {
                json spec = a.has("specFile") ? loadSpecFile(a.get<std::string>("specFile"), cmd) : a.flags["spec"];
                try
                {
                    validateSpec(spec);
                }
                catch (const std::exception& e)
                {
                    return {{"ok", false}, {"error", e.what()}};
                }
                return {{"ok", true}, {"spec", spec}};
            }
            const std::string name = worldArg(a);
            std::optional<WorldRef> w = findWorld(name);
            if (!name.empty() && !w)
                throw CommandError(std::string(cmd) + ": no world named " + name);
            json out = readBack(w);
            if (a.has("file"))
            {
                const fs::path p = paths::resolvePath(a.get<std::string>("file"));
                std::error_code ec;
                fs::create_directories(p.parent_path(), ec);
                std::ofstream f(p, std::ios::binary);
                if (!f)
                    throw CommandError(std::string(cmd) + ": can't write " + paths::toUtf8(p));
                f << out["spec"].dump(2);
                out["file"] = paths::toUtf8(p);
            }
            return out;
        },
        .examples = {"s = worldSpec()['spec']\ns['sites'][0]['params']['tiers'] = 6\nworldBuild(spec=s, replace=True)",
                     "worldSpec(file='worlds/now.json')", "worldSpec(spec={'name': 'Test', 'terrain': {}})['ok']"},
    });

    registerCheckCommands(ctx);

    mcp::Server& server = mcp::Server::get();
    server.addResource(ctx, {.uri = "lila://schema/world-spec",
                             .name = "World spec schema",
                             .description = "JSON schema of the world spec worldBuild builds and worldSpec reads back: remix, terrain, settlements, "
                                            "buildings, slots, sites, zones, check, export.",
                             .mimeType = "application/json",
                             .read = [] { return worldSpecSchema().dump(2); }});
    server.addResource(ctx, {.uri = "lila://guide/open-world",
                             .name = "Open World guide",
                             .description = "Building a whole open-world map over MCP: the world spec recipe, build order, worldCheck kinds and fixes, the overview map.",
                             .read = [] { return std::string(kGuide); }});
    server.addPrompt(ctx, {.name = "make_open_world",
                           .description = "Build an open-world map from a description: remix real places, settlements, buildings, sites, zones; check and export.",
                           .arguments = json::array({
                               {{"name", "description"}, {"description", "The map, e.g. \"coastal city with a port, an inland capital, foothill villages and a farm valley\""}, {"required", true}},
                               {{"name", "size"}, {"description", "Land extent, e.g. \"12 km by 12 km\""}, {"required", false}},
                               {{"name", "places"}, {"description", "Real places to remix, e.g. \"Mazatlan and Culiacan\""}, {"required", false}},
                               {{"name", "target"}, {"description", "Engine to export for: unreal or files"}, {"required", false}},
                           }),
                           .get = [](const json& args) { return fillPrompt(kPrompt, args); }});
}
} // namespace openworld::world

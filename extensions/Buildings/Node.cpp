// The Buildings geometry node: building specs and edits in, the merged shell and every module as an instance out,
// each module published as a slot. Buildings are generated in parallel and cached by what went into them, so an edit
// to one building of a cell regenerates only that one.

#include "BuildingsScene.h"

#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/Slots.h>
#include <lila/Tasks.h>

#include <chrono>
#include <mutex>
#include <set>
#include <unordered_map>

namespace buildings
{
using lila::gn::ExecContext;
using lila::gn::GeometryPtr;
namespace slots = lila::slots;

namespace
{
struct Cached
{
    std::shared_ptr<const Result> result;
    uint64_t tick = 0;
};
std::mutex gGenMutex;
std::unordered_map<uint64_t, Cached> gGen;
uint64_t gTick = 0;
constexpr size_t kCacheMax = 30000;

std::mutex gPubMutex;
/** The building Edit Rooms cuts away: its shell and modules are left out of its mesh. */
std::string gCutaway;
/** What modifiers set to lod auto show, by camera distance (the viewport sets it); near until it has. */
std::unordered_map<uint64_t, std::string> gViewLod;
std::map<uint64_t, std::shared_ptr<const Published>> gPub;

std::mutex gGeoMutex;
std::unordered_map<std::string, GeometryPtr> gGeo;

struct ParsedSpecs
{
    uint64_t hash = 0;
    std::vector<Spec> specs;
    std::vector<uint64_t> hashes;
};
std::mutex gParseMutex;
std::map<uint64_t, std::shared_ptr<const ParsedSpecs>> gParsed; // by owner

std::shared_ptr<const ParsedSpecs> parseSpecs(uint64_t owner, const std::string& text)
{
    const uint64_t h = fnv(text);
    {
        std::lock_guard lock(gParseMutex);
        auto it = gParsed.find(owner);
        if (it != gParsed.end() && it->second->hash == h)
            return it->second;
    }
    auto p = std::make_shared<ParsedSpecs>();
    p->hash = h;
    json j;
    try
    {
        j = text.empty() ? json::array() : json::parse(text);
    }
    catch (const std::exception& e)
    {
        throw lila::gn::ExecError(std::string("Buildings: spec is not JSON: ") + e.what());
    }
    if (!j.is_array())
        throw lila::gn::ExecError("Buildings: spec is a list of buildings");
    p->specs.reserve(j.size());
    for (const json& e : j)
    {
        p->specs.push_back(Spec::fromJson(e));
        p->hashes.push_back(fnv(e.dump()));
    }
    std::lock_guard lock(gParseMutex);
    gParsed[owner] = p;
    return p;
}

GeometryPtr emptyRef()
{
    static const GeometryPtr g = lila::gn::makeGeometry(lila::gn::Geometry{});
    return g;
}

/**
 * The palette colour a module's instance carries: render for the wall-like blocks and domes, roof for roofs, trim for the
 * rest. It shows on the module's sections without a shared material (bld_render, bld_trim, bld_roof); the shared ones
 * (glass, metal, frames...) keep their own colour.
 */
glm::vec3 modulePaletteColor(const Palette& p, const std::string& kind)
{
    static const std::set<std::string> kRender = {"wall", "pilaster", "cornice", "corner", "band", "plinth", "dome"};
    if (kind == "roof")
        return p.roof;
    return kRender.contains(kind) ? p.render : p.trim;
}

GeometryPtr placeholderRef(const Module& m)
{
    const Vec3 q{std::round(m.size.x / 5.0) * 5.0, std::round(m.size.y / 5.0) * 5.0, std::round(m.size.z)};
    const std::string key = m.kind + "/" + std::to_string(int(q.x)) + "/" + std::to_string(int(q.y)) + "/" + std::to_string(int(q.z)) + "/" +
                            std::to_string(m.variant);
    {
        std::lock_guard lock(gGeoMutex);
        auto it = gGeo.find(key);
        if (it != gGeo.end())
            return it->second;
    }
    const Soup& s = placeholderSoup(m.kind, m.size, m.variant);
    std::vector<std::string> names;
    for (int k = 0; k < MatCount; ++k)
        names.push_back(matName(k));
    GeometryPtr g = lila::gn::meshGeometry(toMesh(s, names));
    std::lock_guard lock(gGeoMutex);
    gGeo.emplace(key, g);
    return g;
}

std::string styleIdFor(const Spec& s, const json& edit)
{
    if (edit.is_object() && edit.contains("style") && edit["style"].is_string())
        return edit["style"].get<std::string>();
    return s.style.empty() ? styleFor("", s.program, s.use) : s.style;
}

uint64_t styleHash(const Style& s)
{
    return fnv(s.toJson().dump());
}

bool setsVisual(const slots::Payload& p)
{
    return !p.item.empty() || !p.pool.empty() || !p.poolTag.empty() || !p.mesh.empty() || !p.placeholder.empty() || (p.hidden && *p.hidden);
}

void exec(ExecContext& c)
{
    LILA_ZONE();
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t owner = c.env.owner;
    const bool autoLod = c.s("lod") == "auto";
    std::string lod = c.s("lod").empty() ? "near" : c.s("lod");
    if (autoLod)
    {
        std::lock_guard lock(gPubMutex);
        const auto it = gViewLod.find(owner);
        lod = it == gViewLod.end() ? "near" : it->second;
    }
    const std::string cell = c.s("cell");
    const bool withModules = c.b("modules");
    std::shared_ptr<const ParsedSpecs> parsed;
    {
        LILA_ZONE_N("buildings.parse");
        parsed = parseSpecs(owner, c.s("spec"));
    }
    json edits = json::object();
    if (const std::string e = c.s("edits"); !e.empty())
    {
        try
        {
            edits = json::parse(e);
        }
        catch (const std::exception& ex)
        {
            throw lila::gn::ExecError(std::string("Buildings: edits are not JSON: ") + ex.what());
        }
    }

    auto pub = std::make_shared<Published>();
    pub->modifier = owner;
    pub->cell = cell;
    pub->lod = lod;
    pub->autoLod = autoLod;
    for (const Spec& s : parsed->specs)
        for (const Vec2& p : s.footprint)
        {
            pub->lo = glm::min(pub->lo, Vec3(p.x, s.ground, p.y));
            pub->hi = glm::max(pub->hi, Vec3(p.x, s.ground, p.y));
        }
    pub->specs = parsed->specs;
    const size_t n = parsed->specs.size();
    pub->results.resize(n);

    // Styles and cache keys.
    std::map<std::string, std::pair<std::shared_ptr<const Style>, uint64_t>> styles;
    std::vector<std::shared_ptr<const Style>> styleOf(n);
    std::vector<Edits> editOf(n);
    std::vector<uint64_t> keys(n);
    std::set<std::string> missingStyles;
    for (size_t i = 0; i < n; ++i)
    {
        const Spec& s = parsed->specs[i];
        const json e = edits.value(s.id, json::object());
        std::string id = styleIdFor(s, e);
        auto it = styles.find(id);
        if (it == styles.end())
        {
            std::shared_ptr<const Style> st = findStyle(id);
            if (!st)
            {
                missingStyles.insert(id);
                st = findStyle("residential");
            }
            if (!st)
                throw lila::gn::ExecError("Buildings: no styles are loaded (the Buildings resources are missing)");
            it = styles.emplace(id, std::make_pair(st, styleHash(*st))).first;
        }
        styleOf[i] = it->second.first;
        editOf[i] = Edits::fromJson(e);
        keys[i] = parsed->hashes[i] ^ (it->second.second * 31) ^ (fnv(e.dump()) * 1099511628211ull);
        pub->byId[s.id] = int(i);
    }
    for (const std::string& m : missingStyles)
        pub->warnings.push_back("style '" + m + "' is not in the library; residential stands in");

    // Generate what the cache lacks, in parallel.
    std::vector<size_t> todo;
    {
        std::lock_guard lock(gGenMutex);
        ++gTick;
        for (size_t i = 0; i < n; ++i)
        {
            auto it = gGen.find(keys[i]);
            if (it != gGen.end())
            {
                pub->results[i] = it->second.result;
                it->second.tick = gTick;
            }
            else
                todo.push_back(i);
        }
    }
    {
        LILA_ZONE_N("buildings.generate");
        tbb::parallel_for(tbb::blocked_range<size_t>(0, todo.size()), [&](const tbb::blocked_range<size_t>& r) {
            for (size_t k = r.begin(); k < r.end(); ++k)
            {
                const size_t i = todo[k];
                pub->results[i] = generate(parsed->specs[i], *styleOf[i], editOf[i]);
            }
        });
    }
    pub->generated = int(todo.size());
    {
        std::lock_guard lock(gGenMutex);
        for (size_t i : todo)
            gGen[keys[i]] = {pub->results[i], gTick};
        if (gGen.size() > kCacheMax)
        {
            std::vector<std::pair<uint64_t, uint64_t>> ages;
            for (auto& [k, v] : gGen)
                ages.push_back({v.tick, k});
            std::sort(ages.begin(), ages.end());
            for (size_t k = 0; k < ages.size() / 3; ++k)
                gGen.erase(ages[k].second);
        }
    }
    for (size_t i = 0; i < n; ++i)
        for (const std::string& w : pub->results[i]->warnings)
            pub->warnings.push_back(parsed->specs[i].id + ": " + w);

    // Slots for every module, and which shell modules a rule or override replaced.
    const slots::StatePtr st = slots::state();
    const bool anyChange = !st->rules.empty() || !st->overrides.empty();
    std::vector<std::string> rulePatterns;
    for (const slots::Rule& r : st->rules)
        if (r.enabled)
            rulePatterns.push_back(r.type);

    std::string cutaway;
    {
        std::lock_guard lock(gPubMutex);
        cutaway = gCutaway;
    }
    Soup shell;
    std::vector<int32_t> faceBuilding;
    std::vector<std::string> materialNames;
    std::unordered_map<std::string, int32_t> materialIndex;
    auto materialId = [&](const std::string& name) {
        auto it = materialIndex.find(name);
        if (it != materialIndex.end())
            return it->second;
        const int32_t id = int32_t(materialNames.size());
        materialNames.push_back(name);
        materialIndex.emplace(name, id);
        return id;
    };
    auto inst = std::make_shared<lila::gn::Instances>();
    // The building's palette on its modules, which are shared placeholder meshes across buildings.
    std::vector<glm::vec4> moduleColors;
    std::unordered_map<std::string, int> refIndex;
    slots::InstanceSlots spec;
    spec.owner = "buildings:" + cell;
    std::vector<slots::Slot> slotList;
    {
        LILA_ZONE_N("buildings.assemble");
        size_t total = 0;
        for (size_t i = 0; i < n; ++i)
            total += pub->results[i]->modules.size();
        slotList.reserve(total);
        shell.reserve(4096);
        for (size_t i = 0; i < n; ++i)
        {
            const Result& r = *pub->results[i];
            const Spec& sp = parsed->specs[i];
            const bool cut = !cutaway.empty() && r.id == cutaway;
            std::vector<std::string> regions = sp.regions;
            regions.push_back("style:" + r.style);
            if (!cell.empty())
                regions.push_back("cell:" + cell);
            std::vector<std::string> tags = {"building"};
            if (!sp.program.empty())
                tags.push_back(sp.program);
            if (r.enterable)
                tags.push_back("enterable");
            std::vector<char> dropped(r.modules.size(), 0);
            for (size_t k = 0; k < r.modules.size(); ++k)
            {
                const Module& m = r.modules[k];
                slots::Slot s;
                s.id = slots::makeId(r.id, m.path);
                s.type = m.type;
                s.owner = r.id;
                s.regions = regions;
                s.tags = tags;
                if (!m.data.is_null())
                {
                    s.payload.data = m.data;
                    // The entry doors of enterable buildings: a rule on tag "entry" gives them the game's door class.
                    if (m.data.contains("entry"))
                        s.tags.push_back("entry");
                }
                s.transform = m.frame();
                if (m.shell && anyChange)
                {
                    bool check = st->overrides.count(s.id) > 0;
                    for (size_t p = 0; p < rulePatterns.size() && !check; ++p)
                        check = slots::typeMatches(rulePatterns[p], m.type);
                    if (check && setsVisual(slots::resolve(s, *st).payload))
                        dropped[k] = 1;
                }
                if (lod == "near" && withModules && !cut)
                {
                    int ref = 0;
                    if (m.shell)
                    {
                        auto it = refIndex.find("");
                        if (it == refIndex.end())
                        {
                            ref = int(inst->references.size());
                            inst->references.push_back(emptyRef());
                            refIndex.emplace("", ref);
                        }
                        else
                            ref = it->second;
                    }
                    else
                    {
                        const std::string key = m.kind + "/" + std::to_string(int(std::round(m.size.x / 5.0))) + "/" +
                                                std::to_string(int(std::round(m.size.y / 5.0))) + "/" + std::to_string(int(std::round(m.size.z))) +
                                                "/" + std::to_string(m.variant);
                        auto it = refIndex.find(key);
                        if (it == refIndex.end())
                        {
                            ref = int(inst->references.size());
                            inst->references.push_back(placeholderRef(m));
                            refIndex.emplace(key, ref);
                        }
                        else
                            ref = it->second;
                    }
                    inst->add(s.transform, ref);
                    moduleColors.push_back(m.shell ? glm::vec4(0.0f) : glm::vec4(modulePaletteColor(r.palette, m.kind), 1.0f));
                }
                slotList.push_back(std::move(s));
            }

            if (cut)
                continue;
            // Shell faces, materials by palette.
            const Soup& src = lod == "near" ? r.shell : lod == "hlod" ? r.hlod : r.far;
            int32_t mats[MatCount];
            for (int k = 0; k < MatCount; ++k)
                mats[k] = materialId(materialFor(r, k));
            const int32_t bayBase = 0;
            (void)bayBase;
            for (size_t f = 0, off = 0; f < src.sizes.size(); off += src.sizes[f], ++f)
            {
                const int32_t o = src.owner[f];
                if (lod == "near" && o >= 0 && size_t(o) < dropped.size() && dropped[size_t(o)])
                    continue;
                const int32_t m = mats[std::clamp(src.mat[f], 0, int(MatCount) - 1)];
                shell.poly(src.pts.data() + off, src.uvs.data() + off, src.sizes[f], m, src.trim[f], o, src.part[f], src.bay[f], src.colour[f]);
                faceBuilding.push_back(int32_t(i));
            }
        }
    }

    lila::gn::Geometry g;
    {
        LILA_ZONE_N("buildings.mesh");
        // toMesh writes material ids as given; the soup holds node material indices already.
        lila::geo::Mesh mesh = toMesh(shell, materialNames);
        if (mesh.numFaces() == faceBuilding.size())
        {
            auto& fa = mesh.faceAttrs();
            fa.add("bldIndex", lila::geo::AttrType::Int);
            auto bi = fa.get<int32_t>("bldIndex");
            std::copy(faceBuilding.begin(), faceBuilding.end(), bi.begin());
        }
        pub->faces = mesh.numFaces();
        g.mesh = std::make_shared<const lila::geo::Mesh>(std::move(mesh));
    }
    pub->slots = slotList.size();
    GeometryPtr out;
    if (lod == "near" && withModules)
    {
        LILA_ZONE_N("buildings.slots");
        std::span<glm::vec4> colors = inst->attrs.getOrAdd<glm::vec4>(lila::gn::kInstanceColorAttr);
        std::copy(moduleColors.begin(), moduleColors.end(), colors.begin());
        g.instances = inst;
        spec.slots = std::move(slotList);
        out = slots::applyToInstances(lila::gn::makeGeometry(std::move(g)), lila::scene::NodeId(owner), spec);
    }
    else
    {
        if (owner != 0)
            slots::publish(lila::scene::NodeId(owner), std::move(slotList));
        out = lila::gn::makeGeometry(std::move(g));
    }
    pub->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    {
        std::lock_guard lock(gPubMutex);
        gPub[owner] = pub;
    }
    c.out("geometry", out);
}
} // namespace

std::string materialFor(const Result& r, int mat)
{
    switch (mat)
    {
    case MatRender:
        return r.palette.renderIndex >= 0 ? "Bld_" + r.style + "_render" + std::to_string(r.palette.renderIndex) : "Bld_c" + colourHex(r.palette.render).substr(1);
    case MatTrim:
        return r.palette.trimIndex >= 0 ? "Bld_" + r.style + "_trim" + std::to_string(r.palette.trimIndex) : "Bld_c" + colourHex(r.palette.trim).substr(1);
    case MatRoof:
        return r.palette.roofIndex >= 0 ? "Bld_" + r.style + "_roof" + std::to_string(r.palette.roofIndex) : "Bld_c" + colourHex(r.palette.roof).substr(1);
    case MatBase:
        return "Bld_" + r.style + "_base";
    default:
        return matName(mat);
    }
}

std::vector<std::pair<std::string, glm::vec3>> styleMaterials(const Style& s)
{
    std::vector<std::pair<std::string, glm::vec3>> out;
    for (size_t k = 0; k < s.renders.size(); ++k)
        out.push_back({"Bld_" + s.id + "_render" + std::to_string(k), s.renders[k]});
    for (size_t k = 0; k < s.trims.size(); ++k)
        out.push_back({"Bld_" + s.id + "_trim" + std::to_string(k), s.trims[k]});
    for (size_t k = 0; k < s.roofs.size(); ++k)
        out.push_back({"Bld_" + s.id + "_roof" + std::to_string(k), s.roofs[k]});
    out.push_back({"Bld_" + s.id + "_base", s.bases.empty() ? glm::vec3(0.35f) : s.bases.front()});
    return out;
}

std::shared_ptr<const Published> published(uint64_t modifier)
{
    std::lock_guard lock(gPubMutex);
    auto it = gPub.find(modifier);
    return it == gPub.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<const Published>> allPublished()
{
    std::vector<std::shared_ptr<const Published>> out;
    std::lock_guard lock(gPubMutex);
    for (auto& [k, v] : gPub)
        out.push_back(v);
    return out;
}

void forgetPublished(uint64_t modifier)
{
    std::lock_guard lock(gPubMutex);
    gPub.erase(modifier);
}

bool setViewLod(uint64_t modifier, const std::string& lod)
{
    std::lock_guard lock(gPubMutex);
    std::string& was = gViewLod[modifier];
    if (was == lod)
        return false;
    was = lod;
    return true;
}

bool setCutawayBuilding(const std::string& id)
{
    std::lock_guard lock(gPubMutex);
    if (gCutaway == id)
        return false;
    gCutaway = id;
    return true;
}

void clearGenerationCache()
{
    std::lock_guard lock(gGenMutex);
    gGen.clear();
}

void registerNode(lila::ext::Context& ctx)
{
    using Socket = lila::gn::SocketType;
    lila::gn::NodeDecl d;
    d.type = kNodeType;
    d.label = "Buildings";
    d.category = "Buildings";
    d.doc = "Procedural buildings from specs (Settlement envelopes, footprints): a merged shell and every module (windows, doors, "
            "shopfronts, signs, balconies, tanks...) as an instance, each published as a slot with a stable id. Edits are kept by "
            "building id. lod near draws shell and modules, hlod a simplified proxy, far flat-roof massing.";
    d.sockets.inputs = {
        {.id = "spec", .type = Socket::String, .defaultValue = "[]", .doc = "Buildings as a JSON list (buildingCreate writes it).", .editor = "multiline"},
        {.id = "edits", .type = Socket::String, .defaultValue = "{}", .doc = "Edits by building id (buildingEdit writes them).", .editor = "multiline"},
        {.id = "lod", .type = Socket::String, .defaultValue = "near", .doc = "near, hlod or far."},
        {.id = "cell", .type = Socket::String, .defaultValue = "", .doc = "The cell these buildings are in (x_z), or a building id when standalone."},
        {.id = "modules", .type = Socket::Bool, .defaultValue = true, .doc = "Draw the modules (near only)."},
    };
    d.sockets.outputs = {{.id = "geometry", .type = Socket::Geometry, .doc = "Shell mesh and module instances."}};
    d.exec = exec;
    if (!lila::gn::registerDecl(ctx, std::move(d)))
        lila::log::warn("Buildings: the Buildings geometry node could not be registered");
}
} // namespace buildings

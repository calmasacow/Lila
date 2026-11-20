// Age presets and the age command. A preset is a Stagehand arrangement preset whose steps are weathering commands; the
// built-in ones are registered with Stagehand at load, so they show in its library and run through arrangePreset like
// any other. Every preset comes in a realistic and a toon version, and every piece slot defaults to the library's pieces
// for a role, the user's own first.

#include "Pieces.h"
#include "Placeholders.h"
#include "Weathering.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Log.h>
#include <lila/Profile.h>

#include <algorithm>

using namespace lila;

namespace weathering
{
void registerBasePieces();

namespace age
{
using T = ArgType;

/** Items slot defaulting to the library's pieces with a role (user pieces first, in the style). */
json roleSlot(const char* doc, std::initializer_list<const char*> roles, const std::string& style)
{
    json def = json::array();
    for (const char* r : roles)
        def.push_back(std::string("tag:weathering+") + r + "+" + style);
    return {{"kind", "items"}, {"doc", doc}, {"default", def}};
}

json targetsSlot() { return {{"kind", "meshes"}, {"doc", "What to age: walls, buildings, Blockwork blocks, rocks."}}; }

json baseStep(const char* slot, json flags)
{
    flags["items"] = std::string("$") + slot;
    return {{"command", "baseDressing"}, {"args", json::array({"$targets"})}, {"flags", std::move(flags)}};
}

/** The built-in presets in one style. Toon versions keep the shapes chunkier: fewer, bigger pieces. */
std::vector<json> builtins(const std::string& style)
{
    const bool toon = style == "toon";
    const double few = toon ? 0.6 : 1.0;  // toon dressing is sparser
    const double big = toon ? 1.25 : 1.0; // and its pieces bigger
    const std::string suffix = toon ? " Toon" : "";
    auto preset = [&](const std::string& name, const std::string& description, json slots, json steps) {
        slots["targets"] = targetsSlot();
        return json{{"name", name + suffix},
                    {"description", description},
                    {"category", "Weathering"},
                    {"style", style},
                    {"tags", json::array({"weathering", "age", style})},
                    {"slots", std::move(slots)},
                    {"steps", std::move(steps)}};
    };
    std::vector<json> out;
    out.push_back(preset("Concrete Debris", "Broken concrete heaped at the foot of walls, thinning outward.",
                         {{"rubble", roleSlot("Rubble pieces.", {"rubble"}, style)}},
                         json::array({baseStep("rubble", {{"width", 90.0 * big}, {"density", 9.0 * few}, {"falloff", 1.8}, {"name", "Debris"}})})));
    out.push_back(preset("Bricks and Rocks", "Loose bricks and stones along the base.",
                         {{"bricks", roleSlot("Bricks.", {"brick"}, style)}, {"rocks", roleSlot("Rocks.", {"rock"}, style)}},
                         json::array({baseStep("bricks", {{"width", 70.0 * big}, {"density", 6.0 * few}, {"name", "Bricks"}}),
                                      baseStep("rocks", {{"width", 110.0 * big}, {"density", 3.0 * few}, {"falloff", 1.0}, {"name", "Rocks"}})})));
    out.push_back(preset("Leaning Planks", "Boards and beams leaning on the walls.",
                         {{"planks", roleSlot("Planks and beams.", {"plank", "beam"}, style)}},
                         json::array({baseStep("planks", {{"width", 70.0}, {"density", 2.5 * few}, {"leanChance", 0.85}, {"name", "Planks"}})})));
    out.push_back(preset("Demolished Building", "The heaviest: rubble, bricks and broken timber piled round the base.",
                         {{"rubble", roleSlot("Rubble pieces.", {"rubble"}, style)},
                          {"bricks", roleSlot("Bricks.", {"brick"}, style)},
                          {"timber", roleSlot("Planks and beams.", {"plank", "beam"}, style)}},
                         json::array({baseStep("rubble", {{"width", 170.0 * big}, {"density", 14.0 * few}, {"falloff", 1.2}, {"name", "Rubble"}}),
                                      baseStep("bricks", {{"width", 130.0 * big}, {"density", 6.0 * few}, {"name", "Bricks"}}),
                                      baseStep("timber", {{"width", 110.0}, {"density", 2.0 * few}, {"leanChance", 0.5}, {"name", "Timber"}})})));
    out.push_back(preset("Burned Yard", "Charred boards and ash-dark rubble round a burned-out shell.",
                         {{"charred", roleSlot("Burned boards.", {"charred"}, style)}, {"rubble", roleSlot("Rubble pieces.", {"rubble"}, style)}},
                         json::array({baseStep("charred", {{"width", 100.0}, {"density", 3.0 * few}, {"leanChance", 0.4}, {"name", "Charred"}}),
                                      baseStep("rubble", {{"width", 80.0 * big}, {"density", 5.0 * few}, {"scaleNear", 0.8}, {"name", "Rubble"}})})));
    out.push_back(preset("Abandoned", "Boards leaning on the walls, weeds coming up along the base, litter blown against it.",
                         {{"planks", roleSlot("Planks.", {"plank"}, style)},
                          {"weeds", roleSlot("Weeds and grass.", {"plant", "grass"}, style)},
                          {"litter", roleSlot("Litter.", {"litter"}, style)}},
                         json::array({baseStep("planks", {{"width", 60.0}, {"density", 1.5 * few}, {"leanChance", 0.9}, {"name", "Planks"}}),
                                      baseStep("weeds", {{"width", 50.0 * big}, {"density", 12.0 * few}, {"falloff", 2.0}, {"name", "Weeds"}}),
                                      baseStep("litter", {{"width", 120.0}, {"density", 1.0 * few}, {"falloff", 0.6}, {"scaleNear", 1.0}, {"scaleFar", 1.0},
                                                          {"name", "Litter"}})})));
    out.push_back(preset("Overgrown", "Moss at the foot of the walls, weeds and bushes taking over the ground round it.",
                         {{"moss", roleSlot("Moss.", {"moss"}, style)},
                          {"plants", roleSlot("Weeds and grass.", {"plant", "grass"}, style)},
                          {"bushes", roleSlot("Bushes.", {"bush"}, style)}},
                         json::array({baseStep("moss", {{"width", 40.0}, {"density", 14.0 * few}, {"falloff", 2.5}, {"name", "Moss"}}),
                                      baseStep("plants", {{"width", 90.0 * big}, {"density", 16.0 * few}, {"falloff", 1.2}, {"name", "Plants"}}),
                                      baseStep("bushes", {{"width", 140.0 * big}, {"density", 0.8 * few}, {"falloff", 0.8}, {"spacing", 1.1}, {"name", "Bushes"}})})));
    return out;
}

/** Every weathering preset (category Weathering), as Stagehand lists them. */
json agePresetList()
{
    json out = json::array();
    for (const json& p : ops::execute("arrangementPresets", json::object()))
        if (p.value("category", std::string()) == "Weathering")
            out.push_back(p);
    return out;
}

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

/** The preset's name in a style: "Abandoned" in toon is "Abandoned Toon" when there is one. */
std::string styledPreset(const std::string& name, const std::string& style, const char* cmd)
{
    const json all = agePresetList();
    auto has = [&](const std::string& n) {
        return std::ranges::any_of(all, [&](const json& p) { return lower(p.value("name", std::string())) == lower(n); });
    };
    std::string base = name;
    if (lower(base).ends_with(" toon"))
        base = base.substr(0, base.size() - 5);
    const std::string want = style == "toon" ? base + " Toon" : base;
    if (has(want))
        return want;
    if (has(name))
        return name;
    std::string names;
    for (const json& p : all)
        names += (names.empty() ? "" : ", ") + p.value("name", std::string());
    throw CommandError(std::string(cmd) + ": no weathering preset named '" + name + "' (there are " + names + ")");
}

/** Every mesh an object stands for: itself, the meshes under a group, or an object set's members. */
void expandMeshes(Graph& g, const std::string& name, std::vector<std::string>& out, const char* cmd)
{
    scene::Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    if (n->type().isA("objectSet"))
    {
        for (const json& m : ops::execute("setMembers", {{"args", {n->path()}}, {"nodes", true}}))
            expandMeshes(g, m.get<std::string>(), out, cmd);
        return;
    }
    if (n->type().isA("mesh"))
    {
        if (std::ranges::find(out, n->path()) == out.end())
            out.push_back(n->path());
        return;
    }
    for (NodeId c : g.children(n->id()))
        expandMeshes(g, g.node(c)->path(), out, cmd);
}
} // namespace age

bool registerAge(ext::Context& ctx)
{
    using namespace age;
    registerBasePieces();
    for (const char* style : {"realistic", "toon"})
        for (json& p : builtins(style))
        {
            const std::string name = p["name"].get<std::string>();
            ops::execute("arrangementRegister", {{"args", {name}}, {"spec", std::move(p)}, {"source", "Weathering"}});
        }

    ctx.registerCommand({
        .name = "agePresets",
        .doc = "The weathering presets Age offers: built in (Concrete Debris, Bricks and Rocks, Leaning Planks, Demolished "
               "Building, Burned Yard, Abandoned, Overgrown, each with a Toon version) and any arrangement preset saved with "
               "category 'Weathering'. Each has name, description, style, slots and steps, as arrangementPresets gives them.",
        .undoable = false,
        .run = [](const Args&) -> json { return agePresetList(); },
        .examples = {"[p['name'] for p in agePresets()]"},
    });

    ctx.registerCommand({
        .name = "age",
        .doc = "Age things in one step with a weathering preset: dressing round their base, and whatever else the preset "
               "does, as one undo step gathered into one group. Objects may be meshes, groups (every mesh under them) or "
               "object sets such as Blockwork visgroups. Piece slots left out use the library's pieces for their role: your "
               "own assets tagged 'weathering' and the role win over the placeholders. Returns group, preset and each step's "
               "result.",
        .positionalDoc = "objects to age (default the selection)",
        .flags = {{"preset", "p", T::String, "Abandoned", "Preset name (agePresets lists them)."},
                  styleFlag(),
                  {"slots", "sl", T::Any, nullptr, "Piece slots to fill yourself: {slot: [meshes, library ids or tag:a+b]}."},
                  {"seed", "s", T::Int, int64_t(1), "Another seed gives another result."},
                  {"name", "n", T::String, nullptr, "Name of the group made (default the preset's name)."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const std::vector<std::string> objects = a.objects();
            if (objects.empty())
                throw CommandError("age: select or name what to age");
            std::vector<std::string> meshes;
            for (const std::string& o : objects)
                expandMeshes(g, o, meshes, "age");
            if (meshes.empty())
                throw CommandError("age: the objects have no meshes");
            const std::string preset = styledPreset(a.get<std::string>("preset", "Abandoned"), a.get<std::string>("style", "realistic"), "age");
            // Placeholders must exist before slot defaults name them by tag.
            buildPlaceholders(false);
            json slots = a.has("slots") ? a.flags["slots"] : json::object();
            if (!slots.is_object())
                throw CommandError("age: slots is a dict of slot: pieces");
            slots["targets"] = meshes;
            json call = {{"args", {preset}}, {"slots", slots}, {"seed", a.get<int64_t>("seed", 1)}};
            if (a.has("name"))
                call["name"] = a.get<std::string>("name");
            return ops::execute("arrangePreset", call);
        },
        .examples = {"age('Shed', preset='Abandoned')", "age(['Tower', 'Wall'], preset='Overgrown', style='toon', seed=3)",
                     "age('Visgroup_Ruins', preset='Demolished Building', slots={'rubble': ['MyRubble_grp']})"},
    });

    ctx.registerCommand({
        .name = "weatheringAddPieces",
        .doc = "Make your own meshes the pieces for a role: each selected object goes into the library tagged 'weathering', "
               "the role and the style, so every preset that uses the role picks them instead of the placeholders. Roles in "
               "use: rubble, rock, brick, plank, beam, charred, plant, grass, moss, bush, pebble, litter, leaf, and the ones "
               "edge decals, ivy and the generators list. Returns the asset ids.",
        .positionalDoc = "objects (default the selection)",
        .flags = {{"role", "r", T::String, nullptr, "The role: rubble, plank, plant, ..."},
                  styleFlag(),
                  {"collection", "c", T::String, nullptr, "Library collection (default Weathering/<role>)."},
                  {"tier", "", T::String, "user", "user, workspace, or a library folder path."},
                  {"thumbnail", "", T::Bool, true, "Render each asset's thumbnail."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (!a.has("role"))
                throw CommandError("weatheringAddPieces: give role=");
            const std::string role = a.get<std::string>("role");
            const std::string style = a.get<std::string>("style", "realistic");
            const std::vector<std::string> objects = a.objects();
            if (objects.empty())
                throw CommandError("weatheringAddPieces: select the meshes to add");
            json ids = json::array();
            for (const std::string& o : objects)
            {
                scene::Node* n = g.find(o);
                if (!n)
                    throw CommandError("weatheringAddPieces: no node named '" + o + "'");
                const json made = ops::execute("libraryAdd", {{"args", {n->path()}},
                                                              {"name", n->name()},
                                                              {"collection", a.get<std::string>("collection", "Weathering/" + role)},
                                                              {"tags", json::array({"weathering", role, style})},
                                                              {"tier", a.get<std::string>("tier", "user")},
                                                              {"thumbnail", a.get<bool>("thumbnail", true)},
                                                              {"overwrite", true}});
                ids.push_back(made.is_object() ? made.value("id", json()) : made);
            }
            return ids;
        },
        .examples = {"weatheringAddPieces(['Chunk1', 'Chunk2', 'Chunk3'], role='rubble')", "weatheringAddPieces(role='ivy-leaf', style='toon')"},
    });
    return true;
}
} // namespace weathering

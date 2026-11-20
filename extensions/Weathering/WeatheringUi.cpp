// The Level > Weathering menu: Age and the dressing commands as actions, so they are searchable, scriptable and get
// option boxes from their flags. Presets themselves are picked from the Presets tab of the Prop Library.

#include "Weathering.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/LevelToolkit.h>
#include <lila/Menus.h>

using namespace lila;
using namespace lila::ui;

namespace weathering
{
constexpr const char* kWeatheringMenu = "Level/Weathering";

void registerWeatheringUi(ext::Context& ctx)
{
    struct Entry
    {
        const char* id;
        const char* label;
        const char* command;
        const char* icon;
        const char* doc;
        const char* python;
        std::vector<std::string> keywords;
    };
    const Entry entries[] = {
        {"weathering.age", "Age", "age", "wand", "Age the selection with a weathering preset: dressing round its base and whatever else the preset adds.",
         "age('Shed', preset='Abandoned')", {"weather", "ruin", "abandoned", "overgrown", "grunge", "destroy", "damage", "dirty"}},
        {"weathering.baseDressing", "Base Dressing", "baseDressing", "stack-2",
         "Scatter pieces in a band round where the selection meets the ground: rubble, planks leaning on walls, weeds.",
         "baseDressing('Shed', items=['tag:weathering+rubble'])", {"scatter around", "debris", "rubble", "foot of wall", "skirt"}},
        {"weathering.groundDressing", "Ground Dressing", "groundDressing", "grain",
         "Scatter pieces over the selected ground, give it a ground material and darken it round walls and pieces.",
         "groundDressing('Ground', preset='Asphalt Crack')", {"scatter ground", "litter", "pebbles", "grass", "floor"}},
        {"weathering.groundTint", "Ground Tint", "groundTint", "droplet",
         "Darken the selected ground round what stands on it and wet its dips, live.", "groundTint('Ground')",
         {"ground colorize", "dirt", "wet", "puddle", "contact", "ambient occlusion"}},
        {"weathering.addPieces", "Add Pieces", "weatheringAddPieces", "books",
         "Make the selected meshes the library's pieces for a role (rubble, plank, ivy-leaf), so presets use them.",
         "weatheringAddPieces(['Chunk1', 'Chunk2'], role='rubble')", {"custom", "own meshes", "kit", "library", "replace placeholders"}},
        {"weathering.unreal", "Build Unreal Materials", "weatheringUnreal", "world-upload",
         "Build the weathering material functions and materials in the linked Unreal project.", "weatheringUnreal()",
         {"material function", "MF_LilaGroundTint", "unreal", "ue5"}},
    };
    for (const Entry& e : entries)
        actions::add(ctx, {.id = e.id,
                           .label = e.label,
                           .category = "Weathering",
                           .doc = e.doc,
                           .python = e.python,
                           .command = e.command,
                           .call = json::object(),
                           .icon = e.icon,
                           .keywords = e.keywords});
    menus::addItem(ctx, {.id = "weathering.menu", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Weathering",
                         .before = levelToolkit::kAssetsSection});
    for (const char* a : {"weathering.age", "weathering.baseDressing", "weathering.groundDressing", "weathering.groundTint"})
        menus::addItem(ctx, {.id = std::string("weathering.menu.") + a, .menu = kWeatheringMenu, .action = a});
    menus::addItem(ctx, {.id = "weathering.menu.sepPieces", .menu = kWeatheringMenu, .kind = menus::Kind::Separator});
    for (const char* a : {"weathering.addPieces", "weathering.unreal"})
        menus::addItem(ctx, {.id = std::string("weathering.menu.") + a, .menu = kWeatheringMenu, .action = a});
}
} // namespace weathering

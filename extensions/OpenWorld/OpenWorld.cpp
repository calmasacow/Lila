// Open World: the extension's entry point, Level > Open World and the Level Toolkit's Open World section.

#include "OpenWorld.h"
#include "ZoneScene.h"

#include <lila/Extension.h>
#include <lila/Menus.h>

using namespace lila;
using namespace lila::ui;

namespace openworld
{
namespace
{
std::vector<levelToolkit::Item>& sectionItems()
{
    static std::vector<levelToolkit::Item> items;
    return items;
}
} // namespace

void addTools(ext::Context& ctx, const std::vector<std::string>& menuOrder, const std::vector<levelToolkit::Item>& items)
{
    static bool menuMade = false;
    if (!menuMade)
    {
        menus::addItem(ctx, {.id = "openworld.menu", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Open World",
                             .before = levelToolkit::kDressSection});
        menuMade = true;
    }
    static int separators = 0;
    for (const std::string& a : menuOrder)
        if (a.empty())
            menus::addItem(ctx, {.id = "openworld.menu.sep" + std::to_string(++separators), .menu = kMenu, .kind = menus::Kind::Separator});
        else
            menus::addItem(ctx, {.id = "openworld.menu." + a, .menu = kMenu, .action = a});
    for (const levelToolkit::Item& i : items)
        sectionItems().push_back(i);
    levelToolkit::add(ctx, {.id = kSection, .title = "Open World", .items = sectionItems(), .order = 25, .docs = "open-world"});
}
} // namespace openworld

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    openworld::registerRemix(ctx);
    openworld::registerRemixUi(ctx);
    openworld::zones::registerZoneCommands(ctx);
    return 0;
}

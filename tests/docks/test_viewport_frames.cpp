#include <doctest/doctest.h>

#include "ops/Command.h"
#include "render/FrameState.h"
#include "scene/Graph.h"
#include "scene/SceneCommands.h"
#include "ui/ViewportWidget.h"

using namespace lila;
using json = nlohmann::json;

namespace
{
size_t drawsOf(const render::FrameState& s, scene::NodeId id)
{
    size_t n = 0;
    for (const render::MeshDraw& d : s.meshes)
        n += d.node == uint64_t(id);
    return n;
}
} // namespace

TEST_CASE("viewport: a modified mesh's cage wire is laid on only once something shows it")
{
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    ops::execute("newScene", json::object());
    const std::string cube = ops::execute("polyCube", {{"name", "Caged"}})[0];
    ops::execute("addModifier", {{"args", {cube}}, {"kind", "subdivision"}});
    ops::execute("select", {{"args", json::array()}});
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const scene::NodeId id = g.find(cube)->id();

    ui::ViewportWidget view;
    view.resize(400, 300);
    // Shaded, nothing selected: the surface alone, no wire laid onto the subdivided result.
    const size_t unselected = drawsOf(view.liveFrame(), id);
    CHECK(unselected == 1);

    // Selecting it shows the cage wire, so the next frame builds it.
    ops::execute("select", {{"args", {cube}}});
    const size_t selected = drawsOf(view.liveFrame(), id);
    CHECK(selected > unselected);
}

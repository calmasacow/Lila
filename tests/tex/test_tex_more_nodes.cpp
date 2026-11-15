#include "TexTest.h"

using namespace textest;

namespace
{
/** The node fed by perlin noise on its first input. */
Graph fed(const std::string& type, json values, const std::string& input = "input", const std::string& out = "out")
{
    Graph g;
    add(g, "Src", "perlin", {{"scale", 4}, {"seed", 7}});
    add(g, "N", type, std::move(values));
    link(g, "Src.out", "N." + input);
    return into(g, "N." + out);
}
} // namespace

TEST_CASE("tex more nodes: golden images")
{
    struct Case
    {
        const char* name;
        Graph graph;
    };
    Graph bricks;
    add(bricks, "B", "bricks");
    add(bricks, "H", "hexTile", {{"scale", 3}, {"seed", 2}, {"hueJitter", 0.3}});
    link(bricks, "B.mask", "H.input");
    bricks = into(bricks, "H.out");

    Graph wipe;
    add(wipe, "A", "perlin", {{"scale", 4}});
    add(wipe, "B", "voronoi", {{"scale", 5}});
    add(wipe, "C", "compare", {{"position", 0.4}, {"angle", 20.0}});
    link(wipe, "A.out", "C.a");
    link(wipe, "B.f1", "C.b");
    wipe = into(wipe, "C.out");

    Graph cells;
    add(cells, "R", "randomPerCell", {{"cellsX", 6}, {"cellsY", 3}, {"seed", 4}});
    cells = into(cells, "R.color");

    const std::vector<Case> cases = {
        {"hexTile", bricks},
        {"compare", wipe},
        {"colorMask", fed("colorMask", {{"color", {0.5, 0.5, 0.5, 1.0}}, {"tolerance", 0.1}, {"softness", 0.1}})},
        {"multiColorMask", fed("multiColorMask", {{"count", 2}, {"color1", {0.3, 0.3, 0.3, 1.0}}, {"color2", {0.7, 0.7, 0.7, 1.0}}, {"tolerance", 0.05}})},
        {"trig", fed("trig", {{"operation", "arcsine"}}, "a")},
        {"condition", fed("condition", {{"comparison", "greater"}}, "a")},
        {"remap", fed("remap", {{"fromLow", 0.3}, {"fromHigh", 0.7}, {"toLow", 1.0}, {"toHigh", 0.0}})},
        {"randomPerCell", cells},
    };
    for (const Case& c : cases)
    {
        INFO(c.name);
        Plan p = plan(c.graph, "Output", "baseColor", 64);
        REQUIRE_MESSAGE(p.error.empty(), p.error);
        golden(std::string("more_") + c.name, evaluateCpu(p).outputs.at("out"));
    }
}

TEST_CASE("tex more nodes: remap, condition and color mask values")
{
    Graph g;
    add(g, "C", "constant", {{"value", 0.6}});
    add(g, "R", "remap", {{"fromLow", 0.5}, {"fromHigh", 1.0}, {"toLow", 0.0}, {"toHigh", 10.0}});
    link(g, "C.out", "R.input");
    CHECK(texel(eval(into(g, "R.out"), "Output", "baseColor", 4), 1, 1)[0] == doctest::Approx(2.0).epsilon(1e-3));

    Graph h;
    add(h, "C", "constant", {{"value", 0.6}});
    add(h, "K", "condition", {{"comparison", "less"}});
    add(h, "T", "color", {{"color", {1.0, 0.0, 0.0, 1.0}}});
    add(h, "F", "color", {{"color", {0.0, 0.0, 1.0, 1.0}}});
    link(h, "C.out", "K.a");
    link(h, "T.out", "K.ifTrue");
    link(h, "F.out", "K.ifFalse");
    // 0.6 is not less than b's default 0.5: ifFalse.
    CHECK(texel(eval(into(h, "K.out"), "Output", "baseColor", 4), 1, 1)[2] == doctest::Approx(1.0));

    Graph m;
    add(m, "C", "color", {{"color", {0.9, 0.1, 0.1, 1.0}}});
    add(m, "M", "colorMask", {{"color", {1.0, 0.0, 0.0, 1.0}}, {"tolerance", 0.2}, {"softness", 0.01}});
    link(m, "C.out", "M.input");
    CHECK(texel(eval(into(m, "M.out"), "Output", "baseColor", 4, PortType::Gray), 1, 1)[0] == doctest::Approx(1.0));
}

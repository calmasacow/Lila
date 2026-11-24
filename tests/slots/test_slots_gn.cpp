#include <doctest/doctest.h>

#include "gn/Gn.h"
#include "gn/InstanceView.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "slots/Slots.h"

using namespace lila;
using json = nlohmann::json;

namespace
{
json gnRun(const char* name, json call = json::object()) { return ops::execute(name, call); }
json gnArgs(std::initializer_list<json> a) { return {{"args", json(a)}}; }
std::string gnAdd(const std::string& g, const char* type) { return gnRun("gnAddNode", gnArgs({g, type})).get<std::string>(); }
void gnLink(const std::string& g, const std::string& from, const std::string& to) { gnRun("gnConnect", gnArgs({g, from, to})); }
void gnSet(const std::string& g, const std::string& plug, json v) { gnRun("gnSetValue", gnArgs({g, plug, v})); }

/** Evaluates until the slots' deferred visuals are built (the hook re-runs the modifier once). */
gn::InstanceViewPtr settle(const std::string& mesh)
{
    scene::Graph& g = scene::Graph::current();
    for (int k = 0; k < 3; ++k)
        g.evaluate();
    return gn::instanceViewFor(g, g.find(mesh)->id());
}

size_t instanceCount(const gn::InstanceViewPtr& v)
{
    size_t n = 0;
    if (v)
        for (const auto& p : v->parts)
            n += p.transforms->size();
    return n;
}

struct Scatter
{
    std::string mesh, cube, slotsNode, instancer;
};

Scatter scatter()
{
    gnRun("newScene");
    slots::refresh();
    Scatter s;
    s.mesh = gnRun("polyPlane", {{"width", 100.0}, {"depth", 100.0}, {"name", "Field"}})[0];
    gnRun("gnCreateGroup", gnArgs({"Props"}));
    const std::string grid = gnAdd("Props", "MeshGrid"), dist = gnAdd("Props", "DistributePointsOnFaces"), iop = gnAdd("Props", "InstanceOnPoints");
    s.cube = gnAdd("Props", "MeshCube");
    s.slotsNode = gnAdd("Props", "Slots");
    s.instancer = iop;
    gnSet("Props", grid + ".sizeX", 400);
    gnSet("Props", grid + ".sizeY", 400);
    gnSet("Props", dist + ".density", 2);
    gnSet("Props", dist + ".seed", 5);
    gnSet("Props", s.slotsNode + ".type", "container.20ft");
    gnSet("Props", s.slotsNode + ".owner", "yard:Port");
    gnLink("Props", grid + ".mesh", dist + ".mesh");
    gnLink("Props", dist + ".points", iop + ".points");
    gnLink("Props", s.cube + ".mesh", iop + ".instance");
    gnLink("Props", iop + ".instances", s.slotsNode + ".instances");
    gnLink("Props", s.slotsNode + ".instances", "GroupOutput.geometry");
    gnRun("gnApply", gnArgs({s.mesh, "Props"}));
    return s;
}

std::vector<std::string> slotIds()
{
    std::vector<std::string> out;
    for (const slots::Slot& s : slots::collect(scene::Graph::current()))
        out.push_back(s.id);
    std::sort(out.begin(), out.end());
    return out;
}
} // namespace

TEST_CASE("the Slots node publishes stable ids and an unrelated edit keeps them")
{
    const Scatter s = scatter();
    const gn::InstanceViewPtr v = settle(s.mesh);
    const size_t n = instanceCount(v);
    REQUIRE(n > 10);
    const std::vector<std::string> before = slotIds();
    CHECK(before.size() == n);
    CHECK(before.front().starts_with("yard:Port/i"));
    for (const slots::Slot& sl : slots::collect(scene::Graph::current()))
        CHECK(sl.type == "container.20ft");

    // The cube's size is nothing the slots depend on.
    gnSet("Props", s.cube + ".size", json::array({30, 30, 30}));
    settle(s.mesh);
    CHECK(slotIds() == before);
}

TEST_CASE("rules swap and hide geometry-node instances")
{
    const Scatter s = scatter();
    const size_t n = instanceCount(settle(s.mesh));
    REQUIRE(n > 10);
    gnRun("polyCylinder", {{"name", "Crate"}});
    gnRun("slotRule", {{"args", {"add"}}, {"type", "container"}, {"mesh", "Crate"}});
    gn::InstanceViewPtr v = settle(s.mesh);
    REQUIRE(v);
    REQUIRE(v->parts.size() == 1);
    CHECK(v->parts[0].mesh->numFaces() != 6);
    CHECK(instanceCount(v) == n);

    const std::string first = slotIds().front();
    gnRun("slotSet", {{"id", first}, {"hidden", true}});
    CHECK(instanceCount(settle(s.mesh)) == n - 1);
    gnRun("undo");
    CHECK(instanceCount(settle(s.mesh)) == n);
    gnRun("undo");
    v = settle(s.mesh);
    REQUIRE(v->parts.size() == 1);
    CHECK(v->parts[0].mesh->numFaces() == 6);
}

TEST_CASE("int attributes make the path when given")
{
    const Scatter s = scatter();
    gnSet("Props", s.slotsNode + ".pathAttributes", "missing");
    settle(s.mesh);
    for (const std::string& id : slotIds())
        CHECK(id.starts_with("yard:Port/missing?"));
    const json rep = gnRun("slotReport", {{"validate", false}});
    CHECK_FALSE(rep["duplicates"].empty());
}

TEST_CASE("instance colours: a stored colour, a rule's data colour and an override's reach the instanced draw")
{
    const Scatter s = scatter();
    gn::InstanceViewPtr v = settle(s.mesh);
    const size_t n = instanceCount(v);
    REQUIRE(n > 10);
    REQUIRE(v->parts.size() == 1);
    CHECK_FALSE(v->parts[0].colors);

    // A colour the generator stores on its instances.
    const std::string store = gnAdd("Props", "StoreNamedAttribute");
    gnSet("Props", store + ".dataType", "color");
    gnSet("Props", store + ".domain", "instance");
    gnSet("Props", store + ".name", "color");
    gnSet("Props", store + ".value", json::array({0.0, 1.0, 0.0, 1.0}));
    gnLink("Props", s.instancer + ".instances", store + ".geometry");
    gnLink("Props", store + ".geometry", s.slotsNode + ".instances");
    v = settle(s.mesh);
    REQUIRE(v->parts.size() == 1);
    REQUIRE(v->parts[0].colors);
    REQUIRE(v->parts[0].colors->size() == n);
    for (const glm::vec4& c : *v->parts[0].colors)
        CHECK(c == glm::vec4(0, 1, 0, 1));

    // A rule's data colour (sRGB hex) wins over it, and an override's over the rule's.
    gnRun("slotRule", {{"args", {"add"}}, {"type", "container"}, {"data", {{"color", "#ff0000"}}}});
    v = settle(s.mesh);
    REQUIRE(v->parts[0].colors);
    for (const glm::vec4& c : *v->parts[0].colors)
        CHECK(c == glm::vec4(1, 0, 0, 1));
    const std::string first = slotIds().front();
    gnRun("slotSet", {{"id", first}, {"data", {{"color", json::array({0.0, 0.0, 1.0, 0.5})}}}});
    v = settle(s.mesh);
    int blue = 0, red = 0, other = 0;
    for (const glm::vec4& c : *v->parts[0].colors)
    {
        if (c == glm::vec4(0, 0, 1, 0.5f))
            ++blue;
        else if (c == glm::vec4(1, 0, 0, 1))
            ++red;
        else
            ++other;
    }
    CHECK(blue == 1);
    CHECK(red == int(n) - 1);
    CHECK(other == 0);

    CHECK(slots::dataColor({{"color", "#808080"}})->r == doctest::Approx(0.2158f).epsilon(0.01));
    CHECK_FALSE(slots::dataColor({{"color", "red"}}));
    CHECK_FALSE(slots::dataColor({{"livery", "x"}}));
}

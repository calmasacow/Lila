#include "Blockwork.h"

#include "extensions/ExtensionManager.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;
using Approx = doctest::Approx;
using geo::Index;

namespace
{
json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

Graph& g() { return Graph::current(); }

void fresh()
{
    auto& em = extensions::ExtensionManager::get();
    if (!em.isLoaded("Blockwork"))
        REQUIRE(em.load(LILA_TEST_BLOCKWORK_PACKAGE) == "Blockwork");
    run("newScene");
}

geo::Mesh out(const std::string& path)
{
    g().evaluate();
    Node* n = g().find(path);
    REQUIRE(n);
    const MeshPtr& m = n->as<MeshPtr>(MeshNode::kOutMesh);
    REQUIRE(m);
    return *m;
}

geo::Bounds worldBounds(const std::string& path)
{
    geo::Mesh m = out(path);
    const glm::dmat4 w = g().find(path)->as<glm::dmat4>(Transform::kWorldMatrix);
    geo::Bounds b;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            b.extend(glm::dvec3(w * glm::dvec4(m.position(p), 1.0)));
    return b;
}

bool closed(const geo::Mesh& m)
{
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e))
            return false;
    return m.validate().empty();
}

std::string box(glm::dvec3 origin, glm::dvec3 size, const std::string& name = "block1")
{
    return run("blockworkBlock", {{"origin", {origin.x, origin.y, origin.z}}, {"size", {size.x, size.y, size.z}}, {"name", name}}).get<std::string>();
}

/** The face whose normal is closest to n. */
Index faceFacing(const geo::Mesh& m, glm::dvec3 n)
{
    Index best = geo::kInvalid;
    double bestDot = -2.0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && glm::dot(m.faceNormal(f), n) > bestDot)
            bestDot = glm::dot(m.faceNormal(f), n), best = f;
    return best;
}

int countBlocks()
{
    int n = 0;
    for (NodeId id : g().nodes())
        if (Node* k = g().node(id); k && blockwork::isBlock(*k))
            ++n;
    return n;
}
} // namespace

TEST_CASE("blockwork: a box block stands on its footprint, pivot centred, under the level group")
{
    fresh();
    const std::string b = box({0, 0, 0}, {256, 128, 64});
    geo::Mesh m = out(b);
    CHECK(m.numFaces() == 6);
    CHECK(closed(m));
    geo::Bounds bb = worldBounds(b);
    CHECK(bb.min.x == Approx(0));
    CHECK(bb.max.x == Approx(256));
    CHECK(bb.max.z == Approx(128));
    CHECK(bb.max.y == Approx(64));
    CHECK(g().find(b)->as<glm::dvec3>(Transform::kRotatePivot).x == Approx(128));
    CHECK(g().node(g().parent(g().find(b)->id()))->name() == "Level");
    CHECK(blockwork::isBlock(*g().find(b)));
    CHECK(m.cornerAttrs().has("uv0"));
}

TEST_CASE("blockwork: negative sizes and flat blocks")
{
    fresh();
    geo::Bounds bb = worldBounds(box({0, 0, 0}, {-128, 64, -32}));
    CHECK(bb.min.x == Approx(-128));
    CHECK(bb.min.y == Approx(-32));
    CHECK(out(box({0, 0, 0}, {128, 128, 0}, "floor")).numFaces() == 1);
    geo::Mesh wall = out(box({0, 0, 0}, {128, 0, 64}, "wall"));
    CHECK(wall.numFaces() == 1);
    CHECK(std::abs(wall.faceNormal(0).y) == Approx(0).epsilon(1e-6));
    CHECK_THROWS(box({0, 0, 0}, {0, 0, 64}, "line"));
}

TEST_CASE("blockwork: shapes are closed")
{
    fresh();
    for (const char* shape : {"box", "wedge", "cylinder", "arch", "stairs"})
    {
        CAPTURE(shape);
        const std::string b = run("blockworkBlock", {{"shape", shape}, {"size", {256, 192, 160}}, {"name", shape}}).get<std::string>();
        geo::Mesh m = out(b);
        CHECK(closed(m));
        CHECK(worldBounds(b).max.y == Approx(160));
    }
    CHECK(out("cylinder").numFaces() == 14);
    CHECK(out("wedge").numFaces() == 5);
    CHECK(out("stairs").numFaces() == 8 * 2 + 4);
    const std::string l = run("blockworkBlock", {{"shape", "prism"}, {"profile", {{0, 0}, {256, 0}, {256, 128}, {128, 128}, {128, 256}, {0, 256}}},
                                                 {"size", {0, 0, 64}}, {"name", "ell"}})
                              .get<std::string>();
    geo::Mesh ell = out(l);
    CHECK(closed(ell));
    CHECK(ell.numFaces() == 8);
}

TEST_CASE("blockwork: stairs climb the way direction turns them")
{
    fresh();
    const std::string s = run("blockworkBlock", {{"shape", "stairs"}, {"size", {100, 400, 200}}, {"steps", 4}}).get<std::string>();
    geo::Mesh m = out(s);
    // The highest step's tread is at the far end (+z) for direction 0.
    double topZ = -1e9;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && m.position(p).y > 199.0)
            topZ = std::max(topZ, m.position(p).z);
    CHECK(topZ == Approx(400));
    double lowZ = 1e9;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && m.position(p).y > 199.0)
            lowZ = std::min(lowZ, m.position(p).z);
    CHECK(lowZ == Approx(300));
}

TEST_CASE("blockwork: texture lock keeps world texel density through extend")
{
    fresh();
    const std::string b = box({0, 0, 0}, {200, 200, 200});
    geo::Mesh before = out(b);
    const Index top = faceFacing(before, {0, 1, 0});
    run("blockworkExtend", {{"args", {b}}, {"faces", {top}}, {"distance", 100}});
    geo::Mesh m = out(b);
    CHECK(worldBounds(b).max.y == Approx(300));
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    REQUIRE(!uv.empty());
    // Every corner's uv is its position over the 100 cm tile, so the side faces did not stretch.
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const blockwork::FaceMapping fm = blockwork::faceMapping(m, f);
        m.forEachFaceCorner(f, [&](Index c) {
            glm::dvec2 want = blockwork::mapPoint(fm, m.faceNormal(f), m.position(m.cornerPoint(c)), 100.0);
            CHECK(double(uv[c].x) == Approx(want.x).epsilon(1e-5));
            CHECK(double(uv[c].y) == Approx(want.y).epsilon(1e-5));
        });
    }
    // A wall's v runs up the world: the 300 cm wall spans three repeats.
    const Index side = faceFacing(m, {0, 0, 1});
    float vmin = 1e9f, vmax = -1e9f;
    m.forEachFaceCorner(side, [&](Index c) { vmin = std::min(vmin, uv[c].y), vmax = std::max(vmax, uv[c].y); });
    CHECK(double(vmax - vmin) == Approx(3.0));
}

TEST_CASE("blockwork: mapping axes read like a viewer facing the face")
{
    blockwork::FaceMapping m;
    glm::dvec3 u, v;
    blockwork::mappingAxes(m, {0, 0, 1}, u, v);
    CHECK(u.x == Approx(1));
    CHECK(v.y == Approx(1));
    blockwork::mappingAxes(m, {1, 0, 0}, u, v);
    CHECK(u.z == Approx(-1));
    CHECK(v.y == Approx(1));
    blockwork::mappingAxes(m, {0, 1, 0}, u, v);
    CHECK(u.x == Approx(1));
    CHECK(v.z == Approx(-1));
    m.rotate = 90.0f;
    blockwork::mappingAxes(m, {0, 0, 1}, u, v);
    CHECK(u.y == Approx(1));
    CHECK(v.x == Approx(-1));
    // A slope maps on its own plane, so a ramp does not stretch.
    m.rotate = 0.0f;
    const glm::dvec3 n = glm::normalize(glm::dvec3(0, 1, 1));
    blockwork::mappingAxes(m, n, u, v);
    CHECK(glm::dot(u, n) == Approx(0).epsilon(1e-9));
    CHECK(glm::dot(v, n) == Approx(0).epsilon(1e-9));
}

TEST_CASE("blockwork: hollow walls a block, open faces bridged")
{
    fresh();
    const std::string b = box({0, 0, 0}, {256, 256, 256});
    run("blockworkHollow", {{"args", {b}}, {"thickness", 16}});
    geo::Mesh m = out(b);
    CHECK(m.numFaces() == 12);
    CHECK(closed(m));
    // The inner box is the outer one shrunk by the thickness.
    double innerMax = 0;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && m.position(p).x < 250 && m.position(p).x > 10)
            innerMax = std::max(innerMax, m.position(p).x);
    CHECK(innerMax == Approx(240));

    fresh();
    const std::string r = box({0, 0, 0}, {256, 256, 256}, "room");
    const Index top = faceFacing(out(r), {0, 1, 0});
    run("blockworkHollow", {{"args", {r}}, {"thickness", 16}, {"faces", {top}}});
    geo::Mesh open = out(r);
    CHECK(open.numFaces() == 5 + 5 + 4);
    CHECK(closed(open));

    fresh();
    const std::string o = box({0, 0, 0}, {256, 256, 256}, "shell");
    run("blockworkHollow", {{"args", {o}}, {"thickness", 16}, {"inward", false}});
    CHECK(closed(out(o)));
    CHECK(worldBounds(o).max.x == Approx(272));
}

TEST_CASE("blockwork: clip keeps a side and caps it")
{
    fresh();
    const std::string b = box({0, 0, 0}, {256, 128, 128});
    run("blockworkClip", {{"args", {b}}, {"point", {128, 0, 0}}, {"normal", {-1, 0, 0}}});
    CHECK(worldBounds(b).max.x == Approx(128));
    CHECK(closed(out(b)));

    fresh();
    const std::string w = box({0, 0, 0}, {256, 128, 128});
    json r = run("blockworkClip", {{"args", {w}}, {"point", {128, 64, 0}}, {"normal", {1, 1, 0}}, {"keep", "both"}});
    REQUIRE(r.size() == 2);
    CHECK(closed(out(r[0].get<std::string>())));
    CHECK(closed(out(r[1].get<std::string>())));
    CHECK(countBlocks() == 2);
}

TEST_CASE("blockwork: carve cuts every block the cutter overlaps")
{
    fresh();
    const std::string wall = box({0, 0, 0}, {512, 256, 32}, "wall");
    const std::string other = box({0, 0, 200}, {512, 256, 32}, "other");
    const int before = out(wall).numFaces();
    const std::string door = box({200, 0, -16}, {96, 192, 64}, "door");
    json r = run("blockworkCarve", {{"args", {door}}});
    CHECK(r.size() == 1);
    CHECK(g().find(door) == nullptr);
    geo::Mesh m = out(wall);
    CHECK(m.numFaces() > before);
    CHECK(closed(m));
    CHECK(out(other).numFaces() == 6);
}

TEST_CASE("blockwork: join merges touching blocks")
{
    fresh();
    const std::string a = box({0, 0, 0}, {128, 128, 128}, "a");
    box({128, 0, 0}, {128, 128, 128}, "b");
    run("blockworkJoin", {{"args", {a}}});
    CHECK(countBlocks() == 1);
    CHECK(worldBounds(a).max.x == Approx(256));
    CHECK(closed(out(a)));
}

TEST_CASE("blockwork: extend copy builds a new block from the faces")
{
    fresh();
    const std::string a = box({0, 0, 0}, {128, 128, 128}, "a");
    const Index side = faceFacing(out(a), {1, 0, 0});
    const std::string made = run("blockworkExtend", {{"args", {a}}, {"faces", {side}}, {"distance", 64}, {"mode", "copy"}}).get<std::string>();
    CHECK(made != a);
    geo::Bounds b = worldBounds(made);
    CHECK(b.min.x == Approx(128));
    CHECK(b.max.x == Approx(192));
    CHECK(closed(out(made)));
    CHECK(countBlocks() == 2);
}

TEST_CASE("blockwork: overlapping faces are removed from both blocks on request")
{
    fresh();
    const std::string a = box({0, 0, 0}, {128, 128, 128}, "a");
    const std::string b = run("blockworkBlock", {{"origin", {128, 0, 0}}, {"size", {128, 128, 128}}, {"name", "b"}, {"removeOverlap", true}})
                              .get<std::string>();
    CHECK(out(a).numFaces() == 5);
    CHECK(out(b).numFaces() == 5);
}

TEST_CASE("blockwork: grid steps")
{
    fresh();
    CHECK(run("blockworkGrid", {{"step", 32}})["step"].get<double>() == Approx(32));
    CHECK(run("blockworkGrid", {{"finer", true}})["step"].get<double>() == Approx(16));
    CHECK(run("blockworkGrid", {{"coarser", true}})["step"].get<double>() == Approx(32));
    CHECK(blockwork::snap(37.0, 16.0) == Approx(32));
}

namespace
{
glm::dvec2 cornerUv(const geo::Mesh& m, Index f, const glm::dvec3& at)
{
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    glm::dvec2 out(1e9);
    double best = 1e9;
    m.forEachFaceCorner(f, [&](Index c) {
        const double d = glm::length(m.position(m.cornerPoint(c)) - at);
        if (d < best)
            best = d, out = glm::dvec2(uv[c]);
    });
    return out;
}

void faceUvBounds(const geo::Mesh& m, Index f, glm::dvec2& lo, glm::dvec2& hi)
{
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    lo = glm::dvec2(1e9), hi = glm::dvec2(-1e9);
    m.forEachFaceCorner(f, [&](Index c) { lo = glm::min(lo, glm::dvec2(uv[c])), hi = glm::max(hi, glm::dvec2(uv[c])); });
}
} // namespace

TEST_CASE("blockwork: shift, fit and justify")
{
    fresh();
    const std::string b = box({0, 0, 0}, {300, 150, 200});
    const Index front = faceFacing(out(b), {0, 0, 1});
    glm::dvec2 lo0, hi0, lo, hi;
    faceUvBounds(out(b), front, lo0, hi0);
    run("blockworkTexture", {{"args", {b}}, {"faces", {front}}, {"shift", {0.25, 0}}});
    faceUvBounds(out(b), front, lo, hi);
    CHECK(lo.x == Approx(lo0.x + 0.25));

    run("blockworkTexture", {{"args", {b}}, {"faces", {front}}, {"fit", "both"}});
    faceUvBounds(out(b), front, lo, hi);
    CHECK(lo.x == Approx(0).epsilon(1e-5));
    CHECK(hi.x == Approx(1));
    CHECK(lo.y == Approx(0).epsilon(1e-5));
    CHECK(hi.y == Approx(1));

    run("blockworkTexture", {{"args", {b}}, {"faces", {front}}, {"reset", true}});
    run("blockworkTexture", {{"args", {b}}, {"faces", {front}}, {"justify", "right"}});
    faceUvBounds(out(b), front, lo, hi);
    CHECK(hi.x == Approx(1));
    CHECK(hi.x - lo.x == Approx(3));
}

TEST_CASE("blockwork: copying a texture round a corner keeps it continuous")
{
    fresh();
    const std::string b = box({0, 0, 0}, {200, 200, 200});
    geo::Mesh m = out(b);
    const Index front = faceFacing(m, {0, 0, 1}), side = faceFacing(m, {1, 0, 0});
    run("blockworkTexture", {{"args", {b}}, {"faces", {front}}, {"shift", {0.3, 0.1}}, {"turn", 30}});
    run("blockworkCopyTexture", {{"args", {b}}, {"source", b}, {"sourceFace", front}, {"faces", {side}}});
    m = out(b);
    // The corner they share has the same uv on both faces.
    for (const glm::dvec3 at : {glm::dvec3(200, 0, 200), glm::dvec3(200, 200, 200)})
    {
        const glm::dvec2 a = cornerUv(m, front, at), c = cornerUv(m, side, at);
        CHECK(a.x == Approx(c.x).epsilon(1e-4));
        CHECK(a.y == Approx(c.y).epsilon(1e-4));
    }
}

TEST_CASE("blockwork: align a texture's bottom to an edge")
{
    fresh();
    const std::string b = box({0, 0, 0}, {256, 128, 128});
    geo::Mesh m = out(b);
    const Index front = faceFacing(m, {0, 0, 1});
    std::vector<Index> pts = m.facePoints(front);
    run("blockworkAlignEdge", {{"args", {b}}, {"face", front}, {"edge", 0}, {"side", "bottom"}, {"fit", "height"}});
    m = out(b);
    const glm::dvec3 pa = m.position(pts[0]), pb = m.position(pts[1]);
    CHECK(cornerUv(m, front, pa).y == Approx(0).epsilon(1e-5));
    CHECK(cornerUv(m, front, pb).y == Approx(0).epsilon(1e-5));
    CHECK(cornerUv(m, front, pa).x == Approx(0).epsilon(1e-5));
    glm::dvec2 lo, hi;
    faceUvBounds(m, front, lo, hi);
    CHECK(hi.y - lo.y == Approx(1));
}

TEST_CASE("blockwork: a strip of faces maps continuously along it")
{
    fresh();
    const std::string road = run("blockworkBlock", {{"origin", {0, 0, 0}}, {"size", {400, 100, 0}}, {"name", "road"}}).get<std::string>();
    // Split the floor into four quads along x, then lay the strip.
    geo::Mesh m = blockwork::baseMesh(g(), g().find(road)->id());
    REQUIRE(m.numFaces() == 1);
    std::vector<glm::dvec3> pts;
    std::vector<Index> sizes, idx;
    for (int i = 0; i <= 4; ++i)
        pts.push_back({100.0 * i, 0, 0}), pts.push_back({100.0 * i, 0, 100});
    for (Index i = 0; i < 4; ++i)
        sizes.push_back(4), idx.insert(idx.end(), {2 * i, 2 * i + 1, 2 * i + 3, 2 * i + 2});
    geo::Mesh strip = geo::Mesh::fromPolygons(pts, sizes, idx);
    if (strip.faceNormal(0).y < 0)
        for (Index f = 0; f < 4; ++f)
            strip.flipFace(f);
    blockwork::setBaseMesh(g(), g().find(road)->id(), strip);
    run("blockworkTextureStrip", {{"args", {road}}, {"faces", {0, 1, 2, 3}}});
    m = out(road);
    for (Index f = 0; f + 1 < 4; ++f)
    {
        const glm::dvec3 at(100.0 * (f + 1), 0, 50);
        const glm::dvec2 a = cornerUv(m, f, glm::dvec3(at.x, 0, 0)), c = cornerUv(m, f + 1, glm::dvec3(at.x, 0, 0));
        CHECK(a.x == Approx(c.x).epsilon(1e-4));
    }
    glm::dvec2 lo, hi;
    faceUvBounds(m, 0, lo, hi);
    CHECK(std::abs(hi.y - lo.y) == Approx(1));
}

TEST_CASE("blockwork: texture lock off keeps the uvs, which then stretch")
{
    fresh();
    const std::string b = box({0, 0, 0}, {200, 200, 200});
    const Index top = faceFacing(out(b), {0, 1, 0});
    const Index side = faceFacing(out(b), {0, 0, 1});
    glm::dvec2 lo0, hi0, lo, hi;
    faceUvBounds(out(b), side, lo0, hi0);
    run("blockworkTextureLock", {{"args", {b}}, {"on", false}});
    faceUvBounds(out(b), side, lo, hi);
    CHECK(hi.y - lo.y == Approx(hi0.y - lo0.y));
    run("blockworkExtend", {{"args", {b}}, {"faces", {top}}, {"distance", 200}});
    faceUvBounds(out(b), side, lo, hi);
    CHECK(hi.y - lo.y == Approx(hi0.y - lo0.y));
    run("blockworkTextureLock", {{"args", {b}}, {"on", true}});
    faceUvBounds(out(b), side, lo, hi);
    CHECK(hi.y - lo.y == Approx(4));
}

TEST_CASE("blockwork: a placeholder image becomes a material mapped at the texel density")
{
    fresh();
    const std::string b = box({0, 0, 0}, {512, 512, 512});
    const std::string mat = run("blockworkPlaceholder", {{"name", "BwTestDev"}, {"size", 256}}).get<std::string>();
    run("blockworkMaterial", {{"args", {b}}, {"material", mat}});
    geo::Mesh m = out(b);
    // 256 px at 5.12 px/cm repeats every 50 cm: a 512 cm face spans 10.24 repeats.
    glm::dvec2 lo, hi;
    faceUvBounds(m, faceFacing(m, {0, 0, 1}), lo, hi);
    CHECK(hi.x - lo.x == Approx(10.24));
    CHECK(m.materialNames().size() >= 1);
}

TEST_CASE("blockwork: entity classes load and place")
{
    fresh();
    json classes = run("blockworkEntityClasses", {{"reload", true}});
    REQUIRE(classes.size() >= 10);
    const std::string ps = run("blockworkEntity", {{"class", "PlayerStart"}, {"position", {100, 0, 50}}, {"turn", 90}}).get<std::string>();
    Node* n = g().find(ps);
    REQUIRE(n);
    CHECK(n->type().isA("locator"));
    CHECK(n->as<std::string>(n->type().attrIndex("entityClass")) == "PlayerStart");
    CHECK(n->type().attrIndex("PlayerStartTag") >= 0);

    const std::string t = run("blockworkEntity", {{"class", "Trigger"}, {"origin", {0, 0, 0}}, {"size", {256, 128, 512}}}).get<std::string>();
    CHECK(blockwork::isBlock(*g().find(t)));
    geo::Bounds b = worldBounds(t);
    CHECK(b.max.x == Approx(256));
    CHECK(b.max.y == Approx(128));
    CHECK(b.max.z == Approx(512));

    g().select({});
    json sent = run("blockworkSendEntities", {{"dryRun", true}});
    REQUIRE(sent["entities"].size() == 2);
    for (const json& e : sent["entities"])
    {
        if (e["name"] == "Trigger1")
        {
            // A 256 x 128 x 512 Lila box is 512 x 256 x 128 in Unreal: forward is Lila z, right Lila x, up Lila y.
            std::vector<double> s = e["scale"];
            std::sort(s.begin(), s.end());
            CHECK(s[0] == Approx(0.64));
            CHECK(s[1] == Approx(1.28));
            CHECK(s[2] == Approx(2.56));
        }
        else
            CHECK(e["class"] == "/Script/Engine.PlayerStart");
        CHECK(!e["id"].get<std::string>().empty());
    }
    // A second send keeps the same ids.
    json again = run("blockworkSendEntities", {{"dryRun", true}});
    CHECK(again["entities"][0]["id"] == sent["entities"][0]["id"]);
}

TEST_CASE("blockwork: a block becomes a volume and back")
{
    fresh();
    const std::string b = box({0, 0, 0}, {128, 128, 128});
    run("blockworkVolume", {{"args", {b}}, {"class", "KillZone"}});
    Node* n = g().find(b);
    CHECK(n->as<std::string>(n->type().attrIndex("entityClass")) == "KillZone");
    CHECK(n->type().attrIndex("DamagePerSec") >= 0);
    CHECK(out(b).numFaces() == 6);
    run("blockworkVolume", {{"args", {b}}, {"clear", true}});
    n = g().find(b);
    CHECK(n->type().attrIndex("entityClass") < 0);
    CHECK(n->type().attrIndex("DamagePerSec") < 0);
    CHECK_THROWS(run("blockworkVolume", {{"args", {b}}, {"class", "PlayerStart"}}));
}

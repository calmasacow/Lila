#include "render/PaintGpu.h"

#include "geo/Bvh.h"
#include "ops/Command.h"
#include "scene/Node.h"
#include "tex/PaintStrokes.h"
#include "tex/PaintSurface.h"
#include "tex/TextureSet.h"

#include <cmath>

/**
 * The channel picker: values of the texture set itself (every channel as the stack evaluates it) or of one paint layer,
 * read at a texel, never from the screen, so lighting and display transforms don't colour what is picked.
 */
namespace lila::render::paint
{
// Named, not anonymous: unity builds put this beside the other paint command files.
namespace pick
{
using namespace tex::paint;
using scene::Node;
using json = nlohmann::json;
using T = ArgType;

scene::Graph& graph() { return scene::Graph::current(); }

/** The set's evaluated channels per UDIM tile, kept while its graph, settings and paint stay the same. */
struct Evaluated
{
    size_t stamp = 0;
    const void* paint = nullptr;
    int udim = 0;
    std::map<std::string, image::Image> images;
};
std::map<scene::NodeId, Evaluated> gEvaluated;

const std::map<std::string, image::Image>& evaluated(Node& set, int udim)
{
    PaintPtr paint = readPaint(set);
    Evaluated& e = gEvaluated[set.id()];
    const size_t stamp = tex::setStamp(set);
    if (e.stamp == stamp && e.paint == paint.get() && e.udim == udim && !e.images.empty())
        return e.images;
    const tex::SetSettings s = tex::readSettings(set);
    std::pair<int, int> size{s.width, s.height};
    for (const tex::Tile& t : s.tiles)
        if (t.udim == udim)
            size = s.tileSize(t);
    e.images = tex::evaluateOutputs(set, tex::channelRequests(tex::readGraph(set), s), udim, size.first, size.second);
    e.stamp = stamp;
    e.paint = paint.get();
    e.udim = udim;
    return e.images;
}

/** The texel of uv (in its UDIM tile) of an image: rgba. */
glm::vec4 texel(const image::Image& img, glm::dvec2 uv)
{
    const double fu = uv.x - std::floor(uv.x), fv = uv.y - std::floor(uv.y);
    const int x = std::clamp(int(fu * img.width), 0, img.width - 1), y = std::clamp(int((1.0 - fv) * img.height), 0, img.height - 1);
    const float* t = &img.rgba[(size_t(y) * size_t(img.width) + size_t(x)) * 4];
    return {t[0], t[1], t[2], t[3]};
}

json valueJson(const tex::Channel* c, glm::vec4 v)
{
    if (c && c->type == tex::PortType::Gray)
        return v.x;
    return {v.x, v.y, v.z};
}

json sampleChannels(const Args& a)
{
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError("texSampleChannels: texture set required");
    Node* set = tex::findSet(graph(), a.positional[0].get<std::string>());
    if (!set)
        throw CommandError("texSampleChannels: no texture set named '" + a.positional[0].get<std::string>() + "'");
    glm::dvec2 uv;
    if (a.has("uv"))
    {
        const json u = a.flags["uv"];
        if (!u.is_array() || u.size() != 2)
            throw CommandError("texSampleChannels: uv is [u, v]");
        uv = {u[0].get<double>(), u[1].get<double>()};
    }
    else if (a.has("position") && a.has("mesh"))
    {
        std::shared_ptr<const PaintMesh> pm = paintMesh(graph(), a.get<std::string>("mesh"), setMaterial(*set));
        if (!pm)
            throw CommandError("texSampleChannels: " + a.get<std::string>("mesh") + " isn't a mesh with faces");
        const json p = a.flags["position"];
        if (!p.is_array() || p.size() != 3)
            throw CommandError("texSampleChannels: position is [x, y, z] in the mesh's object space");
        geo::BvhClosest hit;
        if (!pm->bvh->closestPoint({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()}, hit, std::numeric_limits<double>::max()) ||
            hit.triangle >= pm->triangleCount())
            throw CommandError("texSampleChannels: no surface near " + p.dump());
        uv = glm::dvec2(pm->uv(uint32_t(hit.triangle), float(hit.bary.y), float(hit.bary.z)));
    }
    else
        throw CommandError("texSampleChannels: give uv, or mesh and position");

    const int udim = 1001 + int(std::floor(uv.x)) + 10 * int(std::floor(uv.y));
    const tex::SetSettings settings = tex::readSettings(*set);
    const std::string source = a.get<std::string>("source", "set");
    json values = json::object();
    if (source == "layer")
    {
        std::string layer = a.get<std::string>("layer", "");
        if (layer.empty())
            layer = readPaint(*set)->active;
        if (!readPaint(*set)->buffer(layer))
            throw CommandError("texSampleChannels: no paint layer '" + layer + "' in " + set->name());
        syncBuffer(graph(), *set, layer);
        // Only what the layer painted: a channel it never covered here has no value to pick.
        for (const tex::Channel& c : settings.channels)
        {
            const image::Image img = readChannel(set->id(), layer, c.name, udim);
            if (img.width <= 0)
                continue;
            const glm::vec4 v = texel(img, uv);
            if (v.w > 0.0f)
                values[c.name] = valueJson(&c, v);
        }
    }
    else if (source == "set")
    {
        try
        {
            for (const auto& [name, img] : evaluated(*set, udim))
                if (img.width > 0)
                    values[name] = valueJson(settings.channel(name), texel(img, uv));
        }
        catch (const std::exception& e)
        {
            throw CommandError(std::string("texSampleChannels: ") + e.what());
        }
    }
    else
        throw CommandError("texSampleChannels: source is set or layer");
    return {{"uv", {uv.x, uv.y}}, {"udim", udim}, {"source", source}, {"channels", values}};
}
} // namespace pick

void registerPickCommands()
{
    ops::registerCommand({
        .name = "texSampleChannels",
        .doc = "Pick a texture set's channel values at a point: uv=[u, v], or mesh= and position=[x, y, z] in its object space. source=set "
               "reads every channel as the stack evaluates it; source=layer reads one paint layer's own paint (layer=, else the "
               "active one), leaving out channels it didn't paint there. Values are linear: [r, g, b] for colour and vector channels, "
               "a number for gray ones. Returns {uv, udim, source, channels}.",
        .positionalDoc = "set",
        .flags = {{"uv", "", ArgType::List, nullptr, "[u, v]."},
                  {"mesh", "", ArgType::String, nullptr, "The mesh position is on."},
                  {"position", "", ArgType::List, nullptr, "[x, y, z] in the mesh's object space."},
                  {"source", "", ArgType::String, "set", "set or layer.", {"set", "layer"}},
                  {"layer", "", ArgType::String, nullptr, "The paint layer, for source=layer."}},
        .undoable = false,
        .run = pick::sampleChannels,
    });
}
} // namespace lila::render::paint

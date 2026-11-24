#include "slots/SlotsGn.h"

#include "geo/Mesh.h"
#include "gn/NodesCommon.h"
#include "scene/Graph.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <span>

namespace lila::slots
{
namespace detail
{
Resolved resolveImpl(const Slot& s, const State& st, bool mainThread);
std::vector<Slot> withPins(std::vector<Slot> slots, const State& st, std::vector<std::string>* conflicts);
std::vector<std::string> poolForTag(const std::string& tags, bool mainThread);
} // namespace detail

namespace
{
struct Pending
{
    std::map<std::string, std::pair<Resolved, std::string>> visuals; // key -> (resolved, type)
    std::set<std::string> tags;
    std::set<NodeId> publishers;
};
std::mutex& pendingMutex()
{
    static std::mutex m;
    return m;
}
Pending& pending()
{
    static Pending p;
    return p;
}

std::mutex& geoMutex()
{
    static std::mutex m;
    return m;
}
/** Mesh -> geometry wrapper, so every instance of one visual shares a reference. */
std::map<const geo::Mesh*, std::pair<std::weak_ptr<const geo::Mesh>, gn::GeometryPtr>>& geoCache()
{
    static std::map<const geo::Mesh*, std::pair<std::weak_ptr<const geo::Mesh>, gn::GeometryPtr>> c;
    return c;
}

gn::GeometryPtr geometryOf(const std::shared_ptr<const geo::Mesh>& m)
{
    std::lock_guard lock(geoMutex());
    auto& slot = geoCache()[m.get()];
    if (slot.second && !slot.first.expired())
        return slot.second;
    gn::Geometry g;
    g.mesh = m;
    slot = {m, gn::makeGeometry(std::move(g))};
    return slot.second;
}

bool setsVisual(const Payload& p)
{
    return !p.item.empty() || !p.mesh.empty() || !p.placeholder.empty() || !p.pool.empty() || !p.poolTag.empty();
}
} // namespace

std::string instancePath(const gn::Instances& in, size_t k, const std::vector<std::string>& pathAttributes)
{
    if (pathAttributes.empty())
        return "i" + std::to_string(k);
    std::string path;
    for (const std::string& a : pathAttributes)
    {
        std::span<const int32_t> v = in.attrs.get<int32_t>(a);
        if (!path.empty())
            path += '/';
        path += a + (k < v.size() ? std::to_string(v[k]) : std::string("?"));
    }
    return path;
}

gn::GeometryPtr applyToInstances(const gn::GeometryPtr& in, NodeId publisher, const InstanceSlots& spec)
{
    if (!in || !in->instances)
        return in;
    const gn::Instances& src = *in->instances;
    const std::string owner = spec.owner.empty() ? "unowned/" + std::to_string(publisher) : spec.owner;
    std::span<const int32_t> typeIdx = spec.typeAttribute.empty() ? std::span<const int32_t>{} : src.attrs.get<int32_t>(spec.typeAttribute);

    std::vector<Slot> slots;
    slots.reserve(src.size());
    for (size_t k = 0; k < src.size(); ++k)
    {
        Slot s;
        std::string type = spec.types.empty() ? std::string("prop") : spec.types.front();
        if (!typeIdx.empty() && k < typeIdx.size() && !spec.types.empty())
            type = spec.types[size_t(std::clamp<int32_t>(typeIdx[k], 0, int32_t(spec.types.size()) - 1))];
        s.type = std::move(type);
        s.id = makeId(owner, instancePath(src, k, spec.pathAttributes));
        s.transform = src.transforms[k];
        s.owner = owner;
        s.regions = spec.regions;
        s.tags = spec.tags;
        s.payload = spec.defaults;
        slots.push_back(std::move(s));
    }
    return applySlots(in, publisher, std::move(slots));
}

gn::GeometryPtr applySlots(const gn::GeometryPtr& in, NodeId publisher, std::vector<Slot> slots)
{
    if (!in || !in->instances)
        return in;
    const gn::Instances& src = *in->instances;
    StatePtr st = state();
    slots.resize(src.size());
    for (size_t k = 0; k < src.size(); ++k)
        slots[k].transform = src.transforms[k];
    if (publisher != 0)
        publish(publisher, slots);

    // Pins: a pinned instance keeps its frame; one new under a pinned owner goes.
    std::map<std::string, size_t> byId;
    for (size_t k = 0; k < slots.size(); ++k)
        byId[slots[k].id] = k;
    const std::vector<Slot> pinned = detail::withPins(slots, *st, nullptr);

    auto out = std::make_shared<gn::Instances>();
    out->references = src.references;
    out->attrs.mergeLayout(src.attrs);
    std::map<std::string, int> refOfKey;
    bool missing = false;
    std::vector<int32_t> slotIndex;
    for (const Slot& s : pinned)
    {
        auto at = byId.find(s.id);
        const bool generated = at != byId.end();
        const Resolved r = detail::resolveImpl(s, *st, false);
        if (r.payload.hidden && *r.payload.hidden)
            continue;
        if (!r.payload.poolTag.empty() && r.payload.pool.empty() && r.payload.item.empty() && r.poolPick.empty())
        {
            std::lock_guard lock(pendingMutex());
            pending().tags.insert(r.payload.poolTag);
            pending().publishers.insert(publisher);
            missing = true;
        }
        int ref = generated ? src.reference[at->second] : -1;
        if (setsVisual(r.payload) || !generated)
        {
            const std::string key = visualKey(r, s.type);
            auto known = refOfKey.find(key);
            if (known != refOfKey.end())
                ref = known->second;
            else if (std::shared_ptr<const geo::Mesh> m = cachedVisual(key))
            {
                ref = int(out->references.size());
                out->references.push_back(geometryOf(m));
                refOfKey[key] = ref;
            }
            else
            {
                std::lock_guard lock(pendingMutex());
                pending().visuals[key] = {r, s.type};
                pending().publishers.insert(publisher);
                missing = true;
            }
        }
        if (ref < 0)
            continue;
        const size_t row = out->size();
        out->add(s.transform, ref);
        out->attrs.resize(out->size());
        if (generated)
            out->attrs.copyFrom(src.attrs, row, at->second);
        slotIndex.push_back(generated ? int32_t(at->second) : -1);
    }
    std::span<int32_t> idx = out->attrs.getOrAdd<int32_t>("slotIndex");
    std::copy(slotIndex.begin(), slotIndex.end(), idx.begin());
    (void)missing;
    gn::Geometry g = *in;
    g.instances = std::move(out);
    return gn::makeGeometry(std::move(g));
}

namespace detail
{
/** After an evaluation, on the main thread: builds the visuals and pool tags evaluations asked for, then runs them again. */
void servicePending(scene::Graph& g)
{
    Pending work;
    {
        std::lock_guard lock(pendingMutex());
        if (pending().publishers.empty())
            return;
        work = std::move(pending());
        pending() = Pending{};
    }
    for (const std::string& t : work.tags)
        poolForTag(t, true);
    for (const auto& [key, rt] : work.visuals)
        visualFor(g, rt.first, rt.second, false);
    for (NodeId n : work.publishers)
        if (n != 0 && g.node(n))
            g.touch(n);
}

void registerGnNode()
{
    using namespace gn::nodes;
    auto list = [](const std::string& s) {
        std::vector<std::string> out;
        size_t start = 0;
        while (start < s.size())
        {
            size_t comma = s.find(',', start);
            if (comma == std::string::npos)
                comma = s.size();
            std::string part = s.substr(start, comma - start);
            while (!part.empty() && part.front() == ' ')
                part.erase(part.begin());
            while (!part.empty() && part.back() == ' ')
                part.pop_back();
            if (!part.empty())
                out.push_back(part);
            start = comma + 1;
        }
        return out;
    };
    gn::registerDecl({
        .type = "Slots",
        .label = "Slots",
        .category = "Instances",
        .doc = "Makes instances typed slots with stable ids (owner plus a path from int attributes, or the index), so slot rules and "
               "overrides swap what each one shows. Writes the int attribute slotIndex. Type takes a comma list picked per instance "
               "by Type Attribute.",
        .sockets = {.inputs = {geoIn("instances"), in("type", T::String, "prop", "Slot type, or a comma list indexed by Type Attribute."),
                               in("owner", T::String, "", "The owner's stable id; slot ids start with it."),
                               in("typeAttribute", T::String, "", "Int attribute picking each instance's entry in Type."),
                               in("pathAttributes", T::String, "", "Int attributes (comma list) making each slot's path; empty uses the index."),
                               in("regions", T::String, "", "Regions the slots lie in (comma list), for region rules."),
                               in("tags", T::String, "", "Tags every slot carries (comma list).")},
                    .outputs = {geoOut("instances")}},
        .exec =
            [list](gn::ExecContext& c) {
                InstanceSlots spec;
                spec.owner = c.s("owner");
                spec.types = list(c.s("type"));
                spec.typeAttribute = c.s("typeAttribute");
                spec.pathAttributes = list(c.s("pathAttributes"));
                spec.regions = list(c.s("regions"));
                spec.tags = list(c.s("tags"));
                c.out("instances", applyToInstances(c.geometry("instances"), NodeId(c.env.owner), spec));
            },
    });
}
} // namespace detail
} // namespace lila::slots

// OSM features through the remix: every node placed like the heights under it, ways cut where regions were cut and
// pasted, areas kept whole, relations following their ways. Works on nodes only, so any feature kind comes along.

#include "RemixWork.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace openworld::detail
{
namespace
{
/** A piece of the output: 0 the (warped) background, r + 1 region r's paste. */
struct Pieces
{
    const Work& w;
    size_t source;
    /** The background's forward map per base point, memoised by node. */
    Vec2 map(int piece, Vec2 b) const
    {
        if (piece == 0)
            return w.warp->forward(b);
        return w.spec.regions[size_t(piece - 1)].rigid().apply(b);
    }
    bool present(int piece, Vec2 b) const
    {
        if (!w.owns(source, b))
            return false;
        if (piece == 0)
            return !w.punched(b) && !w.covered(w.warp->forward(b), -1);
        const size_t r = size_t(piece - 1);
        return inside(w.spec.regions[r].polygon, b) && !w.covered(w.spec.regions[r].rigid().apply(b), int(r));
    }
    /** The edge between a present at a and absent at b, on the base map. */
    Vec2 boundary(int piece, Vec2 a, Vec2 b) const
    {
        double lo = 0.0, hi = 1.0;
        for (int it = 0; it < 28; ++it)
        {
            const double mid = 0.5 * (lo + hi);
            (present(piece, a + (b - a) * mid) ? lo : hi) = mid;
        }
        return a + (b - a) * lo;
    }
};

struct Run
{
    std::vector<int64_t> ids;
    bool startCut = false;
    bool endCut = false;
    double startAt = 0.0;
    double endAt = 0.0;
    int piece = 0;
    size_t way = 0;
    Vec2 baseEnd{0.0};
};
} // namespace

void moveFeatures(Work& w, Features& f)
{
    LILA_ZONE();

    std::set<int64_t> usedNodes, usedWays, usedRelations;
    std::vector<int> pieces{0};
    for (size_t r = 0; r < w.spec.regions.size(); ++r)
        if (pastes(w.spec.regions[r].op))
            pieces.push_back(int(r) + 1);
    const double probe = std::max(w.spec.seams.width * 0.5, w.out->after.cell);

    for (size_t s = 0; s < w.data.size(); ++s)
    {
        if (!w.data[s] || w.data[s]->osm.empty())
            continue;
        const SourceData& d = *w.data[s];
        const OsmDoc& doc = d.osm;
        const Source& src = w.spec.sources[s];
        const Pieces P{w, s};

        std::unordered_map<int64_t, Vec2> base;
        base.reserve(doc.nodes.size());
        for (const OsmDoc::Node& n : doc.nodes)
            base[n.id] = placeOnBase(src, w.spec.horizontal, d.geo.toLocal(n.at));
        std::map<std::pair<int, int64_t>, int64_t> instances;
        std::map<std::pair<int, int64_t>, bool> presentMemo;
        auto present = [&](int piece, int64_t id) {
            const auto key = std::pair(piece, id);
            if (auto it = presentMemo.find(key); it != presentMemo.end())
                return it->second;
            return presentMemo[key] = P.present(piece, base.at(id));
        };
        auto instance = [&](int piece, int64_t id) {
            const auto key = std::pair(piece, id);
            if (auto it = instances.find(key); it != instances.end())
                return it->second;
            const OsmDoc::Node* n = doc.node(id);
            const Vec2 at = P.map(piece, base.at(id));
            int64_t out;
            if (piece == 0 && usedNodes.insert(id).second)
            {
                out = id;
                f.nodeAt[out] = f.nodes.size();
                f.nodes.push_back({out, at, n ? n->tags : OsmTags{}});
            }
            else
                out = f.addNode(at, n ? n->tags : OsmTags{});
            usedNodes.insert(out);
            return instances[key] = out;
        };
        auto cutNode = [&](int piece, Vec2 b) {
            const int64_t id = f.addNode(P.map(piece, b), {{"remix", "cut"}});
            usedNodes.insert(id);
            return id;
        };
        auto wayId = [&](int64_t orig, bool first) {
            if (first && usedWays.insert(orig).second)
                return orig;
            int64_t id = f.nextId++;
            usedWays.insert(id);
            return id;
        };

        std::map<int64_t, std::vector<int64_t>> derived;
        for (const OsmDoc::Way& way : doc.ways)
        {
            std::vector<int64_t> refs;
            for (int64_t id : way.nodes)
                if (base.count(id))
                    refs.push_back(id);
            if (refs.size() < 2)
                continue;
            const WayKind kind = wayKind(way);
            const bool closed = refs.front() == refs.back();
            const std::string cls(kind == WayKind::Road ? tagOf(way.tags, "highway") : kind == WayKind::Coastline ? "coastline" : "");

            if (kind == WayKind::Area)
            {
                // Areas are never cut: the whole outline goes with the piece holding most of it.
                int best = -1;
                size_t bestCount = 0;
                for (int p : pieces)
                {
                    size_t c = 0;
                    for (size_t i = 0; i + (closed ? 1 : 0) < refs.size(); ++i)
                        c += present(p, refs[i]);
                    if (c > bestCount)
                        best = p, bestCount = c;
                }
                if (best < 0)
                    continue;
                Features::Way out{wayId(way.id, best == 0), {}, way.tags, kind, false};
                for (int64_t id : refs)
                    out.nodes.push_back(instance(best, id));
                derived[way.id].push_back(out.id);
                f.ways.push_back(std::move(out));
                continue;
            }

            std::vector<Run> runs;
            for (int p : pieces)
            {
                Run run;
                run.piece = p;
                auto finish = [&](bool endCut, double at, Vec2 baseEnd) {
                    run.endCut = endCut;
                    run.endAt = at;
                    run.baseEnd = baseEnd;
                    if (run.ids.size() >= 2)
                        runs.push_back(run);
                    run = Run();
                    run.piece = p;
                };
                for (size_t i = 0; i < refs.size(); ++i)
                {
                    const bool here = present(p, refs[i]);
                    const Vec2 b = base.at(refs[i]);
                    if (here)
                    {
                        if (run.ids.empty())
                        {
                            if (i > 0)
                            {
                                const Vec2 c = P.boundary(p, b, base.at(refs[i - 1]));
                                run.ids.push_back(cutNode(p, c));
                                run.startCut = true;
                                run.startAt = double(i) - glm::length(b - c) / std::max(1e-9, glm::length(b - base.at(refs[i - 1])));
                            }
                            else
                                run.startAt = 0.0;
                        }
                        run.ids.push_back(instance(p, refs[i]));
                        // A long segment may cross a small hole or paste with both ends present.
                        if (i + 1 < refs.size() && present(p, refs[i + 1]))
                        {
                            const Vec2 nb = base.at(refs[i + 1]);
                            const double len = glm::length(nb - b);
                            const int samples = int(len / probe);
                            double firstOut = -1.0, lastOut = -1.0;
                            for (int k = 1; k < samples; ++k)
                            {
                                const double t = double(k) / samples;
                                if (!P.present(p, b + (nb - b) * t))
                                {
                                    if (firstOut < 0.0)
                                        firstOut = t;
                                    lastOut = t;
                                }
                            }
                            if (firstOut > 0.0)
                            {
                                const Vec2 exit = P.boundary(p, b, b + (nb - b) * firstOut);
                                run.ids.push_back(cutNode(p, exit));
                                finish(true, double(i) + firstOut, exit);
                                const Vec2 entry = P.boundary(p, nb, b + (nb - b) * lastOut);
                                run.ids.push_back(cutNode(p, entry));
                                run.startCut = true;
                                run.startAt = double(i) + lastOut;
                            }
                        }
                    }
                    else if (!run.ids.empty())
                    {
                        const Vec2 c = P.boundary(p, base.at(refs[i - 1]), b);
                        run.ids.push_back(cutNode(p, c));
                        finish(true, double(i) - 1.0 + glm::length(c - base.at(refs[i - 1])) / std::max(1e-9, glm::length(b - base.at(refs[i - 1]))), c);
                    }
                }
                if (!run.ids.empty())
                    finish(false, double(refs.size() - 1), base.at(refs.back()));
            }
            if (runs.empty())
                continue;
            // A closed line cut somewhere: its last and first runs of a piece meet at the closing node.
            if (closed)
                for (int p : pieces)
                {
                    auto first = std::find_if(runs.begin(), runs.end(), [&](const Run& r) { return r.piece == p && !r.startCut && r.startAt == 0.0; });
                    auto last = std::find_if(runs.begin(), runs.end(), [&](const Run& r) { return r.piece == p && !r.endCut && r.endAt == double(refs.size() - 1); });
                    if (first != runs.end() && last != runs.end() && first != last)
                    {
                        Run merged = *last;
                        merged.ids.insert(merged.ids.end(), first->ids.begin() + 1, first->ids.end());
                        merged.endCut = first->endCut;
                        merged.endAt = first->endAt + double(refs.size() - 1);
                        merged.baseEnd = first->baseEnd;
                        const size_t fi = size_t(first - runs.begin()), li = size_t(last - runs.begin());
                        runs[li] = merged;
                        runs.erase(runs.begin() + std::ptrdiff_t(fi));
                    }
                }
            bool firstOut = true;
            std::vector<std::pair<Run*, size_t>> chain;
            for (Run& r : runs)
            {
                Features::Way out{wayId(way.id, firstOut && r.piece == 0), r.ids, way.tags, kind, false};
                firstOut = false;
                r.way = f.ways.size();
                derived[way.id].push_back(out.id);
                f.ways.push_back(std::move(out));
                if (kind == WayKind::River)
                    f.riverBaseEnd[r.way] = r.baseEnd;
                if (kind != WayKind::Road && kind != WayKind::Coastline)
                    continue;
                size_t startEnd = SIZE_MAX, endEnd = SIZE_MAX;
                if (r.startCut)
                {
                    startEnd = f.ends.size();
                    f.ends.push_back({r.ids.front(), r.way, true, kind, cls});
                }
                if (r.endCut)
                {
                    endEnd = f.ends.size();
                    f.ends.push_back({r.ids.back(), r.way, false, kind, cls});
                }
                const bool duplicate = r.piece > 0 && w.spec.regions[size_t(r.piece - 1)].op == RegionOp::Duplicate;
                if (!duplicate)
                    chain.push_back({&r, (startEnd << 32) ^ endEnd});
            }
            // Partners: consecutive runs along the original way (duplicates excluded, the original stays).
            std::sort(chain.begin(), chain.end(), [](const auto& a, const auto& b) { return a.first->startAt < b.first->startAt; });
            for (size_t k = 0; k + 1 < chain.size(); ++k)
            {
                const Run& a = *chain[k].first;
                const Run& b = *chain[k + 1].first;
                if (!a.endCut || !b.startCut)
                    continue;
                size_t ea = SIZE_MAX, eb = SIZE_MAX;
                for (size_t e = 0; e < f.ends.size(); ++e)
                {
                    if (f.ends[e].way == a.way && !f.ends[e].start)
                        ea = e;
                    if (f.ends[e].way == b.way && f.ends[e].start)
                        eb = e;
                }
                if (ea != SIZE_MAX && eb != SIZE_MAX)
                    f.partners.push_back({ea, eb});
            }
        }

        // Points of interest and other tagged nodes not on a way.
        for (const OsmDoc::Node& n : doc.nodes)
        {
            if (n.tags.empty())
                continue;
            for (int p : pieces)
                if (!instances.count({p, n.id}) && present(p, n.id))
                    instance(p, n.id);
        }

        for (const OsmDoc::Relation& rel : doc.relations)
        {
            OsmDoc::Relation out{0, {}, rel.tags};
            for (const OsmDoc::Member& m : rel.members)
            {
                if (m.type == 'w')
                {
                    if (auto it = derived.find(m.ref); it != derived.end())
                        for (int64_t id : it->second)
                            out.members.push_back({'w', id, m.role});
                }
                else if (m.type == 'n')
                {
                    for (int p : pieces)
                        if (base.count(m.ref) && present(p, m.ref))
                            out.members.push_back({'n', instance(p, m.ref), m.role});
                }
                else
                    out.members.push_back(m);
            }
            if (out.members.empty())
                continue;
            if (usedRelations.insert(rel.id).second)
                out.id = rel.id;
            else
            {
                out.id = f.nextId++;
                usedRelations.insert(out.id);
            }
            f.relations.push_back(std::move(out));
        }
    }
}

void finishFeatures(Work& w, Features& f)
{
    LILA_ZONE();

    RemixResult& out = *w.out;
    OsmDoc& doc = out.osm;
    doc = OsmDoc();
    std::set<int64_t> referenced;
    for (const Features::Way& way : f.ways)
        referenced.insert(way.nodes.begin(), way.nodes.end());
    for (const OsmDoc::Relation& r : f.relations)
        for (const OsmDoc::Member& m : r.members)
            if (m.type == 'n')
                referenced.insert(m.ref);
    for (const Features::Node& n : f.nodes)
        if (referenced.count(n.id) || !n.tags.empty())
        {
            OsmTags tags = n.tags;
            // remix=cut marks ends while repairing; ends that were joined are ordinary nodes in the output.
            if (tagOf(tags, "remix") == "cut")
                tags.erase("remix");
            const LatLon ll = out.geo.toGeographic(n.at);
            doc.nodes.push_back({n.id, ll, std::move(tags)});
        }
    for (const Features::Way& way : f.ways)
    {
        doc.ways.push_back({way.id, way.nodes, way.tags});
        if (way.kind == WayKind::Road || way.kind == WayKind::River || way.kind == WayKind::Coastline)
        {
            RemixResult::Line line;
            line.kind = way.kind;
            line.added = way.added;
            for (int64_t id : way.nodes)
                if (const Features::Node* n = f.find(id))
                    line.points.push_back(n->at);
            out.lines.push_back(std::move(line));
        }
    }
    doc.relations = f.relations;
    doc.index();

    for (const Features::End& e : f.ends)
    {
        const Features::Node* n = f.find(e.node);
        CutEnd c{e.node, n ? n->at : Vec2(0.0), e.cls, e.joined, e.how};
        (e.kind == WayKind::Coastline ? out.coastEnds : out.roadEnds).push_back(c);
    }

    size_t roads = 0, rivers = 0, coast = 0, areas = 0, added = 0;
    for (const Features::Way& way : f.ways)
    {
        roads += way.kind == WayKind::Road;
        rivers += way.kind == WayKind::River;
        coast += way.kind == WayKind::Coastline;
        areas += way.kind == WayKind::Area;
        added += way.added;
    }
    size_t joined = 0, coastJoined = 0;
    for (const CutEnd& e : out.roadEnds)
        joined += e.joined;
    for (const CutEnd& e : out.coastEnds)
        coastJoined += e.joined;
    size_t draining = 0;
    for (const RiverState& r : out.rivers)
        draining += r.drains;
    out.stats["features"] = {{"nodes", doc.nodes.size()}, {"ways", doc.ways.size()}, {"relations", doc.relations.size()}, {"roads", roads},
                             {"rivers", rivers}, {"coastline", coast}, {"areas", areas}, {"added", added}};
    out.stats["roadEnds"] = {{"cut", out.roadEnds.size()}, {"joined", joined}, {"dangling", out.roadEnds.size() - joined}};
    out.stats["coastEnds"] = {{"cut", out.coastEnds.size()}, {"joined", coastJoined}, {"dangling", out.coastEnds.size() - coastJoined}};
    out.stats["rivers"] = {{"checked", out.rivers.size()}, {"draining", draining}};
}
} // namespace openworld::detail

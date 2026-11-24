#pragma once

#include "Remix.h"

#include <map>
#include <tuple>
#include <unordered_map>

/** The state one remix run shares between composing heights, moving features and repairing them. Internal. */
namespace openworld::detail
{
struct Work
{
    const RemixSpec& spec;
    std::vector<std::shared_ptr<const SourceData>> data;
    std::vector<Polygon> footprints;
    std::vector<double> scales;
    double feather = 1500.0;
    std::shared_ptr<Warp> warp;
    InverseGrid inverse;
    /** Per region: where it ends up on the output map. */
    std::vector<Polygon> targets;
    std::shared_ptr<RemixResult> out;

    explicit Work(const RemixSpec& s) : spec(s) {}

    /** 0 outside a source's footprint, 1 a feather inside it. */
    double weight(size_t s, Vec2 base) const;
    /** The source is there and no later source wins the spot. */
    bool owns(size_t s, Vec2 base) const;
    /** Inside a cut or delete region's polygon, so gone from the background. */
    bool punched(Vec2 base) const;
    /** Inside a pasted target later than region above (-1: the background, under every paste). */
    bool covered(Vec2 out, int above) const;
};

/** Working features: nodes on the output map, ways by node id. */
struct Features
{
    struct Node
    {
        int64_t id = 0;
        Vec2 at{0.0};
        OsmTags tags;
    };
    struct Way
    {
        int64_t id = 0;
        std::vector<int64_t> nodes;
        OsmTags tags;
        WayKind kind = WayKind::Other;
        bool added = false;
    };
    /** A road or coastline end made by a cut: its node, the way holding it and whether it starts that way. */
    struct End
    {
        int64_t node = 0;
        size_t way = 0;
        bool start = false;
        WayKind kind = WayKind::Road;
        std::string cls;
        bool joined = false;
        std::string how;
    };

    std::vector<Node> nodes;
    std::unordered_map<int64_t, size_t> nodeAt;
    std::vector<Way> ways;
    std::vector<OsmDoc::Relation> relations;
    std::vector<End> ends;
    /** Ends that were one place before a cut (indices into ends). */
    std::vector<std::pair<size_t, size_t>> partners;
    /** Downstream ends of rivers in base coordinates, by way index, for the drained-before test. */
    std::map<size_t, Vec2> riverBaseEnd;
    int64_t nextId = int64_t(1) << 50;

    int64_t addNode(Vec2 at, OsmTags tags = {});
    Node& node(int64_t id) { return nodes[nodeAt.at(id)]; }
    const Node* find(int64_t id) const
    {
        const auto it = nodeAt.find(id);
        return it == nodeAt.end() ? nullptr : &nodes[it->second];
    }
};

void moveFeatures(Work& w, Features& f);
void reconnectRoads(Work& w, Features& f);
void repairRivers(Work& w, Features& f);
void mergeCoast(Work& w, Features& f);
/** Whether each river drains now and did in the sources, into w.out->rivers. */
void assessRivers(Work& w, Features& f);
/** Writes f into w.out: the OSM doc in latitude and longitude, overlay lines, cut ends and river states. */
void finishFeatures(Work& w, Features& f);

/** Pixels-in-the-grid helpers. */
double bilinear(const std::vector<float>& v, const Heightfield& h, Vec2 p);
size_t nearestIndex(const Heightfield& h, Vec2 p);
} // namespace openworld::detail

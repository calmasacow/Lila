// Settlement: street and district names from a naming culture, stable by the ids of the streets they belong to.

#include "Generators.h"

#include <lila/Names.h>

#include <lila/Profile.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>

namespace settlement
{
namespace
{
/** A name for the street keyed h of class c; attempt picks another when the first is taken. */
std::string cultureStreetName(const std::string& culture, StreetClass cls, uint64_t h, int attempt)
{
    return lila::names::streetName(culture, int(std::min(cls, StreetClass::Path)), h, attempt);
}

std::string cultureDistrictName(const std::string& culture, uint64_t h, int attempt) { return lila::names::districtName(culture, h, attempt); }

std::string ordinal(int n)
{
    const int tens = n % 100;
    const char* suffix = tens >= 11 && tens <= 13 ? "th" : n % 10 == 1 ? "st" : n % 10 == 2 ? "nd" : n % 10 == 3 ? "rd" : "th";
    return std::to_string(n) + suffix;
}

int findRootOf(std::vector<int>& parent, int x)
{
    while (parent[size_t(x)] != x)
        x = parent[size_t(x)] = parent[size_t(parent[size_t(x)])];
    return x;
}

/** Runs of street edges carried on through junctions by their straightest continuation of the same class. */
std::vector<std::vector<int>> strokes(const StreetGraph& g)
{
    std::vector<int> parent(g.edges.size());
    std::iota(parent.begin(), parent.end(), 0);
    for (size_t n = 0; n < g.nodes.size(); ++n)
    {
        std::vector<int> es;
        for (int e : g.incident(int(n)))
            if (g.isStreet(e))
                es.push_back(e);
        auto away = [&](int e) {
            const StreetEdge& x = g.edges[size_t(e)];
            const Vec2 d = g.nodes[size_t(x.a == int(n) ? x.b : x.a)].p - g.nodes[n].p;
            const double len = glm::length(d);
            return len > 1e-9 ? d / len : Vec2(1.0, 0.0);
        };
        struct Pair
        {
            double straight;
            int a, b;
        };
        std::vector<Pair> pairs;
        for (size_t i = 0; i < es.size(); ++i)
            for (size_t j = i + 1; j < es.size(); ++j)
            {
                if (g.edges[size_t(es[i])].cls != g.edges[size_t(es[j])].cls)
                    continue;
                const double straight = -glm::dot(away(es[i]), away(es[j]));
                // Through a plain node a street bends as it likes; at a junction it carries on only within 40 degrees.
                if (es.size() == 2 || straight >= 0.766)
                    pairs.push_back({straight, es[i], es[j]});
            }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) {
            return x.straight != y.straight ? x.straight > y.straight : std::pair(x.a, x.b) < std::pair(y.a, y.b);
        });
        std::set<int> used;
        for (const Pair& p : pairs)
            if (!used.count(p.a) && !used.count(p.b))
            {
                used.insert(p.a);
                used.insert(p.b);
                parent[size_t(findRootOf(parent, p.a))] = findRootOf(parent, p.b);
            }
    }
    std::map<int, std::vector<int>> byRoot;
    for (size_t e = 0; e < g.edges.size(); ++e)
        if (g.isStreet(int(e)))
            byRoot[findRootOf(parent, int(e))].push_back(int(e));
    std::vector<std::vector<int>> out;
    for (auto& [root, edges] : byRoot)
        out.push_back(std::move(edges));
    return out;
}

/** Quarters between the main roads: faces of the arterials, the outline, water and exclusions. */
void findDistricts(Layout& l)
{
    StreetGraph major = l.graph;
    for (size_t e = 0; e < major.edges.size(); ++e)
        if (major.edges[e].alive && major.edges[e].cls > StreetClass::Arterial && major.edges[e].cls < StreetClass::Boundary)
            major.removeEdge(int(e));
    auto q = [](Vec2 p) { return uint64_t(std::llround(p.x / 10.0)) * 0x100000001B3ull ^ uint64_t(std::llround(p.y / 10.0)); };
    for (const Face& f : planarFaces(major))
    {
        if (f.outline.size() < 3)
            continue;
        std::set<uint64_t> around;
        for (int e : f.outline.tag)
        {
            const StreetEdge& x = major.edges[size_t(e)];
            if (x.street != 0)
                around.insert(x.street);
            else
                around.insert(hash64(q(major.nodes[size_t(x.a)].p)) ^ hash64(q(major.nodes[size_t(x.b)].p)));
        }
        uint64_t id = 0x6A09E667F3BCC908ull;
        for (uint64_t k : around)
            id = hash64(id ^ k);
        District d;
        d.id = id & ((1ull << 48) - 1); // small enough for any JSON reader
        d.outline = f.outline;
        d.area = area(f.outline) / kM2;
        d.center = centroid(f.outline);
        l.districts.push_back(std::move(d));
    }
    std::sort(l.districts.begin(), l.districts.end(), [](const District& a, const District& b) { return a.id < b.id; });
}

/** Unit direction along the grid's own axes: u along it, v across it. */
struct GridAxes
{
    Vec2 u, v;
};

/** "North 3rd Street" or "East 2nd Avenue" for a stroke running along the grid; empty when it does not. */
std::string gridName(const Layout& l, const std::vector<int>& edges, const GridAxes& ax)
{
    const StreetGraph& g = l.graph;
    Vec2 dir(0.0), mid(0.0);
    double total = 0.0;
    for (int e : edges)
    {
        const Vec2 a = g.nodes[size_t(g.edges[size_t(e)].a)].p, b = g.nodes[size_t(g.edges[size_t(e)].b)].p;
        Vec2 d = b - a;
        const double len = glm::length(d);
        if (len < 1e-9)
            continue;
        if (glm::dot(d, dir) < 0.0)
            d = -d;
        dir += d;
        mid += (a + b) * 0.5 * len;
        total += len;
    }
    if (total <= 0.0 || glm::length(dir) < 1e-9)
        return {};
    dir /= glm::length(dir);
    mid /= total;
    const double along = std::abs(glm::dot(dir, ax.u)), across = std::abs(glm::dot(dir, ax.v));
    const Vec2 off = mid - l.center;
    if (along > 0.966) // within 15 degrees of the grid's run: a numbered street, counted across the grid
    {
        const int k = int(std::lround(glm::dot(off, ax.v) / std::max(l.profile.gridDepth, 100.0)));
        return (k == 0 ? std::string() : k > 0 ? "South " : "North ") + ordinal(std::abs(k) + 1) + " Street";
    }
    if (across > 0.966)
    {
        const int k = int(std::lround(glm::dot(off, ax.u) / std::max(l.profile.gridWidth, 100.0)));
        return (k == 0 ? std::string() : k > 0 ? "East " : "West ") + ordinal(std::abs(k) + 1) + " Avenue";
    }
    return {};
}
} // namespace

std::vector<std::string> cultureNames() { return lila::names::cultures(); }

std::string defaultCulture(std::string_view profileName)
{
    std::string n(profileName);
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
    if (has("scifi") || has("cyber") || has("sector") || has("arcology"))
        return "sector";
    if (has("roman") || has("castrum"))
        return "latin";
    if (has("mexic") || has("sinaloa"))
        return "mexican";
    if (has("colonial") || has("spanish") || has("medina"))
        return "spanish";
    if (n.starts_with("us") || has("manhattan") || has("midwest") || has("grid"))
        return "usGrid";
    if (has("european") || has("haussmann") || has("baroque") || has("french") || has("renaissance"))
        return "french";
    return "oldEnglish";
}

void nameStreets(Layout& l, const Site& site)
{
    LILA_ZONE();

    l.culture = site.culture.empty() ? defaultCulture(l.profile.name) : site.culture;
    lila::names::requireCulture(l.culture);
    const std::string& culture = l.culture;
    // Imported real names (as hashes): a generated name that matches one is passed over like a taken one.
    const lila::names::NameHashes* avoid = l.gen && l.gen->spec && !l.gen->spec->avoid.empty() ? &l.gen->spec->avoid : nullptr;
    auto real = [&](const std::string& s) { return avoid && avoid->matches(s); };
    l.streetNames.clear();
    l.districts.clear();
    findDistricts(l);

    const StreetGraph& g = l.graph;
    std::map<uint64_t, std::string> given;
    for (const StreetOverride& o : site.overrides)
        if (!o.name.empty())
            given[o.street] = o.name;
    std::map<int, std::string> pathNames;
    for (const SitePath& p : site.paths)
        if (!p.name.empty())
            pathNames[p.id] = p.name;

    struct Named
    {
        uint64_t key;
        std::vector<int> edges;
        std::string name;
        bool fixed = false; // given by the user, or a grid number: shared names are meant
    };
    std::vector<Named> runs;
    const double gridRad = glm::radians(l.profile.gridAngle);
    const GridAxes ax{{std::cos(gridRad), std::sin(gridRad)}, {-std::sin(gridRad), std::cos(gridRad)}};
    const bool grid = culture == "usGrid";
    for (std::vector<int>& edges : strokes(g))
    {
        Named n;
        n.key = ~uint64_t(0);
        uint64_t givenStreet = ~uint64_t(0);
        for (int e : edges)
        {
            const StreetEdge& x = g.edges[size_t(e)];
            n.key = std::min(n.key, hash64(x.street));
            if (auto it = given.find(x.street); it != given.end() && x.street < givenStreet)
            {
                givenStreet = x.street;
                n.name = it->second;
            }
        }
        if (n.name.empty())
            for (int e : edges)
                if (auto it = pathNames.find(g.edges[size_t(e)].path); it != pathNames.end())
                {
                    n.name = it->second;
                    break;
                }
        if (n.name.empty() && grid && g.edges[size_t(edges.front())].cls <= StreetClass::Local)
            n.name = gridName(l, edges, ax);
        n.fixed = !n.name.empty();
        n.edges = std::move(edges);
        runs.push_back(std::move(n));
    }
    // Lowest key first, so a street keeps its name whatever is added elsewhere unless two pick the same.
    std::sort(runs.begin(), runs.end(), [](const Named& a, const Named& b) { return a.key < b.key; });
    std::set<std::string> taken;
    for (const Named& n : runs)
        if (n.fixed)
            taken.insert(n.name);
    for (Named& n : runs)
    {
        if (n.fixed)
            continue;
        const StreetClass cls = g.edges[size_t(n.edges.front())].cls;
        for (int attempt = 0; attempt < 64 && n.name.empty(); ++attempt)
            if (std::string s = cultureStreetName(culture, cls, n.key, attempt); !taken.count(s) && !real(s))
                n.name = std::move(s);
        if (n.name.empty())
            n.name = cultureStreetName(culture, cls, n.key, 0) + " " + std::to_string(n.key % 1000);
        taken.insert(n.name);
    }
    for (const Named& n : runs)
        for (int e : n.edges)
            l.streetNames[g.edges[size_t(e)].street] = n.name;

    std::map<uint64_t, std::string> districtGiven(site.districtNames.begin(), site.districtNames.end());
    std::set<std::string> districtTaken;
    for (const District& d : l.districts)
        if (auto it = districtGiven.find(d.id); it != districtGiven.end())
            districtTaken.insert(it->second);
    for (District& d : l.districts)
    {
        if (auto it = districtGiven.find(d.id); it != districtGiven.end())
        {
            d.name = it->second;
            continue;
        }
        if (grid && contains(d.outline.pts, l.center))
            d.name = "Downtown";
        for (int attempt = 0; attempt < 64 && d.name.empty(); ++attempt)
            if (std::string s = cultureDistrictName(culture, d.id, attempt); !districtTaken.count(s) && !real(s))
                d.name = std::move(s);
        if (d.name.empty())
            d.name = cultureDistrictName(culture, d.id, 0) + " " + std::to_string(d.id % 100);
        districtTaken.insert(d.name);
    }
}

std::string streetName(const Layout& l, uint64_t street)
{
    auto it = l.streetNames.find(street);
    return it != l.streetNames.end() ? it->second : std::string();
}
} // namespace settlement

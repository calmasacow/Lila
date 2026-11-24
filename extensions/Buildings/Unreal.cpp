// buildingSend: building meshes to Unreal. Shells go as Nanite static meshes placed where they stand (each cell mesh lies
// inside one of Settlement's World Partition cells), modules as slots keyed by slot id (W0's path in
// unrealPlaceInstances), the HLOD and far proxies as meshes of their own, and MF_LilaBuildingVariation with M_LilaBuilding
// and an instance per style, assigned to the shells' building material slots.

#include "BuildingsScene.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/GeometryNodes.h>
#include <lila/Node.h>
#include <lila/Profile.h>
#include <lila/UnrealLink.h>

#include <set>

using namespace lila;

namespace buildings
{
namespace
{
/** The material instance for a style's shells. */
json styleInstance(const Style& s)
{
    return {{"name", "MI_Bld_" + s.id},
            {"scalars", {{"DirtAmount", 1.0}, {"PaletteStrength", 1.0}}},
            {"vectors", {{"DirtColor", {0.12, 0.1, 0.08}}, {"BaseColor", {0.8, 0.8, 0.8}}}}};
}
} // namespace

void registerUnreal(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "buildingSend",
        .doc = "Send building meshes to Unreal: shells as Nanite static meshes placed where they stand (one per cell mesh, inside "
               "Settlement's World Partition cells), modules as slots keyed by slot id (actors of a slot's Unreal class, instances "
               "of its Unreal mesh, or its placeholder), the HLOD and far proxies (proxies=True), and MF_LilaBuildingVariation with "
               "M_LilaBuilding and MI_Bld_<style> per style, assigned to the shells' building slots. Per-building colour and dirt "
               "travel in vertex colour. dryRun returns what would be sent. Returns the placement plan and the materials.",
        .positionalDoc = "building meshes (default every one)",
        .flags = {{"folder", "f", ArgType::String, "/Game/Lila/Buildings", "Content folder for the shells and module meshes."},
                  {"proxies", "px", ArgType::Bool, true, "Make and send the HLOD and far proxies too."},
                  {"hlodLayer", "hl", ArgType::String, "", "HLOD layer asset for the shells."},
                  {"dryRun", "dr", ArgType::Bool, false, "Only return what would be sent."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            scene::Graph& g = scene::Graph::current();
            g.evaluate();
            std::vector<std::string> meshes;
            for (const json& p : a.positional)
                if (p.is_string())
                    meshes.push_back(p.get<std::string>());
            std::set<std::string> styles;
            for (const auto& p : allPublished())
            {
                scene::Node* mod = g.node(scene::NodeId(p->modifier));
                if (!mod)
                    continue;
                const scene::NodeId mesh = scene::modifiers::meshOf(g, mod->id());
                if (mesh == scene::kNoNode)
                    continue;
                if (a.positional.empty())
                    meshes.push_back(g.node(mesh)->path());
                for (const auto& r : p->results)
                    styles.insert(r->style);
            }
            if (meshes.empty())
                throw CommandError("buildingSend: no building meshes (buildingCreate makes them)");
            json instances = json::array();
            for (const std::string& s : styles)
                if (std::shared_ptr<const Style> st = findStyle(s))
                    instances.push_back(styleInstance(*st));
            json out = {{"meshes", meshes}, {"instances", instances}};
            if (a.get<bool>("dryRun", false))
                return out;
            std::vector<std::string> send = meshes;
            if (a.get<bool>("proxies", true))
            {
                json args = json::array();
                for (const std::string& m : meshes)
                    args.push_back(m);
                const json made = ops::execute("buildingProxies", {{"args", args}});
                for (const json& p : made["proxies"])
                    send.push_back(p["mesh"].get<std::string>());
                out["proxies"] = made["proxies"].size();
            }
            try
            {
                link::Link::get().ensureConnected();
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("buildingSend: Unreal is not connected (") + e.what() + ")");
            }
            const json features = link::Link::get().status().value("features", json::array());
            if (!features.is_array() || std::find(features.begin(), features.end(), json("buildings")) == features.end())
                throw CommandError("buildingSend: this LilaLink has no buildings feature; update the plugin");
            json args = json::array();
            for (const std::string& m : send)
                args.push_back(m);
            const json plan = ops::execute("unrealPlaceInstances", {{"args", args},
                                                                    {"folder", a.get<std::string>("folder", "/Game/Lila/Buildings")},
                                                                    {"nanite", true},
                                                                    {"hlodLayer", a.get<std::string>("hlodLayer", "")},
                                                                    {"wait", true}});
            out["plan"] = plan;
            // The shells' assets: their building slots get their style's instance.
            json assets = json::array();
            for (const json& grp : plan.value("groups", json::array()))
                if (grp.contains("objectPath") && !grp.contains("label"))
                    assets.push_back(grp["objectPath"]);
            auto client = link::Link::get().client();
            link::RpcResult r = client->call("buildingMaterials", {{"instances", instances}, {"assets", assets}}, link::importCallOptions());
            if (!r.ok)
                throw CommandError("buildingSend: buildingMaterials: " + r.error);
            out["materials"] = r.result;
            return out;
        },
        .examples = {"buildingSend()", "buildingSend('Town_Buildings_0_0', dryRun=True)"},
    });
}
} // namespace buildings

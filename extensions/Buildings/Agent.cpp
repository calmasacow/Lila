// The agent guide lila://guide/buildings: making, editing and checking procedural buildings over MCP.

#include "BuildingsScene.h"

#include <lila/Extension.h>
#include <lila/Mcp.h>

namespace buildings
{
namespace
{
constexpr const char* kGuide = R"(# Buildings

Procedural buildings from Settlement envelopes, footprint curves, OSM buildings (through Settlement's osm pattern) or a
mesh's footprint. Units cm, Y up. Every command is undoable and echoed; Python: `import lila.buildings as bld`.

## Make them

- From a settlement: `settlementBuildings('Town', via='buildings')` (or `buildingCreate(settlement='Town')`). New settlements
  can do it in one go: `settlementFromOsm('town.osm', ground='Terrain', buildingRoute='buildings')`.
- From closed curves: `buildingCreate(curve='Footprints', floors=3, style='residential')`; from a mesh:
  `buildingCreate(mesh='Block', style='warehouse')`.
- Output: a group `<settlement>_Buildings` with one live mesh per 64 m cell (four per Settlement World Partition cell),
  `cell=` to change, `standalone=True` for one mesh per building.
- Styles: `buildingStyles()` lists them; `buildingStyles(style='colonialCentro')` gives the JSON to copy.
  Settlement's style guesses map onto them (mansion -> narcoMansion, police program -> policeStation).

## Ids

- Building id: `building:<lot key hex>` (Settlement lotId), or a hash of the curve/OSM id or mesh.
- Slot id: `<building id>/t<tier>/f<facade>/l<floor>/b<bay>/<kind>`, e.g. `building:1a2b/t0/f0/l0/b2/door`.
  Facades are footprint edges (f3x is a chamfered corner, h0f2 a courtyard edge); `t<n>/roof`, `t0/f1/cornice`,
  `t0/f0/l0/sign`, `site/gate`, `site/pool`.
- Ids stay the same when anything else changes; running buildingCreate again keeps edits, slot overrides and pins.

## Edit

- `buildingInfo(id, bays=True, modules=True)`: style, floors, palette, facades, bays (paths and corners), modules
  with slot ids, floor plates, core, openings.
- `buildingEdit(id, floors=3, style='malecon', seed=2, bayWidth=300, roof='hip', palette={'render': '#e4572e'})`.
- Bays: `buildingEdit(id, bays={'t0/f0/l0/b2': 'door'})` (window, door, shopfront, shopdoor, wall, arcade, garage);
  parts: `toggles={'t0/f0/l1/b1': {'balcony': True}}` (balcony, awning, ac, grille, shutter); `facadeBays={'t0/f0': 5}`;
  `clearBays=[...]`, `clear=True`.
- District: `buildingStyle(polygon=[[x, z], ...], style='colonialCentro')`; `buildingAge(polygon=..., dirt=0.7)` or
  `preset='Grunge'` (Weathering).
- Swap parts with W0 slots: `slotRule('add', type='sign.storefront', poolTag='fictional_signs')`,
  `slotSet(id='building:1a2b/t0/f0/l0/b2/door', unrealClass='/Game/BP/BP_Door.BP_Door_C', data={'locked': True})`.
  Shell parts (wall.bay, cornice, pilaster, plinth, roof.*) drop out of the shell when a rule gives them a mesh.
- Pin a building so regeneration leaves it: `slotPin('owner:building:1a2b')`.
- Sign text is in each sign slot's data (`text`), from W1's fictional business names; never a real name.

## Check and ship

- `buildingCheck()`: modulesIntersecting, doorNotReachingGround, overlappingNeighbours, slotFloating, realName.
- LOD: `buildingLod(lod='far')` per mesh; `buildingProxies()` makes `<mesh>_HLOD` and `<mesh>_Far` for export.
- Unreal: `buildingSend()` sends shells (Nanite), proxies, slots and per-building colours (MF_LilaBuildingVariation).
- Trim sheet: `buildingTrimSheet()` copies and bakes LilaBuilding.lilatrim.

## Interiors (W4) and site kits (W5)

- `buildingInfo` gives `plates` (floor outlines per floor), `core` (stair and lift position and size), `openings` per
  facade (bay, kind, slot, origin, normal, size) and `enterable`; `buildingEdit(id, enterable=True)` marks one.
- Site kits use the styles (mausoleum, portShed, packingPlant, hangar) through `buildingCreate(envelopes=[...], style=...)`.
)";
} // namespace

void registerAgent(lila::ext::Context& ctx)
{
    lila::mcp::Server::get().addResource(ctx, {.uri = "lila://guide/buildings",
                                               .name = "Buildings guide",
                                               .description = "Procedural buildings: make them from settlements, curves or OSM, ids, edits, bay paint, "
                                                              "styles, slots, checks, LOD and Unreal. Read before making or editing buildings.",
                                               .read = [] { return std::string(kGuide); }});
}
} // namespace buildings

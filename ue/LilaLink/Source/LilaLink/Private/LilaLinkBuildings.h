#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * buildingMaterials: MF_LilaBuildingVariation (the per-building palette Lila writes into vertex colour, RGB the painted
 * colour and A the dirt, laid over a trim sheet or any base colour) with M_LilaBuilding built on it, made when missing,
 * older or regenerate; and a material instance per entry of instances (one per building style).
 */
void AddBuildingMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink

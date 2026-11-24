#pragma once

#include "LilaLinkServer.h"

class UMaterialExpression;

namespace LilaLink
{
struct FFunctionGraph;

/**
 * The instance colour Lila sends as custom data 0-3 (linear RGB, amount), as a float4 in G: per-instance custom data on
 * ISM/HISM instances, else the primitive's custom data (actors), through the vector parameter LilaInstanceColor.
 * Zero where nothing was sent.
 */
UMaterialExpression* InstanceColorData(FFunctionGraph& G, int32 X);

/**
 * instanceColorMaterials {regenerate?, assets?: [static mesh paths]}: MF_LilaInstanceColor (the base colour replaced or
 * tinted by the instance colour) and M_LilaInstanceColor on it, built when missing, older or regenerate; every material
 * slot of the given meshes gets M_LilaInstanceColor. Returns {function, material, assigned, created, warnings}.
 */
void AddInstanceColorMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink

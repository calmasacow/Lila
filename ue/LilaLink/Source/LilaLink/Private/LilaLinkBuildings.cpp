#include "LilaLinkBuildings.h"

#include "LilaLinkLog.h"
#include "LilaLinkMaterialFunction.h"
#include "LilaLinkMethods.h"

#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Engine/StaticMesh.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
constexpr int32 Version = 2;
const TCHAR* VersionTag = TEXT("Lila Building v");
const TCHAR* InstanceFolder = TEXT("/Game/LilaLink/Buildings");

TSharedPtr<FJsonValue> Obj(const TSharedRef<FJsonObject>& O) { return MakeShared<FJsonValueObject>(O); }
TSharedPtr<FJsonValue> Str(const FString& S) { return MakeShared<FJsonValueString>(S); }

int32 VersionOf(const UMaterialFunction* Function)
{
	for (UMaterialExpression* E : Function->GetExpressions())
	{
		if (const UMaterialExpressionCustom* C = Cast<UMaterialExpressionCustom>(E); C && C->Description.StartsWith(VersionTag))
		{
			return FCString::Atoi(*C->Description.RightChop(FCString::Strlen(VersionTag)));
		}
	}
	return 0;
}

using FWire = TPair<FString, TPair<UMaterialExpression*, FString>>;
FWire Wire(const FString& Name, UMaterialExpression* E, const FString& Output = FString()) { return MakeTuple(Name, MakeTuple(E, Output)); }

/** A Custom node running Code over named inputs; every one carries the version tag VersionOf reads. */
UMaterialExpressionCustom* Custom(LilaLink::FFunctionGraph& G, const FString& Description, const FString& Code, ECustomMaterialOutputType Type,
	const TArray<FWire>& Wires, int32 X = -400)
{
	auto* E = G.New<UMaterialExpressionCustom>(X);
	E->Description = FString::Printf(TEXT("%s%d %s"), VersionTag, Version, *Description);
	E->Code = Code;
	E->OutputType = Type;
	E->Inputs.Reset();
	for (const FWire& W : Wires)
	{
		E->Inputs.AddDefaulted_GetRef().InputName = FName(*W.Key);
	}
	for (const FWire& W : Wires)
	{
		UMaterialEditingLibrary::ConnectMaterialExpressions(W.Value.Key, W.Value.Value, E, W.Key);
	}
	return E;
}

/**
 * MF_LilaBuildingVariation: the building's painted colour (vertex colour RGB, one colour per building and part as Lila's
 * palette picked it) multiplied into a grey base such as the LilaBuilding trim sheet's albedo, and dirt (vertex alpha)
 * laid over it, broken up by smooth world-space noise (two octaves at DirtScale). Drop it into your own material between the base colour
 * and the output; the generated M_LilaBuilding does exactly that.
 */
void BuildVariation(UMaterialFunction* Function)
{
	LilaLink::FFunctionGraph G(Function, TEXT("Lila Building Variation: per-building painted colour and dirt from the vertex colour Lila's "
											  "Buildings writes (RGB colour, A dirt), over your base colour or the LilaBuilding trim sheet."));
	UMaterialExpression* Base = G.VectorInput(TEXT("BaseColor"), FLinearColor(0.8f, 0.8f, 0.8f), TEXT("Surface"),
		TEXT("The surface's own colour, grey where the palette should show (a trim sheet's albedo)."));
	UMaterialExpression* BaseRough = G.ScalarInput(TEXT("BaseRoughness"), 0.85f, TEXT("Surface"), TEXT("The surface's own roughness."));
	auto* VC = G.New<UMaterialExpressionVertexColor>(-1600);
	UMaterialExpression* VCDefault = Custom(G, TEXT("vertex colour"), TEXT("return float4(VC, VA);"), CMOT_Float4,
		{Wire(TEXT("VC"), VC), Wire(TEXT("VA"), VC, TEXT("A"))}, -1400);
	UMaterialExpression* Palette = G.Input(TEXT("BuildingPalette"), FunctionInput_Vector4, VCDefault,
		TEXT("Painted colour (RGB, linear) and dirt (A): vertex colour when unconnected."));
	UMaterialExpression* Strength = G.ScalarInput(TEXT("PaletteStrength"), 1.0f, TEXT("Palette"), TEXT("0 keeps the base colour, 1 paints it fully."));
	UMaterialExpression* DirtColor = G.VectorInput(TEXT("DirtColor"), FLinearColor(0.12f, 0.1f, 0.08f), TEXT("Dirt"), TEXT("Colour of grime."));
	UMaterialExpression* DirtAmount = G.ScalarInput(TEXT("DirtAmount"), 1.0f, TEXT("Dirt"), TEXT("Scales the dirt Lila wrote per building."));
	UMaterialExpression* DirtScale = G.ScalarInput(TEXT("DirtScale"), 150.0f, TEXT("Dirt"), TEXT("World size of the dirt breakup, cm."));
	auto* World = G.New<UMaterialExpressionWorldPosition>(-1600);
	UMaterialExpression* Result = Custom(G, TEXT("palette and dirt"),
		TEXT("float3 painted = lerp(Base, Base * P.rgb * 1.25, saturate(S));\n"
			 "float3 q = W / max(Scale, 1.0);\n"
			 "float n = 0.0;\n"
			 "for (int o = 0; o < 2; ++o)\n"
			 "{\n"
			 "    float3 i = floor(q), f = frac(q);\n"
			 "    f = f * f * (3.0 - 2.0 * f);\n"
			 "    float c[8];\n"
			 "    for (int k = 0; k < 8; ++k)\n"
			 "        c[k] = frac(sin(dot(i + float3(k & 1, (k >> 1) & 1, k >> 2), float3(127.1, 311.7, 74.7))) * 43758.5453);\n"
			 "    n += lerp(lerp(lerp(c[0], c[1], f.x), lerp(c[2], c[3], f.x), f.y), lerp(lerp(c[4], c[5], f.x), lerp(c[6], c[7], f.x), f.y), f.z)\n"
			 "         * (o == 0 ? 0.65 : 0.35);\n"
			 "    q = q * 2.7 + 13.0;\n"
			 "}\n"
			 "float d = saturate(P.a * A * (0.55 + 0.45 * n));\n"
			 "return float4(lerp(painted, Dirt, d), d);"),
		CMOT_Float4,
		{Wire(TEXT("Base"), Base), Wire(TEXT("P"), Palette), Wire(TEXT("S"), Strength), Wire(TEXT("Dirt"), DirtColor), Wire(TEXT("A"), DirtAmount),
			Wire(TEXT("Scale"), DirtScale), Wire(TEXT("W"), World)});
	UMaterialExpression* Color = Custom(G, TEXT("colour"), TEXT("return R.rgb;"), CMOT_Float3, {Wire(TEXT("R"), Result)}, -200);
	UMaterialExpression* Rough = Custom(G, TEXT("roughness"), TEXT("return saturate(Rough + R.a * 0.1);"), CMOT_Float1,
		{Wire(TEXT("R"), Result), Wire(TEXT("Rough"), BaseRough)}, -200);
	UMaterialExpression* Dirt = Custom(G, TEXT("dirt"), TEXT("return R.a;"), CMOT_Float1, {Wire(TEXT("R"), Result)}, -200);
	G.Output(Color, TEXT("BaseColor"), TEXT("The base colour painted and dirtied."));
	G.Output(Rough, TEXT("Roughness"), TEXT("Roughness, a touch rougher where dirty."));
	G.Output(Dirt, TEXT("Dirt"), TEXT("The dirt mask, 0 to 1, for your own layers."));
	G.Finish();
}

void Master(UMaterial* Material, UMaterialFunction* Function, TArray<FString>& OutWarnings)
{
	UMaterialExpressionMaterialFunctionCall* Call = LilaLink::ResetToFunctionCall(Material, Function);
	if (!LilaLink::ConnectFunctionOutputs(Call, {{TEXT("BaseColor"), MP_BaseColor}, {TEXT("Roughness"), MP_Roughness}}))
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not wire the outputs of %s into %s"), *Function->GetName(), *Material->GetName()));
	}
	Material->SetUsageByFlag(MATUSAGE_Nanite, true);
	Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
	Material->PostEditChange();
	Material->MarkPackageDirty();
}

UMaterialInstanceConstant* InstanceFor(const FString& Name, UMaterialInterface* Parent, TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	const FString Path = FString(InstanceFolder) + TEXT("/") + Name + TEXT(".") + Name;
	UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Instance)
	{
		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>(GetTransientPackage());
		Factory->InitialParent = Parent;
		Instance = Cast<UMaterialInstanceConstant>(
			FAssetToolsModule::GetModule().Get().CreateAsset(Name, InstanceFolder, UMaterialInstanceConstant::StaticClass(), Factory));
	}
	if (!Instance)
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not create material instance %s in %s"), *Name, InstanceFolder));
		return nullptr;
	}
	if (Instance->Parent != Parent)
	{
		UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Parent);
	}
	OutCreated.AddUnique(Instance->GetPathName());
	return Instance;
}

/**
 * buildingMaterials {regenerate?, instances?: [{name, scalars {name: v}, vectors {name: [r, g, b]}}]}: builds
 * MF_LilaBuildingVariation and M_LilaBuilding when missing or older, then one instance per entry. Returns {function,
 * material, instances, created, warnings}.
 */
TSharedPtr<FJsonValue> BuildingMaterials(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkBuildings::BuildingMaterials);

	bool bRegenerate = false;
	Params->TryGetBoolField(TEXT("regenerate"), bRegenerate);
	TArray<FString> Created, Warnings;
	FString Error;
	const LilaLink::FMaterialPair Pair =
		LilaLink::FindOrCreateMaterialPair(LilaLink::MastersFolderPath(), TEXT("MF_LilaBuildingVariation"), TEXT("M_LilaBuilding"), Error);
	if (!Pair.Material)
	{
		OutError = TEXT("buildingMaterials: ") + Error;
		return nullptr;
	}
	if (Pair.bCreated || bRegenerate || VersionOf(Pair.Function) < Version)
	{
		BuildVariation(Pair.Function);
		Master(Pair.Material, Pair.Function, Warnings);
		Created.AddUnique(Pair.Function->GetPathName());
		Created.AddUnique(Pair.Material->GetPathName());
	}
	TSharedRef<FJsonObject> Instances = MakeShared<FJsonObject>();
	const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
	if (Params->TryGetArrayField(TEXT("instances"), List))
	{
		for (const TSharedPtr<FJsonValue>& V : *List)
		{
			const TSharedPtr<FJsonObject> In = V->AsObject();
			if (!In.IsValid())
			{
				continue;
			}
			const FString Name = In->GetStringField(TEXT("name"));
			UMaterialInstanceConstant* Instance = InstanceFor(Name, Pair.Material, Created, Warnings);
			if (!Instance)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* Scalars = nullptr;
			if (In->TryGetObjectField(TEXT("scalars"), Scalars))
			{
				for (const auto& [Key, Value] : (*Scalars)->Values)
				{
					UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, FName(*Key), float(Value->AsNumber()));
				}
			}
			const TSharedPtr<FJsonObject>* Vectors = nullptr;
			if (In->TryGetObjectField(TEXT("vectors"), Vectors))
			{
				for (const auto& [Key, Value] : (*Vectors)->Values)
				{
					const TArray<TSharedPtr<FJsonValue>>& C = Value->AsArray();
					if (C.Num() >= 3)
					{
						UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, FName(*Key),
							FLinearColor(float(C[0]->AsNumber()), float(C[1]->AsNumber()), float(C[2]->AsNumber())));
					}
				}
			}
			UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
			Instances->SetStringField(Name, Instance->GetPathName());
		}
	}
	// Shell assets: slots named Bld_<style>_<part><k> take their style's instance, Bld_c<hex> (a given colour) the material.
	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	if (Params->TryGetArrayField(TEXT("assets"), Assets))
	{
		for (const TSharedPtr<FJsonValue>& V : *Assets)
		{
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *V->AsString(), nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!Mesh)
			{
				Warnings.Add(FString::Printf(TEXT("No static mesh %s to assign building materials to"), *V->AsString()));
				continue;
			}
			bool bChanged = false;
			TArray<FStaticMaterial>& Slots = Mesh->GetStaticMaterials();
			for (int32 K = 0; K < Slots.Num(); ++K)
			{
				const FString Slot = Slots[K].MaterialSlotName.ToString();
				if (!Slot.StartsWith(TEXT("Bld_"), ESearchCase::CaseSensitive))
				{
					continue;
				}
				FString Style = Slot.RightChop(4);
				int32 Cut = INDEX_NONE;
				if (Style.FindChar(TCHAR('_'), Cut))
				{
					Style = Style.Left(Cut);
				}
				UMaterialInterface* Use = Pair.Material;
				FString InstancePath;
				if (Instances->TryGetStringField(TEXT("MI_Bld_") + Style, InstancePath))
				{
					if (UMaterialInterface* MI = LoadObject<UMaterialInterface>(nullptr, *InstancePath, nullptr, LOAD_NoWarn | LOAD_Quiet))
					{
						Use = MI;
					}
				}
				if (Slots[K].MaterialInterface != Use)
				{
					Slots[K].MaterialInterface = Use;
					bChanged = true;
				}
			}
			if (bChanged)
			{
				Mesh->Modify();
				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();
				Created.AddUnique(Mesh->GetPathName());
			}
		}
	}
	TArray<FString> SaveErrors;
	LilaLink::SaveAssetPaths(Created, SaveErrors);
	Warnings.Append(SaveErrors);
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("function"), Pair.Function->GetPathName());
	Out->SetStringField(TEXT("material"), Pair.Material->GetPathName());
	Out->SetObjectField(TEXT("instances"), Instances);
	TArray<TSharedPtr<FJsonValue>> C, W;
	for (const FString& P : Created)
	{
		C.Add(Str(P));
	}
	for (const FString& P : Warnings)
	{
		W.Add(Str(P));
		UE_LOG(LogLilaLink, Warning, TEXT("buildingMaterials: %s"), *P);
	}
	Out->SetArrayField(TEXT("created"), C);
	Out->SetArrayField(TEXT("warnings"), W);
	return Obj(Out);
}
} // namespace

void LilaLink::AddBuildingMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("buildingMaterials"), LilaLink::Sync(&BuildingMaterials));
}

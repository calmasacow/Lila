#include "LilaLinkProjectConfig.h"

#include "LilaLinkLog.h"

#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/Object.h"

FString LilaLink::UpdateProjectIni(UObject* Settings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkProjectConfig::UpdateProjectIni);

	if (!Settings)
	{
		return FString();
	}
	const FString File = FPaths::ConvertRelativePathToFull(Settings->GetDefaultConfigFilename());
	FString Before;
	const bool bExisted = FFileHelper::LoadFileToString(Before, *File);
	Settings->TryUpdateDefaultConfigFile();
	FString After;
	FFileHelper::LoadFileToString(After, *File);
	if (bExisted && Before == After)
	{
		return FString();
	}
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("LilaLink/ConfigBackups"));
	IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
	// Several saves within a second (settings classes sharing DefaultPlugins.ini) each keep their own copy.
	const FString Stem = Dir / FPaths::GetBaseFilename(File) + TEXT("-") + FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
	const TCHAR* Extension = bExisted ? TEXT(".ini") : TEXT(".absent");
	FString Backup = Stem + Extension;
	for (int32 N = 2; IFileManager::Get().FileExists(*Backup); ++N)
	{
		Backup = FString::Printf(TEXT("%s-%d%s"), *Stem, N, Extension);
	}
	const bool bSaved = bExisted ? FFileHelper::SaveStringToFile(Before, *Backup)
								 : FFileHelper::SaveStringToFile(FString::Printf(TEXT("%s did not exist before LilaLink wrote it: delete it to go back."), *File), *Backup);
	UE_LOG(LogLilaLink, Log, TEXT("%s changed; it was %s"), *File, bSaved ? *(TEXT("backed up to ") + Backup) : TEXT("not backed up (Saved is not writable)"));
	return bSaved ? Backup : FString();
}


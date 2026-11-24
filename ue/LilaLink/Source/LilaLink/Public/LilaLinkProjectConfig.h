#pragma once

#include "CoreMinimal.h"

class UObject;

/** Project config LilaLink and its feature plugins write, backed up before a change. */
namespace LilaLink
{
/**
 * Saves Settings to its project ini (TryUpdateDefaultConfigFile); when that changes the file, the file as it was is kept
 * in Saved/LilaLink/ConfigBackups first (a .absent note when it did not exist). Returns the backup's path, empty when the
 * file did not change.
 */
LILALINK_API FString UpdateProjectIni(UObject* Settings);
} // namespace LilaLink

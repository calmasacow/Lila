# Builds Release, tests it, builds the installer and publishes a GitHub release with latest.json for the updater.
# HEAD must be committed and pushed; this never pushes. -DryRun stops before publishing.
param(
    [string]$Notes = "",
    # Markdown notes for the GitHub release page; -Notes still goes into latest.json for the updater.
    [string]$NotesFile = "",
    [string]$BuildDir = "build",
    [switch]$DryRun,
    [switch]$SkipBuild,
    [switch]$SkipTests,
    # Also upload the staged build to Steam (scripts\steam\upload.ps1).
    [switch]$Steam
)
$ErrorActionPreference = "Stop"
$repo = Resolve-Path "$PSScriptRoot\..\.."
Set-Location $repo

function Find-Gh {
    $cmd = Get-Command gh -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $link = "$env:LOCALAPPDATA\Microsoft\WinGet\Links\gh.exe"
    if (Test-Path $link) { return $link }
    throw "GitHub CLI not found. Install it with: winget install GitHub.cli"
}

$version = (Select-String -Path "$repo\CMakeLists.txt" -Pattern 'project\(Lila VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$tag = "v$version"
"Releasing Lila $version ($tag)"

$head = git rev-parse HEAD
# A dry run only builds, so it works on uncommitted or unpublished work.
if (-not $DryRun) {
    if (git status --porcelain) { throw "Commit or stash your changes first: a release is built from HEAD." }
    git fetch --quiet origin
    if (-not (git branch -r --contains $head)) { throw "HEAD $head isn't on origin yet. Push it, then run this again." }
    if (git ls-remote --tags origin $tag) { throw "$tag already exists. Bump the version in CMakeLists.txt first." }
    & "$repo\scripts\release\symbols.ps1" -Check
}

if (-not $SkipBuild) {
    cmd /c "scripts\env.cmd && cmake --build $BuildDir --config Release"
    if ($LASTEXITCODE -ne 0) { throw "Release build failed. If Lila is running from $BuildDir\bin\Release, close it." }
}
if (-not $SkipTests) {
    cmd /c "scripts\env.cmd >nul 2>&1 && python scripts\quiet.py test $BuildDir Release -LE known_failing"
    if ($LASTEXITCODE -ne 0) { throw "Tests failed, full log in $BuildDir\last_test.log" }
}

& "$repo\scripts\installer\build.ps1" -Config Release -BuildDir $BuildDir -Notes $Notes
$exe = "$repo\build-installer\Lila-$version-Setup.exe"
$manifest = "$repo\build-installer\latest.json"

if ($DryRun) {
    "Dry run: built $exe and $manifest; nothing published."
    return
}

& "$repo\scripts\release\symbols.ps1" -Config Release -BuildDir $BuildDir

$gh = Find-Gh
$notesArgs = if ($NotesFile) { @("--notes-file", $NotesFile) } elseif ($Notes) { @("--notes", $Notes) } else { @("--generate-notes") }
& $gh release create $tag $exe $manifest --target $head --title "Lila $version" @notesArgs
if ($LASTEXITCODE -ne 0) { throw "gh release create failed. Run 'gh auth login' if you haven't." }

if ($Steam) {
    & "$repo\scripts\steam\upload.ps1" -Config Release -BuildDir $BuildDir -SkipBuild -SkipSymbols
}
"Released $tag"

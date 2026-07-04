param(
    [string]$Version = "",
    [string]$OutDir = "",
    [string]$ExePath = "",
    [string]$RuntimeRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [switch]$IncludeRuntimeSource = $false
)

$ErrorActionPreference = "Stop"

function Resolve-ProjectVersion {
    param(
        [string]$Requested,
        [string]$Root
    )
    if (-not [string]::IsNullOrWhiteSpace($Requested)) {
        return $Requested
    }
    if (Get-Command git -ErrorAction SilentlyContinue) {
        $exact = & git -C $Root describe --tags --exact-match 2>$null
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($exact)) {
            return $exact.Trim()
        }
        $described = & git -C $Root describe --tags --dirty --always 2>$null
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($described)) {
            return $described.Trim()
        }
    }
    return "unversioned"
}

function Copy-IfExists {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    if (Test-Path $Source -PathType Leaf) {
        Copy-Item -Force $Source $Destination
    }
}

$Version = Resolve-ProjectVersion -Requested $Version -Root $RuntimeRoot
Write-Host "Package version: $Version"

if (-not $OutDir) { $OutDir = Join-Path $RuntimeRoot ".build\package" }
$ArtifactName = "meccha-camouflage-$Version"
if (-not $ExePath) { $ExePath = Join-Path $RuntimeRoot ".build\bin\meccha-camouflage.exe" }
if (-not (Test-Path $ExePath -PathType Leaf)) { throw "Executable not found: $ExePath. Run scripts/build.ps1 first." }

$ExeDir = Split-Path -Parent (Resolve-Path $ExePath).Path
$NativeDir = Join-Path $ExeDir "native"
$MeshProfilesDir = Join-Path $ExeDir "mesh-profiles"
foreach ($required in @(
    (Join-Path $NativeDir "runtime-bridge.dll"),
    (Join-Path $NativeDir "runtime-injector.exe"),
    (Join-Path $MeshProfilesDir "paintman.mesh-profile-v2.json")
)) {
    if (-not (Test-Path $required -PathType Leaf)) {
        throw "Required packaged runtime file is missing: $required"
    }
}

$TmpRoot = Join-Path $OutDir "tmp-release"
Remove-Item -Recurse -Force $TmpRoot -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $TmpRoot | Out-Null

Copy-Item -Recurse -Force -Path (Join-Path $ExeDir "*") -Destination $TmpRoot
Copy-IfExists -Source (Join-Path $RuntimeRoot "README.md") -Destination (Join-Path $TmpRoot "README.md")
Copy-IfExists -Source (Join-Path $RuntimeRoot "LICENSE.txt") -Destination (Join-Path $TmpRoot "LICENSE.txt")
Copy-IfExists -Source (Join-Path $RuntimeRoot "BRANDING.md") -Destination (Join-Path $TmpRoot "BRANDING.md")
$AssetOutDir = Join-Path $TmpRoot "assets"
New-Item -ItemType Directory -Force -Path $AssetOutDir | Out-Null
Copy-IfExists -Source (Join-Path $RuntimeRoot "assets\icon.png") -Destination (Join-Path $AssetOutDir "icon.png")

Set-Content -Encoding ASCII -Path (Join-Path $TmpRoot "runtime-config.json") -Value @'
{
  "version": "%VERSION%",
  "runtime": "wpf",
  "mode": "service",
  "game_process_name": "PenguinHotel-Win64-Shipping.exe",
  "config_dir": "%LOCALAPPDATA%\\MecchaCamouflage\\versions\\%VERSION%\\config",
  "log_dir": "%LOCALAPPDATA%\\MecchaCamouflage\\versions\\%VERSION%\\logs"
}
'@.Replace("%VERSION%", $Version)

if ($IncludeRuntimeSource) {
    Copy-Item -Recurse -Force (Join-Path $RuntimeRoot "runtime") (Join-Path $TmpRoot "runtime")
    Copy-Item -Recurse -Force (Join-Path $RuntimeRoot "scripts") (Join-Path $TmpRoot "scripts")
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$ZipPath = Join-Path $OutDir "$ArtifactName.zip"
if (Test-Path $ZipPath) { Remove-Item -Force $ZipPath }
$Zip = [System.IO.Compression.ZipFile]::Open($ZipPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    $Root = (Resolve-Path $TmpRoot).Path.TrimEnd("\", "/") + [System.IO.Path]::DirectorySeparatorChar
    Get-ChildItem $TmpRoot -Recurse -File | ForEach-Object {
        $FullPath = (Resolve-Path $_.FullName).Path
        $RelativePath = $FullPath.Substring($Root.Length).Replace("\", "/")
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($Zip, $_.FullName, $RelativePath, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally {
    $Zip.Dispose()
}

Remove-Item -Recurse -Force $TmpRoot
Write-Host "Wrote $ZipPath"

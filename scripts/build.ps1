param(
    [string]$RuntimeRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$OutDir = "",
    [string]$ExeName = "meccha-camouflage",
    [string]$Version = ""
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

function Quote-CmdArg([string]$Value) {
    if ($Value -match '^[A-Za-z0-9_./:=+\-\\]+$') {
        return $Value
    }
    return '"' + ($Value -replace '"', '\"') + '"'
}

function Get-VsDevCmd {
    $VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $VsWhere)) { return "" }
    $VsInstall = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $VsInstall) { return "" }
    $VsDevCmd = Join-Path $VsInstall "Common7\Tools\VsDevCmd.bat"
    if (Test-Path $VsDevCmd) { return $VsDevCmd }
    return ""
}

function Invoke-VsToolCommand {
    param(
        [Parameter(Mandatory = $true)][string]$ToolName,
        [Parameter(Mandatory = $true)][string[]]$ToolArgs
    )
    if (Get-Command $ToolName -ErrorAction SilentlyContinue) {
        & $ToolName @ToolArgs
        if ($LASTEXITCODE -ne 0) { throw "$ToolName failed with exit code $LASTEXITCODE" }
        return
    }
    $VsDevCmd = Get-VsDevCmd
    if (-not $VsDevCmd) {
        throw "$ToolName was not found. Install Visual Studio 2022 Build Tools or run from a VS Developer PowerShell."
    }
    $ArgText = ($ToolArgs | ForEach-Object { Quote-CmdArg $_ }) -join " "
    $CommandLine = "$(Quote-CmdArg $VsDevCmd) -arch=x64 -host_arch=x64 >nul && $ToolName $ArgText"
    cmd /d /c $CommandLine
    if ($LASTEXITCODE -ne 0) { throw "$ToolName failed with exit code $LASTEXITCODE" }
}

function Get-ExeBaseName {
    param([string]$Name)
    $candidate = (New-Object System.IO.FileInfo($Name)).BaseName
    if ([string]::IsNullOrWhiteSpace($candidate)) { return "meccha-camouflage" }
    return $candidate
}

function Invoke-DotNet {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    & dotnet @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "dotnet $($Arguments -join ' ') failed with exit code $LASTEXITCODE"
    }
}

function Clear-DirectoryContents {
    param([Parameter(Mandatory = $true)][string]$Path)
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
    $full = (Resolve-Path $Path).Path.TrimEnd("\", "/")
    $root = [System.IO.Path]::GetPathRoot($full).TrimEnd("\", "/")
    if ($full -eq $root) {
        throw "Refusing to clear filesystem root: $full"
    }
    Get-ChildItem -Force -LiteralPath $full | Remove-Item -Recurse -Force
}

$Version = Resolve-ProjectVersion -Requested $Version -Root $RuntimeRoot
$ExeName = Get-ExeBaseName -Name $ExeName
Write-Host "Build version: $Version"

if (-not $OutDir) {
    $OutDir = Join-Path $RuntimeRoot ".build\bin"
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
$ObjDir = Join-Path $RuntimeRoot ".build\obj"
$NativePackageDir = Join-Path $ObjDir "package-native"

$BridgeSource = Join-Path $RuntimeRoot "runtime\src\bridge.cpp"
$InjectorSource = Join-Path $RuntimeRoot "runtime\src\injector.cpp"
$WebHostProject = Join-Path $RuntimeRoot "runtime\csharp\MecchaCamouflage.WebHost\MecchaCamouflage.WebHost.csproj"
$TestsProject = Join-Path $RuntimeRoot "runtime\csharp\MecchaCamouflage.Tests\MecchaCamouflage.Tests.csproj"
$MeshProfilesSourceDir = Join-Path $RuntimeRoot "assets\mesh-profiles"

foreach ($path in @($BridgeSource, $InjectorSource, $WebHostProject, $TestsProject)) {
    if (-not (Test-Path $path -PathType Leaf)) {
        throw "Required source not found: $path"
    }
}
if (-not (Test-Path $MeshProfilesSourceDir -PathType Container)) {
    throw "Mesh profile asset directory not found: $MeshProfilesSourceDir"
}

Clear-DirectoryContents -Path $OutDir
New-Item -ItemType Directory -Force -Path $ObjDir | Out-Null
Clear-DirectoryContents -Path $NativePackageDir

Push-Location $RuntimeRoot
try {
    Invoke-DotNet -Arguments @("run", "--project", $TestsProject, "-c", "Release")

    $BridgeOutput = Join-Path $NativePackageDir "runtime-bridge.dll"
    $InjectorOutput = Join-Path $NativePackageDir "runtime-injector.exe"
    Invoke-VsToolCommand -ToolName "cl.exe" -ToolArgs @(
        "/nologo", "/std:c++17", "/EHsc", "/O2", "/LD", $BridgeSource,
        "/Fo:$(Join-Path $ObjDir 'bridge.obj')",
        "/Fe:$BridgeOutput",
        "Ws2_32.lib",
        "User32.lib"
    )
    Invoke-VsToolCommand -ToolName "cl.exe" -ToolArgs @(
        "/nologo", "/EHsc", "/O2", $InjectorSource,
        "/Fo:$(Join-Path $ObjDir 'injector.obj')",
        "/Fe:$InjectorOutput"
    )

    if (-not (Test-Path $BridgeOutput -PathType Leaf)) {
        throw "Bridge DLL was not produced: $BridgeOutput"
    }
    if (-not (Test-Path $InjectorOutput -PathType Leaf)) {
        throw "Injector EXE was not produced: $InjectorOutput"
    }

    $MeshProfiles = @(Get-ChildItem -Path $MeshProfilesSourceDir -Filter "*.json" -File)
    if ($MeshProfiles.Count -le 0) {
        throw "No mesh profile JSON assets found in: $MeshProfilesSourceDir"
    }

    Invoke-DotNet -Arguments @(
        "publish", $WebHostProject,
        "-c", "Release",
        "-r", "win-x64",
        "--self-contained", "true",
        "-o", $OutDir,
        "/p:PublishSingleFile=true",
        "/p:IncludeAllContentForSelfExtract=true",
        "/p:IncludeNativeLibrariesForSelfExtract=true",
        "/p:EnableCompressionInSingleFile=true",
        "/p:MecchaAppVersion=$Version",
        "/p:MecchaNativeRuntimeDir=$NativePackageDir",
        "/p:MecchaMeshProfilesDir=$MeshProfilesSourceDir"
    )

    $DefaultControllerOutput = Join-Path $OutDir "meccha-camouflage.exe"
    $ControllerOutput = Join-Path $OutDir "$ExeName.exe"
    if ($DefaultControllerOutput -ne $ControllerOutput -and (Test-Path $DefaultControllerOutput -PathType Leaf)) {
        Move-Item -Force $DefaultControllerOutput $ControllerOutput
    }

    if (-not (Test-Path $ControllerOutput -PathType Leaf)) {
        throw "WebView2 controller EXE was not produced: $ControllerOutput"
    }
}
finally {
    Pop-Location
}

Write-Host "Built runtime artifacts:"
Write-Host "  $(Join-Path $OutDir "$ExeName.exe")"
Write-Host "  native runtime embedded from $NativePackageDir"

param(
    [string]$RuntimeRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"

if (-not $OutDir) {
    $OutDir = Join-Path $RuntimeRoot "native\bin"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$BridgeSource = Join-Path $RuntimeRoot "native\src\meccha_xenos_bridge.cpp"
$InjectorSource = Join-Path $RuntimeRoot "native\src\meccha_xenos_injector.cpp"
if (-not (Test-Path $BridgeSource)) {
    throw "Bridge source not found: $BridgeSource"
}
if (-not (Test-Path $InjectorSource)) {
    throw "Injector source not found: $InjectorSource"
}

function Invoke-Cl {
    param([Parameter(Mandatory = $true)][string[]]$CompilerArgs)
    & cl.exe @CompilerArgs
    if ($LASTEXITCODE -ne 0) {
        throw "cl.exe failed with exit code $LASTEXITCODE"
    }
}

function Quote-CmdArg([string]$Value) {
    if ($Value -match '^[A-Za-z0-9_./:=+\-\\]+$') {
        return $Value
    }
    return '"' + ($Value -replace '"', '\"') + '"'
}

function Get-VsDevCmd {
    $VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $VsWhere)) {
        return ""
    }
    $VsInstall = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $VsInstall) {
        return ""
    }
    $VsDevCmd = Join-Path $VsInstall "Common7\Tools\VsDevCmd.bat"
    if (Test-Path $VsDevCmd) {
        return $VsDevCmd
    }
    return ""
}

function Invoke-NativeBuildCommand {
    param([Parameter(Mandatory = $true)][string[]]$CompilerArgs)
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        Invoke-Cl -CompilerArgs $CompilerArgs
        return
    }

    $VsDevCmd = Get-VsDevCmd
    if (-not $VsDevCmd) {
        throw "cl.exe was not found. Install Visual Studio 2022 Build Tools or run from a VS Developer PowerShell."
    }
    $ArgText = ($CompilerArgs | ForEach-Object { Quote-CmdArg $_ }) -join " "
    $CommandLine = "$(Quote-CmdArg $VsDevCmd) -arch=x64 -host_arch=x64 >nul && cl.exe $ArgText"
    cmd /d /c $CommandLine
    if ($LASTEXITCODE -ne 0) {
        throw "cl.exe failed with exit code $LASTEXITCODE"
    }
}

Push-Location $RuntimeRoot
try {
    $BridgeOutput = Join-Path $OutDir "meccha-xenos-bridge.dll"
    $InjectorOutput = Join-Path $OutDir "meccha-xenos-injector.exe"

    Invoke-NativeBuildCommand -CompilerArgs @(
        "/nologo", "/std:c++17", "/EHsc", "/O2", "/LD", $BridgeSource,
        "/Fe:$BridgeOutput",
        "Ws2_32.lib",
        "User32.lib"
    )
    Invoke-NativeBuildCommand -CompilerArgs @(
        "/nologo", "/EHsc", "/O2", $InjectorSource,
        "/Fe:$InjectorOutput"
    )

    if (-not (Test-Path $BridgeOutput)) {
        throw "Bridge DLL was not produced: $BridgeOutput"
    }
    if (-not (Test-Path $InjectorOutput)) {
        throw "Injector EXE was not produced: $InjectorOutput"
    }
}
finally {
    Pop-Location
}

Write-Host "Built native artifacts:"
Write-Host "  $(Join-Path $OutDir 'meccha-xenos-bridge.dll')"
Write-Host "  $(Join-Path $OutDir 'meccha-xenos-injector.exe')"

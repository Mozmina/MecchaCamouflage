param(
    [string]$GameRoot = "C:\Program Files (x86)\Steam\steamapps\common\MECCHA CHAMELEON",
    [string]$ExePath = "",
    [string]$ConfigPath = "",
    [string]$GameExecutable = "PenguinHotel-Win64-Shipping.exe",
    [string]$InstallSubDir = "Chameleon\Binaries\Win64",
    [string]$ExeName = "meccha-camouflage.exe"
)

$ErrorActionPreference = "Stop"

$RuntimeRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not (Test-Path $GameRoot -PathType Container)) {
    throw "Game root was not found or not a directory: $GameRoot"
}
if (-not $ExePath) {
    $RuntimeName = [System.IO.Path]::GetFileNameWithoutExtension($ExeName)
    $ExePath = Join-Path $RuntimeRoot ".build\native\bin\$RuntimeName.exe"
}

if (-not (Test-Path $ExePath -PathType Leaf)) {
    throw "Executable not found: $ExePath. Run scripts/build_native.ps1 first."
}

$GameBin = Join-Path $GameRoot $InstallSubDir
if (-not (Test-Path $GameBin)) {
    throw "Target game folder was not found: $GameBin"
}
$GameExe = Join-Path $GameBin $GameExecutable
if (-not (Test-Path $GameExe)) {
    throw "Expected game executable was not found: $GameExe. Ensure -GameRoot points to game root."
}

$TargetExe = Join-Path $GameBin $ExeName
$Installed = $true
try {
    Copy-Item -Force $ExePath $TargetExe
} catch {
    $Installed = $false
    $PendingExe = Join-Path $GameBin ($ExeName + ".pending.exe")
    Copy-Item -Force $ExePath $PendingExe
    $WatcherScript = Join-Path $RuntimeRoot "scripts\deploy_replace_when_unlocked.ps1"
    if (Test-Path $WatcherScript) {
        $Arguments = @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", $WatcherScript,
            "-TargetFile", $TargetExe,
            "-PendingFile", $PendingExe
        )
        Start-Process -WindowStyle Hidden -FilePath "powershell.exe" -ArgumentList $Arguments | Out-Null
        Write-Warning "Could not replace $TargetExe because it is in use. Staged new exe at $PendingExe and started a watcher."
    } else {
        Write-Warning "Could not replace $TargetExe because it is in use. Close game and re-run deploy."
    }
}

if ($ConfigPath -and (Test-Path $ConfigPath)) {
    Copy-Item -Force $ConfigPath $GameBin
}

$Hashes = @{}
if (Test-Path $TargetExe) {
    $Hashes["target"] = (Get-FileHash -Algorithm SHA256 $TargetExe).Hash.ToLowerInvariant()
}
if (Test-Path $ExePath) {
    $Hashes["source"] = (Get-FileHash -Algorithm SHA256 $ExePath).Hash.ToLowerInvariant()
}

Write-Host "Deployed:"
Write-Host "  $TargetExe"
if ($Installed) {
    Write-Host ("  sha256=$($Hashes["target"])")
} else {
    Write-Host ("  staged=$PendingExe")
}

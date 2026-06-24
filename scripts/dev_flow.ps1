param(
    [ValidateSet("all", "build", "deploy", "run", "none")]
    [string]$Action = "all",
    [string]$GameRoot = "C:\Program Files (x86)\Steam\steamapps\common\MECCHA CHAMELEON",
    [string]$RuntimeRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$ExeName = "meccha-camouflage.exe",
    [int]$LoopFrames = 10,
    [int]$FrameDelayMs = 16,
    [int]$RunForever = 1,
    [switch]$Quick,
    [string]$Adapter = "xenos",
    [string]$BridgePath = "",
    [int]$ServiceMaxFrames = 0,
    [float]$ServiceMaxDurationSeconds = 0.0,
    [string]$ServiceStopFile = "",
    [Alias("RuntimeStopKey")]
    [string]$ServiceStopKey = "",
    [string[]]$RuntimeArgs,
    [string]$RuntimeArgString = ""
)

$ErrorActionPreference = "Stop"

$BuildScript = Join-Path $PSScriptRoot "build_native.ps1"
$DeployScript = Join-Path $PSScriptRoot "deploy_to_game.ps1"
$RuntimeName = [System.IO.Path]::GetFileNameWithoutExtension($ExeName)

function Invoke-PipelineStep {
    param(
        [string]$Name,
        [scriptblock]$ScriptBlock
    )
    $global:LASTEXITCODE = 0
    & $ScriptBlock
    if (-not $?) {
        throw "$Name failed."
    }
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed with exit code $LASTEXITCODE."
    }
}

function Resolve-RuntimeExe {
    param([string]$RuntimeRoot, [string]$RuntimeName)
    $candidate = Join-Path $RuntimeRoot ".build\native\bin\$RuntimeName.exe"
    if (Test-Path $candidate) {
        return (Resolve-Path $candidate).Path
    }
    return ""
}

function Convert-RuntimeArgString {
    param([string]$RuntimeArgString)
    if ([string]::IsNullOrWhiteSpace($RuntimeArgString)) {
        return @()
    }
    $tokens = New-Object System.Collections.Generic.List[string]
    $builder = New-Object System.Text.StringBuilder
    $state = "Normal"
    $inEscape = $false
    foreach ($char in $RuntimeArgString.ToCharArray()) {
        if ($inEscape) {
            [void]$builder.Append($char)
            $inEscape = $false
            continue
        }
        if ($char -eq '\') {
            $inEscape = $true
            continue
        }
        switch ($state) {
            "SingleQuote" { if ($char -eq "'") { $state = "Normal" } else { [void]$builder.Append($char) }; continue }
            "DoubleQuote" { if ($char -eq '"') { $state = "Normal" } else { [void]$builder.Append($char) }; continue }
        }
        if ($char -eq "'") { $state = "SingleQuote"; continue }
        if ($char -eq '"') { $state = "DoubleQuote"; continue }
        if ([char]::IsWhiteSpace($char)) {
            if ($builder.Length -gt 0) {
                $tokens.Add($builder.ToString())
                $builder.Clear() | Out-Null
            }
            continue
        }
        [void]$builder.Append($char)
    }
    if ($builder.Length -gt 0) {
        $tokens.Add($builder.ToString())
    }
    return $tokens.ToArray()
}

if ($RuntimeArgString) {
    $stringArgs = Convert-RuntimeArgString -RuntimeArgString $RuntimeArgString
    $RuntimeArgs = @($stringArgs + $RuntimeArgs)
}

if ($Action -eq "build" -or $Action -eq "all") {
    Write-Host "Building native runtime exe..."
    Invoke-PipelineStep -Name "build_native.ps1" -ScriptBlock {
        & $BuildScript -RuntimeRoot $RuntimeRoot -ExeName $RuntimeName
    }
}

if ($Action -eq "deploy" -or $Action -eq "all") {
    Write-Host "Deploying runtime exe to game folder..."
    $ExePath = Resolve-RuntimeExe -RuntimeRoot $RuntimeRoot -RuntimeName $RuntimeName
    if (-not $ExePath) {
        throw "Executable not found in .build/native/bin."
    }
    Write-Host ("Using runtime exe: " + $ExePath)
    Invoke-PipelineStep -Name "deploy_to_game.ps1" -ScriptBlock {
        & $DeployScript -GameRoot $GameRoot -ExePath $ExePath -ExeName $ExeName
    }
}

if ($Action -eq "run" -or $Action -eq "all") {
    $ExePath = Resolve-RuntimeExe -RuntimeRoot $RuntimeRoot -RuntimeName $RuntimeName
    if (-not (Test-Path $ExePath)) {
        throw "Executable not found: $ExePath"
    }
    Write-Host "Using runtime exe: $ExePath"
    if (-not $RuntimeArgs -or $RuntimeArgs.Count -eq 0) {
        if ($Quick) {
            $RuntimeArgs = @("--mode", "probe", "--print-summary")
        } elseif ($RunForever -ne 0) {
            $RuntimeArgs = @("--mode", "service", "--frame-delay-ms", [string]$FrameDelayMs, "--print-summary")
            if ($ServiceMaxFrames -gt 0) { $RuntimeArgs += @("--service-max-frames", [string]$ServiceMaxFrames) }
            if ($ServiceMaxDurationSeconds -gt 0) { $RuntimeArgs += @("--service-max-duration-seconds", [string]$ServiceMaxDurationSeconds) }
            if ($ServiceStopFile) { $RuntimeArgs += @("--service-stop-file", $ServiceStopFile) }
            if ($ServiceStopKey) { $RuntimeArgs += @("--service-stop-key", $ServiceStopKey) }
        } else {
            $RuntimeArgs = @("--mode", "probe", "--service-max-frames", [string]$LoopFrames, "--frame-delay-ms", [string]$FrameDelayMs, "--print-summary")
        }
    }
    Write-Host "Runtime args: $($RuntimeArgs -join ' ')"
    Invoke-PipelineStep -Name "runtime execution" -ScriptBlock {
        & $ExePath @RuntimeArgs
    }
}

Write-Host "Done: action=$Action"

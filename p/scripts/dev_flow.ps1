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

$BuildScript = Join-Path $PSScriptRoot "build_exe.ps1"
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

function Resolve-LatestRuntimeExe {
    param([string]$RuntimeRoot, [string]$RuntimeName)

    $ManifestPath = Join-Path (Join-Path $RuntimeRoot "dist") "$($RuntimeName).build.json"
    if (Test-Path $ManifestPath) {
        try {
            $manifest = Get-Content -Path $ManifestPath -Raw | ConvertFrom-Json
            foreach ($candidate in @($manifest.selected_exe, $manifest.target_exe, $manifest.pending_exe)) {
                if ($candidate -and (Test-Path $candidate)) {
                    return (Resolve-Path $candidate).Path
                }
            }
            Write-Host "Build manifest exists but selected/pending/target exe is missing; falling back to newest dist exe."
        } catch {
            Write-Host "Failed to read build manifest; falling back to newest dist exe."
        }
    }

    $DistDir = Join-Path $RuntimeRoot "dist"
    if (-not (Test-Path $DistDir)) {
        return ""
    }
    $all = Get-ChildItem -Path $DistDir -File -Filter "$RuntimeName*.exe" -ErrorAction SilentlyContinue
    if (-not $all) {
        return ""
    }
    return ($all | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
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
            "SingleQuote" {
                if ($char -eq "'") {
                    $state = "Normal"
                } else {
                    [void]$builder.Append($char)
                }
                continue
            }
            "DoubleQuote" {
                if ($char -eq '"') {
                    $state = "Normal"
                } else {
                    [void]$builder.Append($char)
                }
                continue
            }
        }

        if ($char -eq "'") {
            $state = "SingleQuote"
            continue
        }
        if ($char -eq '"') {
            $state = "DoubleQuote"
            continue
        }
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
    Write-Host "Building local runtime exe..."
    Invoke-PipelineStep -Name "build_exe.ps1" -ScriptBlock {
        & $BuildScript -RuntimeRoot $RuntimeRoot -ExeName $RuntimeName
    }
}

if ($Action -eq "deploy" -or $Action -eq "all") {
    Write-Host "Deploying runtime exe to game folder..."
    $ExePath = Resolve-LatestRuntimeExe -RuntimeRoot $RuntimeRoot -RuntimeName $RuntimeName
    if (-not $ExePath) {
        throw "Executable not found in dist."
    }
    Write-Host ("Using runtime exe: " + $ExePath)
    Invoke-PipelineStep -Name "deploy_to_game.ps1" -ScriptBlock {
        & $DeployScript -GameRoot $GameRoot -ExePath $ExePath
    }
}

if ($Action -eq "run" -or $Action -eq "all") {
    $ExePath = Resolve-LatestRuntimeExe -RuntimeRoot $RuntimeRoot -RuntimeName $RuntimeName
    if (-not (Test-Path $ExePath)) {
        throw "Executable not found: $ExePath"
    }
    Write-Host "Using runtime exe: $ExePath"
    Write-Host "Runtime args: $($RuntimeArgs -join ' ')"
    if (-not $RuntimeArgs -or $RuntimeArgs.Count -eq 0) {
        if ($Quick) {
            $RuntimeArgs = @("--mode", "loop", "--quick", "--loop-frames", "5", "--adapter", "noop", "--print-summary")
        } elseif ($RunForever -ne 0) {
            $RuntimeArgs = @("--mode", "service", "--frame-delay-ms", [string]$FrameDelayMs, "--adapter", $Adapter, "--print-summary")
            if ($ServiceMaxFrames -gt 0) {
                $RuntimeArgs += @("--service-max-frames", [string]$ServiceMaxFrames)
            }
            if ($ServiceMaxDurationSeconds -gt 0) {
                $RuntimeArgs += @("--service-max-duration-seconds", [string]$ServiceMaxDurationSeconds)
            }
            if ($ServiceStopFile) {
                $RuntimeArgs += @("--service-stop-file", $ServiceStopFile)
            }
            if ($ServiceStopKey) {
                $RuntimeArgs += @("--service-stop-key", $ServiceStopKey)
            }
            if (-not $RuntimeArgs.Contains("--service-trigger-key")) {
                $RuntimeArgs += @("--service-trigger-key", "f10")
            }
            if ($Adapter -eq "xenos" -and $BridgePath) {
                $RuntimeArgs += @("--bridge-path", $BridgePath)
            }
        } else {
            $RuntimeArgs = @("--mode", "loop", "--loop-frames", [string]$LoopFrames, "--frame-delay-ms", [string]$FrameDelayMs, "--adapter", "noop", "--print-summary")
        }
    }
    Invoke-PipelineStep -Name "runtime execution" -ScriptBlock {
        & $ExePath @RuntimeArgs
    }
}

Write-Host "Done: action=$Action"

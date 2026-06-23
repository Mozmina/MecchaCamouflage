param(
    [string]$RuntimeRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$OutDir = "",
    [string]$ExeName = "meccha-camouflage",
    [string]$VenvDir = "",
    [switch]$UseVenv = $true,
    [bool]$OneFile = $true,
    [bool]$BuildNative = $true
)

$ErrorActionPreference = "Stop"

if (-not $OutDir) {
    $OutDir = Join-Path $RuntimeRoot "dist"
}
$RequirementFile = Join-Path $RuntimeRoot "requirements.txt"
if (-not $VenvDir) {
    $VenvDir = Join-Path $RuntimeRoot ".venv"
}

$PyProject = Join-Path $RuntimeRoot "pyproject.toml"

function Get-ExeBaseName {
    param([string]$Name)

    $candidate = (New-Object System.IO.FileInfo($Name)).BaseName
    if ([string]::IsNullOrWhiteSpace($candidate)) {
        return "meccha-camouflage"
    }
    return $candidate
}

$ExeName = Get-ExeBaseName -Name $ExeName

function New-BuildTag {
    return Get-Date -Format "yyyyMMddHHmmssfff"
}

function Assert-PythonDependency {
    param([string]$PythonExe, [string]$ReqFile)
    & $PythonExe -m pip --version | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "pip is not usable in $PythonExe"
    }

    & $PythonExe -m pip install --upgrade -q --disable-pip-version-check pip | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to update pip in venv."
    }

    if (Test-Path $ReqFile) {
        Write-Host "Installing dependencies from requirements.txt..."
        & $PythonExe -m pip install --disable-pip-version-check -q -r $ReqFile
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to install requirements."
        }
    } else {
        Write-Host "Installing required PyInstaller..."
        & $PythonExe -m pip install --disable-pip-version-check -q PyInstaller
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to install PyInstaller."
        }
    }
}

function Invoke-PyInstallerBuild {
    param([string[]]$PyInstallerArgs)
    & $PythonExe -m PyInstaller @PyInstallerArgs
    return $LASTEXITCODE
}

function Test-FileWritable {
    param([string]$Path)
    try {
        $handle = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
        $handle.Close()
        return $true
    } catch {
        return $false
    }
}

function Resolve-PythonExe {
    param([string]$VenvDir, [switch]$UseVenv, [string]$FallbackPython)

    if ($UseVenv) {
        if (-not (Test-Path $VenvDir)) {
            Write-Host "Creating venv at $VenvDir..."
            & $FallbackPython -m venv $VenvDir
            if ($LASTEXITCODE -ne 0) {
                throw "Failed to create venv at $VenvDir"
            }
        }

        $candidates = @(
            (Join-Path $VenvDir "Scripts\python.exe"),
            (Join-Path $VenvDir "bin\python.exe"),
            (Join-Path $VenvDir "bin/python"),
            (Join-Path $VenvDir "Scripts\python")
        )

        foreach ($candidate in $candidates) {
            if (Test-Path $candidate) {
                return (Resolve-Path $candidate).Path
            }
        }

        Write-Warning "Venv was created but python executable not found in expected paths. Falling back to system python."
    }

    return $FallbackPython
}

function Ensure-VenvPosixCompat {
    param([string]$VenvDir)

    $scriptsDir = Join-Path $VenvDir "Scripts"
    if (-not (Test-Path $scriptsDir)) {
        return
    }

    $binDir = Join-Path $VenvDir "bin"
    New-Item -ItemType Directory -Force -Path $binDir | Out-Null

    $activateSource = Join-Path $scriptsDir "activate"
    $activateTarget = Join-Path $binDir "activate"
    if (Test-Path $activateSource) {
        Set-Content -Encoding ASCII -Path $activateTarget -Value (Get-Content -Raw -Path $activateSource)
    }

    $pythonTarget = if (Test-Path (Join-Path $scriptsDir "python.exe")) { "python.exe" } else { "python" }
    $pipTarget = if (Test-Path (Join-Path $scriptsDir "pip.exe")) { "pip.exe" } else { "pip" }

    $pythonShimTemplate = @'
#!/usr/bin/env sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$SCRIPT_DIR/../Scripts/{0}" "$@"
'@
    $pythonShimTarget = Join-Path $binDir "python"
    $python3ShimTarget = Join-Path $binDir "python3"
    if (Test-Path (Join-Path $scriptsDir $pythonTarget)) {
        Set-Content -Encoding ASCII -Path $pythonShimTarget -Value ($pythonShimTemplate -f $pythonTarget)
        Set-Content -Encoding ASCII -Path $python3ShimTarget -Value ($pythonShimTemplate -f $pythonTarget)
    }

    $pipShimTemplate = @'
#!/usr/bin/env sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$SCRIPT_DIR/../Scripts/{0}" "$@"
'@
    $pipShimTarget = Join-Path $binDir "pip"
    if (Test-Path (Join-Path $scriptsDir $pipTarget)) {
        Set-Content -Encoding ASCII -Path $pipShimTarget -Value ($pipShimTemplate -f $pipTarget)
    }

    if (Get-Command chmod -ErrorAction SilentlyContinue) {
        & chmod +x $activateTarget 2>$null
        & chmod +x $pythonShimTarget 2>$null
        & chmod +x $python3ShimTarget 2>$null
        & chmod +x $pipShimTarget 2>$null
    }
}

function Get-GitCommit {
    param([string]$RepoRoot)
    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) {
        return @{ Commit = ""; Dirty = $false }
    }
    try {
        $rev = & git -C $RepoRoot rev-parse HEAD
        if ($LASTEXITCODE -ne 0) {
            return @{ Commit = ""; Dirty = $false }
        }
        $status = & git -C $RepoRoot status --porcelain
        return @{
            Commit = $rev.Trim()
            Dirty = [bool]($status -and $status.Trim())
        }
    } catch {
        return @{ Commit = ""; Dirty = $false }
    }
}

function Get-PyProjectVersion {
    param([string]$PyProjectPath)
    $content = Get-Content -Path $PyProjectPath -Raw -ErrorAction SilentlyContinue
    if (-not $content) {
        return "unknown"
    }
    if ($content -match 'version\s*=\s*"([^"]+)"') {
        return $matches[1]
    }
    return "unknown"
}

function Get-PyInstallerVersion {
    param([string]$PythonExe)
    $version = & $PythonExe -m PyInstaller --version
    if ($LASTEXITCODE -ne 0 -or -not $version) {
        return "unknown"
    }
    return $version.Trim()
}

function Get-FileHashString {
    param([string]$FilePath)
    if (-not (Test-Path $FilePath)) {
        return ""
    }
    return (Get-FileHash -Path $FilePath -Algorithm SHA256).Hash.ToLowerInvariant()
}

$SystemPython = Get-Command python -ErrorAction SilentlyContinue
if (-not $SystemPython) {
    $SystemPython = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $SystemPython) {
    throw "python is required. Install Python 3.10+ and retry."
}

$PythonExe = Resolve-PythonExe -VenvDir $VenvDir -UseVenv:$UseVenv -FallbackPython $SystemPython.Path
Ensure-VenvPosixCompat -VenvDir $VenvDir

Push-Location $RuntimeRoot
try {
    $NativeInjector = Join-Path $RuntimeRoot "native\bin\meccha-xenos-injector.exe"
    $NativeBridge = Join-Path $RuntimeRoot "native\bin\meccha-xenos-bridge.dll"
    if ($BuildNative) {
        $BuildNativeScript = Join-Path $RuntimeRoot "scripts\build_native.ps1"
        if (-not (Test-Path $BuildNativeScript)) {
            throw "Native build script missing: $BuildNativeScript"
        }
        Write-Host "Building native injector and bridge..."
        & $BuildNativeScript -RuntimeRoot $RuntimeRoot
        if ($LASTEXITCODE -ne 0) {
            throw "Native build failed with exit code $LASTEXITCODE"
        }
    }
    if (-not (Test-Path $NativeInjector) -or -not (Test-Path $NativeBridge)) {
        throw "Native artifacts are required but missing. Expected: $NativeInjector and $NativeBridge"
    }

    $Entrypoint = Join-Path $RuntimeRoot "src/__main__.py"
    $BuildWorkDir = Join-Path $RuntimeRoot "build_pyexe"
    $SpecDir = Join-Path $RuntimeRoot "build_pyexe"
    Remove-Item -Recurse -Force $BuildWorkDir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    New-Item -ItemType Directory -Force -Path $SpecDir | Out-Null

    $BuildName = "{0}.{1}" -f $ExeName, (New-BuildTag)
    $PyInstallerArgs = @(
        "--noconfirm",
        "--clean",
        "--distpath", $OutDir,
        "--workpath", $BuildWorkDir,
        "--specpath", $SpecDir,
        "--name", $BuildName
    )
    if ($OneFile) {
        $PyInstallerArgs += "--onefile"
    }
    $NativeDir = Join-Path $RuntimeRoot "native"
    if (Test-Path $NativeDir) {
        $DataSeparator = [System.IO.Path]::PathSeparator
        $PyInstallerArgs += @("--add-data", "$NativeDir$($DataSeparator)native")
    }
    $PyInstallerArgs += $Entrypoint

    if (-not (Test-Path $Entrypoint)) {
        throw "Entrypoint not found: $Entrypoint"
    }

    Assert-PythonDependency -PythonExe $PythonExe -ReqFile $RequirementFile

    Write-Host ("Using python: " + $PythonExe)
    $buildExitCode = Invoke-PyInstallerBuild -PyInstallerArgs $PyInstallerArgs
    if ($buildExitCode -ne 0) {
        Write-Host "PyInstaller build failed, retrying after dependency refresh..."
        Assert-PythonDependency -PythonExe $PythonExe -ReqFile $RequirementFile
        $buildExitCode = Invoke-PyInstallerBuild -PyInstallerArgs $PyInstallerArgs
    }
    if ($buildExitCode -ne 0) {
        throw "PyInstaller build failed."
    }

    $BuiltExe = Join-Path $OutDir "$BuildName.exe"
    if (-not (Test-Path $BuiltExe)) {
        throw "Expected built executable missing: $BuiltExe"
    }

    $TargetExe = Join-Path $OutDir "$ExeName.exe"
    $PendingExe = Join-Path $OutDir "$ExeName.pending.exe"
    if (Test-Path $PendingExe) {
        Remove-Item -Force $PendingExe -ErrorAction SilentlyContinue
    }

    $copied = $false
    try {
        if (-not (Test-Path $TargetExe) -or (Test-FileWritable $TargetExe)) {
            Copy-Item -Force $BuiltExe $TargetExe
            $copied = $true
        } else {
            throw "Target executable in use"
        }
    } catch {
        Copy-Item -Force $BuiltExe $PendingExe
        Write-Host "Target exe is locked. Built artifact staged."
        Write-Host ("  " + $PendingExe)
    } finally {
        if (Test-Path $BuiltExe) {
            Remove-Item -Force $BuiltExe -ErrorAction SilentlyContinue
        }
    }

    if ($copied) {
        Write-Host "Built exe:"
        Write-Host ("  " + $TargetExe)
        $SelectedExe = $TargetExe
    } else {
        Write-Host "Built exe staged:"
        Write-Host ("  " + $PendingExe)
        $SelectedExe = $PendingExe
    }

    $cliFile = Join-Path $RuntimeRoot "src/cli.py"
    $pyProjectVersion = Get-PyProjectVersion -PyProjectPath $PyProject
    $gitMeta = Get-GitCommit -RepoRoot $RuntimeRoot
    $pyinstallerVersion = Get-PyInstallerVersion -PythonExe $PythonExe
    $manifest = @{
        build_started_utc = (Get-Date).ToUniversalTime().ToString("o")
        build_name = $BuildName
        runtime_name = $ExeName
        dist_dir = $OutDir
        source_exe = $BuiltExe
        target_exe = $TargetExe
        pending_exe = $PendingExe
        selected_exe = $SelectedExe
        python_exe = $PythonExe
        pyinstaller = $pyinstallerVersion
        pyproject_version = $pyProjectVersion
        git = @{
            commit = $gitMeta.Commit
            dirty = $gitMeta.Dirty
        }
        hashes = @{
            cli_py = (Get-FileHashString -FilePath $cliFile)
            requirements_txt = (Get-FileHashString -FilePath $RequirementFile)
            built_exe = ""
            native_injector = (Get-FileHashString -FilePath $NativeInjector)
            native_bridge = (Get-FileHashString -FilePath $NativeBridge)
        }
        native = @{
            injector = $NativeInjector
            bridge = $NativeBridge
            injector_exists = (Test-Path $NativeInjector)
            bridge_exists = (Test-Path $NativeBridge)
        }
        status = if ($copied) { "deployed_to_target" } else { "staged_pending" }
    }
    $manifest["hashes"]["built_exe"] = Get-FileHashString -FilePath $SelectedExe
    $manifestPath = Join-Path $OutDir "$ExeName.build.json"
    $manifest | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 $manifestPath
    Write-Host ("Build manifest: " + $manifestPath)
}
finally {
    Pop-Location
}

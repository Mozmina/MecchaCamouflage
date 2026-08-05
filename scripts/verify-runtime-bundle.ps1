param(
    [Parameter(Mandatory = $true)][string]$ExePath,
    [Parameter(Mandatory = $true)][string]$ExpectedVersion,
    [Parameter(Mandatory = $true)][string]$BridgePath,
    [Parameter(Mandatory = $true)][string]$ProfileDirectory
)

$ErrorActionPreference = "Stop"

function Convert-BytesToHex([byte[]]$Bytes) {
    return (($Bytes | ForEach-Object {
        $_.ToString("x2")
    }) -join "")
}

function Get-ExpectedRuntimeBundle {
    $bridgeHash = (Get-FileHash `
        -Algorithm SHA256 `
        -LiteralPath $BridgePath).Hash.ToLowerInvariant()
    $profiles = @(
        Get-ChildItem `
            -LiteralPath $ProfileDirectory `
            -Filter "*.json" `
            -File |
            ForEach-Object {
                [pscustomobject]@{
                    RelativePath = "mesh-profiles/$($_.Name)"
                    Hash = (Get-FileHash `
                        -Algorithm SHA256 `
                        -LiteralPath $_.FullName).Hash.ToLowerInvariant()
                }
            } |
            Sort-Object -Property RelativePath -CaseSensitive
    )
    if ($profiles.Count -eq 0) {
        throw "No runtime profiles were found."
    }
    $manifest = "schema=1`n" +
        "start_block_abi=2`n" +
        "resident_core_abi=2`n" +
        "protocol=2`n" +
        "bridge=$bridgeHash`n"
    foreach ($profile in $profiles) {
        $manifest += "profile=$($profile.RelativePath)=$($profile.Hash)`n"
    }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $id = Convert-BytesToHex $sha.ComputeHash(
            [System.Text.UTF8Encoding]::new($false).GetBytes($manifest))
    }
    finally {
        $sha.Dispose()
    }
    return [pscustomobject]@{
        Id = $id
        BridgeHash = $bridgeHash
        Profiles = $profiles
    }
}

foreach ($requiredPath in @($ExePath, $BridgePath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required file is missing."
    }
}
if (-not (Test-Path -LiteralPath $ProfileDirectory -PathType Container)) {
    throw "Runtime profile directory is missing."
}

$expected = Get-ExpectedRuntimeBundle
$start = [System.Diagnostics.ProcessStartInfo]::new()
$start.FileName = [System.IO.Path]::GetFullPath($ExePath)
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.Arguments = "--verify-runtime-bundle"
$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $start
if (-not $process.Start()) {
    throw "Could not start the packaged runtime verifier."
}
try {
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0) {
        throw "Packaged runtime verification failed with exit $($process.ExitCode): $stderr"
    }
}
finally {
    $process.Dispose()
}

$line = $stdout -split "`r?`n" |
    Where-Object { $_.Trim().Length -gt 0 } |
    Select-Object -Last 1
if (-not $line) {
    throw "Packaged runtime verifier returned no JSON."
}
$actual = $line | ConvertFrom-Json
if (-not $actual.success -or
    [string]$actual.app_version -ne $ExpectedVersion -or
    [string]$actual.runtime_bundle_id -ne $expected.Id -or
    [string]$actual.bridge_sha256 -ne $expected.BridgeHash -or
    [string]::IsNullOrWhiteSpace([string]$actual.package_asset_set_id)) {
    throw "Packaged runtime metadata does not match the tagged build inputs."
}
$actualProfiles = @{}
foreach ($profile in @($actual.profiles)) {
    $actualProfiles[[string]$profile.relative_path] =
        [string]$profile.sha256
}
foreach ($profile in $expected.Profiles) {
    if ($actualProfiles[$profile.RelativePath] -ne $profile.Hash) {
        throw "Packaged runtime profile hash mismatch."
    }
}
if ($actualProfiles.Count -ne $expected.Profiles.Count) {
    throw "Packaged runtime profile count mismatch."
}

$artifactHash = (Get-FileHash `
    -Algorithm SHA256 `
    -LiteralPath $ExePath).Hash.ToLowerInvariant()
$result = [pscustomobject]@{
    success = $true
    app_version = [string]$actual.app_version
    package_asset_set_id = [string]$actual.package_asset_set_id
    runtime_bundle_id = [string]$actual.runtime_bundle_id
    artifact_sha256 = $artifactHash
}
$result | ConvertTo-Json -Compress

if (-not [string]::IsNullOrWhiteSpace($env:GITHUB_STEP_SUMMARY)) {
    @(
        "### Runtime bundle verification"
        ""
        "- App version: ``$($result.app_version)``"
        "- Package asset set: ``$($result.package_asset_set_id)``"
        "- Runtime bundle: ``$($result.runtime_bundle_id)``"
        "- Artifact SHA-256: ``$($result.artifact_sha256)``"
    ) | Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY
}

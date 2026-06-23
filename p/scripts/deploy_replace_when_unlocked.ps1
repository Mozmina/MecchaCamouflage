param(
    [Parameter(Mandatory = $true)]
    [string]$TargetFile,
    [Parameter(Mandatory = $true)]
    [string]$PendingFile,
    [int]$TimeoutSeconds = 1800
)

$ErrorActionPreference = "Stop"

$TargetDir = Split-Path -Parent $TargetFile
$LogPath = Join-Path $TargetDir "meccha-camouflage.pending.install.log"

function Write-DeployLog {
    param([string]$Message)
    $Timestamp = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss.fff")
    Add-Content -Path $LogPath -Value "[$Timestamp] $Message" -Encoding ASCII
}

Write-DeployLog "watcher started target=$TargetFile pending=$PendingFile timeout_seconds=$TimeoutSeconds"

$Deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while ((Get-Date) -lt $Deadline) {
    if (-not (Test-Path $PendingFile)) {
        Write-DeployLog "pending file missing; nothing to install"
        exit 2
    }
    try {
        Copy-Item -Force $PendingFile $TargetFile
        $TargetHash = (Get-FileHash -Algorithm SHA256 $TargetFile).Hash.ToLowerInvariant()
        $PendingHash = (Get-FileHash -Algorithm SHA256 $PendingFile).Hash.ToLowerInvariant()
        if ($TargetHash -ne $PendingHash) {
            throw "hash mismatch target=$TargetHash pending=$PendingHash"
        }
        Remove-Item -Force $PendingFile
        Write-DeployLog "installed pending exe sha256=$TargetHash"
        exit 0
    } catch {
        Start-Sleep -Seconds 2
    }
}

Write-DeployLog "timed out waiting for file unlock"
exit 1

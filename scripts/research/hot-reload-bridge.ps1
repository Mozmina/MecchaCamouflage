param(
    [Parameter(Mandatory = $true)][int]$TargetPid,
    [Parameter(Mandatory = $true)][string]$BridgePath,
    [Parameter(Mandatory = $true)][string]$InjectorPath,
    [Parameter(Mandatory = $true)][string]$GenerationDirectory,
    [string]$ProfileDirectory = "",
    [ValidateRange(1, 20)][int]$MaxGenerations = 8
)

$ErrorActionPreference = "Stop"

function Convert-HexToBytes([string]$Hex) {
    if (($Hex.Length % 2) -ne 0) {
        throw "Hex value has odd length."
    }
    $bytes = New-Object byte[] ($Hex.Length / 2)
    for ($index = 0; $index -lt $bytes.Length; $index++) {
        $bytes[$index] = [Convert]::ToByte(
            $Hex.Substring($index * 2, 2),
            16)
    }
    return $bytes
}

function Convert-BytesToHex([byte[]]$Bytes) {
    return (($Bytes | ForEach-Object {
        $_.ToString("x2")
    }) -join "")
}

function Get-NativeRuntimeBundle(
    [string]$NativeBridgePath,
    [string]$RuntimeProfileDirectory) {
    $bridgeHash = (Get-FileHash `
        -Algorithm SHA256 `
        -LiteralPath $NativeBridgePath).Hash.ToLowerInvariant()
    $profileEntries = @(
        Get-ChildItem `
            -LiteralPath $RuntimeProfileDirectory `
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
    if ($profileEntries.Count -eq 0) {
        throw "Runtime bundle contains no mesh or image profiles."
    }
    $manifest = "schema=1`n" +
        "start_block_abi=2`n" +
        "resident_core_abi=2`n" +
        "protocol=2`n" +
        "bridge=$bridgeHash`n"
    foreach ($entry in $profileEntries) {
        $manifest += "profile=$($entry.RelativePath)=$($entry.Hash)`n"
    }
    $encoding = [System.Text.UTF8Encoding]::new($false)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bundleBytes = $sha.ComputeHash($encoding.GetBytes($manifest))
    }
    finally {
        $sha.Dispose()
    }
    return [pscustomobject]@{
        Id = Convert-BytesToHex $bundleBytes
        BridgeHash = $bridgeHash
        Manifest = $manifest
        Profiles = $profileEntries
    }
}

function Write-U32(
    [byte[]]$Buffer,
    [int]$Offset,
    [uint32]$Value) {
    [Array]::Copy(
        [BitConverter]::GetBytes($Value),
        0,
        $Buffer,
        $Offset,
        4)
}

function Quote-ProcessArg([string]$Value) {
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Read-ResidentCore([int]$ProcessId) {
    $mapping = $null
    $view = $null
    try {
        $mapping = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting(
            "Local\MecchaCamouflage.ResidentCore.$ProcessId",
            [System.IO.MemoryMappedFiles.MemoryMappedFileRights]::Read)
        $headerView = $mapping.CreateViewStream(
            0,
            12,
            [System.IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
        try {
            $header = New-Object byte[] 12
            if ($headerView.Read($header, 0, 12) -ne 12) {
                throw "Resident core header ended early."
            }
        }
        finally {
            $headerView.Dispose()
        }
        $size = [int][BitConverter]::ToUInt32($header, 4)
        if ($size -notin @(104, 136)) {
            throw "Resident core size is unsupported."
        }
        $view = $mapping.CreateViewStream(
            0,
            $size,
            [System.IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
        $bytes = New-Object byte[] $size
        $read = 0
        while ($read -lt $bytes.Length) {
            $count = $view.Read(
                $bytes,
                $read,
                $bytes.Length - $read)
            if ($count -eq 0) {
                throw "Resident core mapping ended early."
            }
            $read += $count
        }
        $magic = [BitConverter]::ToUInt32($bytes, 0)
        $abi = [BitConverter]::ToUInt32($bytes, 8)
        $protocol = [BitConverter]::ToUInt32($bytes, 20)
        $legacy = $magic -eq 0x3152434D -and $size -eq 104 -and
            $abi -eq 1 -and $protocol -eq 1
        $current = $magic -eq 0x3252434D -and $size -eq 136 -and
            $abi -eq 2 -and $protocol -eq 2
        if ((-not $legacy -and -not $current) -or
            [BitConverter]::ToUInt32($bytes, 12) -ne $ProcessId) {
            throw "Resident core identity is invalid."
        }
        $instanceBytes = New-Object byte[] 16
        $tokenBytes = New-Object byte[] 32
        $hashBytes = New-Object byte[] 32
        [Array]::Copy($bytes, 24, $instanceBytes, 0, 16)
        [Array]::Copy($bytes, 40, $tokenBytes, 0, 32)
        [Array]::Copy($bytes, 72, $hashBytes, 0, 32)
        $bundleId = $null
        if ($current) {
            $bundleBytes = New-Object byte[] 32
            [Array]::Copy($bytes, 104, $bundleBytes, 0, 32)
            $bundleId = Convert-BytesToHex $bundleBytes
        }
        return [pscustomobject]@{
            Port = [int][BitConverter]::ToUInt32($bytes, 16)
            Protocol = [int]$protocol
            InstanceId = Convert-BytesToHex $instanceBytes
            Token = Convert-BytesToHex $tokenBytes
            Hash = Convert-BytesToHex $hashBytes
            RuntimeBundleId = $bundleId
            Legacy = $legacy
        }
    }
    catch [System.IO.FileNotFoundException] {
        return $null
    }
    finally {
        if ($null -ne $view) {
            $view.Dispose()
        }
        if ($null -ne $mapping) {
            $mapping.Dispose()
        }
    }
}

function Invoke-BridgeCommand(
    [object]$Resident,
    [string]$Command) {
    $encoding = [System.Text.UTF8Encoding]::new($false)
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $client.Connect("127.0.0.1", $Resident.Port)
        $stream = $client.GetStream()
        $reader = [System.IO.StreamReader]::new(
            $stream,
            $encoding,
            $false,
            4096,
            $true)
        $writer = [System.IO.StreamWriter]::new(
            $stream,
            $encoding,
            4096,
            $true)
        try {
            $writer.AutoFlush = $true
            $hello = @{
                type = "hello"
                bootstrap_protocol = $Resident.Protocol
                instance_id = $Resident.InstanceId
                token = $Resident.Token
            } | ConvertTo-Json -Compress
            $writer.WriteLine($hello)
            $helloReply = $reader.ReadLine()
            if (-not $helloReply) {
                throw "Bridge returned no hello reply."
            }
            $helloObject = $helloReply | ConvertFrom-Json
            if (-not $helloObject.success -or
                [string]$helloObject.metadata.instance_id -ne
                    [string]$Resident.InstanceId -or
                [int]$helloObject.metadata.protocol_version -ne
                    [int]$Resident.Protocol -or
                ($Resident.Hash -and
                    [string]$helloObject.metadata.bridge_hash -ne
                        [string]$Resident.Hash) -or
                ($Resident.RuntimeBundleId -and
                    [string]$helloObject.metadata.runtime_bundle_id -ne
                        [string]$Resident.RuntimeBundleId)) {
                throw "Bridge authentication or generation identity failed."
            }
            $writer.WriteLine($Command)
            $replyText = $reader.ReadToEnd()
            if (-not $replyText) {
                throw "Bridge returned no command reply."
            }
            return ($replyText | ConvertFrom-Json)
        }
        finally {
            $writer.Dispose()
            $reader.Dispose()
            $stream.Dispose()
        }
    }
    finally {
        $client.Dispose()
    }
}

function Wait-ResidentCoreAbsent(
    [int]$ProcessId,
    [int]$TimeoutMilliseconds = 5000) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds(
        $TimeoutMilliseconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($null -eq (Read-ResidentCore $ProcessId)) {
            return
        }
        Start-Sleep -Milliseconds 25
    }
    throw "Old resident core mapping remained published after shutdown."
}

function New-StartBlock(
    [int]$ProcessId,
    [long]$CreationTime,
    [string]$TargetExe,
    [string]$Hash,
    [string]$RuntimeBundleId) {
    $instanceId = [Guid]::NewGuid().ToString("N")
    $tokenBytes = New-Object byte[] 32
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $rng.GetBytes($tokenBytes)
    }
    finally {
        $rng.Dispose()
    }
    $block = New-Object byte[] 160
    Write-U32 $block 0 0x3253434D
    Write-U32 $block 4 160
    Write-U32 $block 8 2
    Write-U32 $block 12 ([uint32]$ProcessId)
    [Array]::Copy(
        (Convert-HexToBytes $instanceId),
        0,
        $block,
        16,
        16)
    [Array]::Copy($tokenBytes, 0, $block, 32, 32)
    [Array]::Copy(
        (Convert-HexToBytes $Hash),
        0,
        $block,
        64,
        32)
    [Array]::Copy(
        (Convert-HexToBytes $RuntimeBundleId),
        0,
        $block,
        96,
        32)
    Write-U32 $block 128 0
    Write-U32 $block 132 0
    Write-U32 $block 136 0
    Write-U32 $block 140 2
    Write-U32 $block 144 0
    Write-U32 $block 148 0
    return [pscustomobject]@{
        Block = $block
        ProcessId = $ProcessId
        CreationTime = $CreationTime
        TargetExe = $TargetExe
        InstanceId = $instanceId
        Token = Convert-BytesToHex $tokenBytes
        Hash = $Hash
        RuntimeBundleId = $RuntimeBundleId
    }
}

function Invoke-Injector(
    [object]$Start,
    [string]$StagedBridgePath,
    [string]$StagedInjectorPath) {
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $StagedInjectorPath
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.Arguments = @(
        (Quote-ProcessArg "--direct"),
        (Quote-ProcessArg $Start.ProcessId.ToString()),
        (Quote-ProcessArg $Start.CreationTime.ToString()),
        (Quote-ProcessArg $Start.TargetExe),
        (Quote-ProcessArg $StagedBridgePath)
    ) -join " "
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $info
    if (-not $process.Start()) {
        throw "Could not start the injector."
    }
    try {
        $process.StandardInput.BaseStream.Write(
            $Start.Block,
            0,
            $Start.Block.Length)
        $process.StandardInput.BaseStream.Flush()
        $process.StandardInput.Close()
        $stdout = $process.StandardOutput.ReadToEnd()
        $stderr = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) {
            throw "Injector failed: exit=$($process.ExitCode) stdout=$stdout stderr=$stderr"
        }
        $line = $stdout -split "`r?`n" |
            Where-Object { $_.Trim().Length -gt 0 } |
            Select-Object -Last 1
        if (-not $line) {
            throw "Injector produced no result: $stderr"
        }
        return ($line | ConvertFrom-Json)
    }
    finally {
        $process.Dispose()
    }
}

if (-not (Test-Path -LiteralPath $BridgePath -PathType Leaf)) {
    throw "BridgePath not found."
}
if (-not (Test-Path -LiteralPath $InjectorPath -PathType Leaf)) {
    throw "InjectorPath not found."
}
if ([string]::IsNullOrWhiteSpace($ProfileDirectory)) {
    $repositoryRoot = Split-Path -Parent (
        Split-Path -Parent $PSScriptRoot)
    $ProfileDirectory = Join-Path `
        $repositoryRoot `
        "resources\mesh-profiles"
}
if (-not (Test-Path -LiteralPath $ProfileDirectory -PathType Container)) {
    throw "ProfileDirectory not found."
}

$target = Get-Process -Id $TargetPid
$targetExe = $target.Path
$creationTime = $target.StartTime.ToUniversalTime().ToFileTimeUtc()
$bundle = Get-NativeRuntimeBundle $BridgePath $ProfileDirectory
$hash = $bundle.BridgeHash
New-Item -ItemType Directory -Force -Path $GenerationDirectory |
    Out-Null
$bundleDirectory = Join-Path `
    $GenerationDirectory `
    "bundle-$($bundle.Id)"
$existingGenerations = @(
    Get-ChildItem -LiteralPath $GenerationDirectory `
        -Filter "bundle-*" `
        -Directory
)
if (-not (Test-Path -LiteralPath $bundleDirectory -PathType Container) -and
    $existingGenerations.Count -ge $MaxGenerations) {
    throw "Hot-reload generation cap reached ($MaxGenerations). Restart the game before loading another native generation."
}
New-Item -ItemType Directory -Force -Path $bundleDirectory |
    Out-Null
$stagedProfileDirectory = Join-Path `
    $bundleDirectory `
    "mesh-profiles"
New-Item -ItemType Directory -Force -Path $stagedProfileDirectory |
    Out-Null
foreach ($entry in $bundle.Profiles) {
    $fileName = [System.IO.Path]::GetFileName($entry.RelativePath)
    $sourcePath = Join-Path $ProfileDirectory $fileName
    $targetPath = Join-Path $stagedProfileDirectory $fileName
    if (-not (Test-Path -LiteralPath $targetPath -PathType Leaf)) {
        Copy-Item -LiteralPath $sourcePath -Destination $targetPath
    }
    $stagedHash = (Get-FileHash `
        -Algorithm SHA256 `
        -LiteralPath $targetPath).Hash.ToLowerInvariant()
    if ($stagedHash -ne $entry.Hash) {
        throw "Immutable runtime profile hash mismatch: $fileName"
    }
}
$aliasName = "runtime-bridge-hot-$($bundle.Id).dll"
$aliasPath = Join-Path $bundleDirectory $aliasName
if (-not (Test-Path -LiteralPath $aliasPath -PathType Leaf)) {
    Copy-Item -LiteralPath $BridgePath -Destination $aliasPath
}
$aliasHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $aliasPath).
    Hash.ToLowerInvariant()
if ($aliasHash -ne $hash) {
    throw "Content-addressed bridge alias hash mismatch."
}
$stagedBundle = Get-NativeRuntimeBundle `
    $aliasPath `
    $stagedProfileDirectory
if ($stagedBundle.Id -ne $bundle.Id) {
    throw "Staged native runtime bundle identity mismatch."
}

$mutexName = "Local\MecchaCamouflage.Inject.$TargetPid"
$mutex = [System.Threading.Mutex]::new($false, $mutexName)
$ownsMutex = $false
$oldHash = ""
$shutdownStage = "not_needed"
try {
    try {
        $ownsMutex = $mutex.WaitOne([TimeSpan]::FromSeconds(30))
    }
    catch [System.Threading.AbandonedMutexException] {
        $ownsMutex = $true
    }
    if (-not $ownsMutex) {
        throw "Timed out waiting for the bridge injection mutex."
    }

    $resident = Read-ResidentCore $TargetPid
    if ($null -ne $resident) {
        $oldHash = $resident.Hash
        $shutdown = Invoke-BridgeCommand $resident '{"type":"shutdown"}'
        $shutdownStage = [string]$shutdown.stage
        if (-not $shutdown.success -or
            -not $shutdown.metadata.active_paint_quiescent -or
            -not $shutdown.metadata.hook_callbacks_quiescent) {
            throw "Resident bridge did not prove active_paint_quiescent and hook_callbacks_quiescent."
        }
        Wait-ResidentCoreAbsent $TargetPid
    }

    $start = New-StartBlock `
        $TargetPid `
        $creationTime `
        $targetExe `
        $hash `
        $bundle.Id
    $injector = Invoke-Injector `
        $start `
        $aliasPath `
        $InjectorPath
    if (-not $injector.success -or
        $injector.state -ne "listening" -or
        [int]$injector.protocol -ne 2 -or
        [string]$injector.instance_id -ne $start.InstanceId -or
        [string]$injector.bridge_hash -ne $hash -or
        [string]$injector.runtime_bundle_id -ne $bundle.Id) {
        throw "New bridge generation did not return the staged V2 identity."
    }
    $newResident = [pscustomobject]@{
        Port = [int]$injector.port
        Protocol = 2
        InstanceId = $start.InstanceId
        Token = $start.Token
        Hash = $hash
        RuntimeBundleId = $bundle.Id
    }
    $ping = Invoke-BridgeCommand $newResident '{"type":"ping"}'
    if (-not $ping.success -or $ping.stage -ne "ping") {
        throw "New bridge generation failed its authenticated ping."
    }
    $published = Read-ResidentCore $TargetPid
    if ($null -eq $published -or
        $published.Hash -ne $hash -or
        $published.RuntimeBundleId -ne $bundle.Id) {
        throw "New resident core was not published with the staged runtime bundle identity."
    }

    [pscustomobject]@{
        success = $true
        target_pid = $TargetPid
        old_bridge_hash = $oldHash
        new_bridge_hash = $hash
        runtime_bundle_id = $bundle.Id
        staged_bridge = $aliasPath
        generation_count = @(
            Get-ChildItem -LiteralPath $GenerationDirectory `
                -Filter "bundle-*" `
                -Directory
        ).Count
        max_generations = $MaxGenerations
        shutdown_stage = $shutdownStage
        injector_state = $injector.state
        port = [int]$injector.port
        resident_core_published = $true
        credentials = "redacted"
    } | ConvertTo-Json -Depth 6
}
finally {
    if ($ownsMutex) {
        $mutex.ReleaseMutex()
    }
    $mutex.Dispose()
}

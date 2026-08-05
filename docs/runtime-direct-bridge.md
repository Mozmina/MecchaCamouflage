# Direct Bridge Generations (v1.7)

This is the authoritative description of the production native bootstrap and
hot-reload path. The controller accepts a process-resident bridge only when its
DLL, runtime profiles, and compatibility ABIs match the package currently
running.

## Runtime bundle identity

The relevant implementation is:

- `src/native/bridge/bridge.cpp`: injected bridge and `BridgeStartV2`.
- `src/native/injector/injector.cpp`: one direct injection attempt.
- `src/native/include/direct_bridge_abi.hpp`: fixed startup ABI.
- `src/csharp/ZemiMecchamouflage.Controller/RuntimeBridgeService.cs`: staging,
  identity, synchronization, and connection ownership.
- `src/csharp/ZemiMecchamouflage.Controller/BridgeBootstrap.cs` /
  `BridgeBootstrapV2.cs`: startup-block and endpoint identity models.
- `src/csharp/ZemiMecchamouflage.Controller/BridgeClient.cs`: HELLO and command
  sequencing.
- `src/csharp/ZemiMecchamouflage.Controller/BridgeGenerationPolicy.cs`,
  `BridgeResidentCore.cs`, `NativeRuntimeBundle.cs`: generation identity,
  reconnect/replace decisions, and immutable staging.

`NativeRuntimeBundleId` is the SHA-256 of a canonical UTF-8/LF manifest:

```text
schema=1
start_block_abi=2
resident_core_abi=2
protocol=2
bridge=<runtime-bridge.dll sha256>
profile=mesh-profiles/<ordinal relative name>=<sha256>
```

All mesh and Image Paint profiles participate in this identity. Application
version and injector hash are verified separately and do not affect resident
behavior identity. Profile entries are sorted with ordinal comparison.

The controller calculates the identity from the validated package cache, copies
the runtime into a new immutable instance directory, and calculates it again
from the staged files. A mismatch prevents injection.

## Bootstrap and resident ABIs

New injections use `BridgeStartV2` and the 160-byte, pointer-free
`BridgeStartBlockV2`.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | magic (`MCS2`) |
| 4 | 4 | structure size (`160`) |
| 8 | 4 | ABI version (`2`) |
| 12 | 4 | expected PID |
| 16 | 16 | instance GUID |
| 32 | 32 | random connection token |
| 64 | 32 | bridge DLL SHA-256 |
| 96 | 32 | runtime bundle SHA-256 |
| 128 | 4 | requested port (`0`) |
| 132 | 4 | result state |
| 136 | 4 | bound port |
| 140 | 4 | bootstrap protocol (`2`) |
| 144 | 4 | Win32 error |
| 148 | 4 | Winsock error |
| 152 | 8 | reserved; must be zero |

The native bridge publishes a 136-byte `MCR2` resident mapping containing its
PID, port, protocol, GUID, token, DLL hash, and runtime bundle ID. HELLO returns
the same identity. The controller requires the start block, injector result,
resident mapping, and authenticated HELLO to agree.

The controller retains a read-only parser for the 104-byte `MCR1` mapping. A
v1.6.x resident may authenticate only as an upgrade source; it is never accepted
as the active v1.7 runtime generation.

## Immutable staging

Each V2 instance is staged under:

```text
%LOCALAPPDATA%\ZemiMecchamouflage\bridge-instances\
  bridge-instance-v2-<pid>-<bundle-prefix>-<guid>\
```

The DLL name contains the runtime bundle generation, full DLL hash, and instance
GUID. Loaded directories are never overwritten or deleted. Cleanup removes only
unloaded V2 directories owned by the current target or by a process that no
longer exists. Legacy directories without provable ownership are retained.

The content-addressed package cache remains responsible for validating embedded
resources. WebView2 data and user settings do not participate in bridge
generation cleanup.

## Connection and replacement lifecycle

The controller captures the exact target PID, creation `FILETIME`, and
normalized executable path. The injector independently verifies all three.

For every connection:

1. Calculate the packaged runtime bundle ID.
2. Read the resident mapping.
3. Reconnect only when the V2 bundle and DLL hashes match.
4. Otherwise acquire `Local\ZemiMecchamouflage.Inject.<pid>` and re-read the
   mapping to avoid a cross-GUI race.
5. Authenticate to the old resident and request shutdown.
6. Require `active_paint_quiescent=true` and
   `hook_callbacks_quiescent=true`.
7. Wait up to five seconds for the resident mapping to disappear.
8. Stage and hash a new immutable instance.
9. Inject its V2 start block.
10. Verify the injector result, HELLO, and newly published resident mapping.

Shutdown closes command admission before canceling queued and executing paint.
It removes ProcessEvent and Present ownership only after paint, UE calls, and
hook callbacks are quiescent. The old DLL remains loaded but inert.

A production process may hold at most three native generations. Development and
research builds allow eight. One automatic replacement attempt is allowed for a
PID and desired bundle during one GUI session. Any timeout, invalid identity,
failed quiescence proof, retry, or generation-cap condition fails closed and
requires a game restart; Paint, Preview, and ESP do not use the stale bridge.

## Command protocol and diagnostics

Every TCP command begins with HELLO containing the protocol, GUID, and random
token. Application commands are sent only after PID, GUID, DLL hash, bundle ID,
and protocol validation.

Normal logs expose only short non-secret identity fields:

- application version and package asset-set ID;
- desired and resident runtime bundle IDs;
- generation match, replacement stage, and generation count.

Tokens, credentials, fixed personal paths, and full target paths are not logged
outside explicit research diagnostics.

## Release verification

GitHub Actions is the release publisher. Before upload, the packaged executable
runs `--verify-runtime-bundle`, re-extracts its embedded assets through the
normal validated cache, and reports its version, asset-set ID, DLL hash, profile
hashes, and runtime bundle ID. The workflow compares these values with the
version-scoped native build and source profiles, then records the release
artifact SHA-256.

Windows verification must cover:

- v1.6.x resident to v1.7 automatic replacement;
- reconnect with an identical bundle;
- profile-only and DLL changes;
- replacement while Paint/Preview/ESP are active;
- shutdown, mapping, HELLO, and generation-cap failure paths;
- unchanged manual Paint, Image Paint, cancellation, and multiplayer behavior.

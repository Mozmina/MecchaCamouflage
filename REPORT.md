# Issue #87 multiplayer paint synchronization investigation report

最終更新: 2026-07-11 (Asia/Tokyo)

対象:

- <https://github.com/acentrist/MecchaCamouflage/issues/87>
- <https://github.com/acentrist/MecchaCamouflage/issues/117>（実質的な重複）
- repository: <https://github.com/acentrist/MecchaCamouflage>
- branch: `main`
- 調査開始時の基準 commit: `992bf7cebc629b9f3e1b279f60e9728861414292`

この文書は、長期調査で確定した事実、実機証拠、失敗した仮説、現在の未コミット実装、未解決 blocker を残すための記録である。GitHub issue コメントの説明は観測情報としてのみ扱い、ここで `VERIFIED` とした事項はコード、runtime probe、event-watch、texture checksum、disassembly のいずれか複数で確認した。

認証情報、bridge token、SSH password、個人名、接続先 IP は記録していない。

## 現在の結論

Issue #87 の長時間遅延は、現時点では Epic Online Services P2P の packet loss が主因であるという証拠はない。

`VERIFIED` な主因は、旧 production-shaped combined 経路が次の二つを同じ stroke に対して実行していたことである。

1. 明示的な `ServerPackedPaintBatch` RPC。
2. painter 側表示のための reflected `PaintAtUVWithBrush`。

2 の内部 common call は anti-echo flag が `false` で、ゲーム本来の realtime/full-stroke replication、record、flush 系分岐にも入る。そのため packed RPC と native full-stroke replication が二重に発生していた。

receiver の full-stroke `All` channel queue は、観測した render budget では概ね一 tick に一 stroke 程度しか進まず、重複した full queue が数分残る。controlled A/B では native realtime path を止めると full-stroke traffic が消え、packed path は継続した。これは EOS loss 仮説より、送信源の二重化と receiver queue pressure を支持する。

現在の未コミット実装は、通常 `combined` を次の経路へ変更している。

- remote: `RuntimePaintableComponent.ServerPackedPaintBatch`
- painter local: `MulticastPackedPaintBatch` の reflected UFunction は呼ばず、
  exact-build guarded native receiver implementation を直接呼び、ゲーム自身の
  `RuntimePaintReplicationManager` queueへ同じpacked batchをenqueue
- remote の production 設定は mode 選択ではなく `batch limit` と
  `batch pacing` の二つの slider。範囲は 1--20 stroke と 50--500 ms、
  default/faster end は 20/50 ms
- remote/local は同じoffset、batch境界、pacingでpaired submissionし、両方成功時
  だけoffsetを完了扱いにする。painter-local receiver backlogはremote peerの
  pressureではないためoutgoing backoffに使わず、game-owned render budgetへdrainを任せる
- 旧4 ms internal-common yieldはresearch A/Bにだけ残り、production標準では使わない
- Brush 1 sizeは15--20/default 20、Brush 2 sizeは5--10/default 10
- plannerは`Fill -> CoarsePaint -> FinePaint`、各passは`Back -> Side -> Front`。
  Fillは1回、Paint regionだけBrush 1のdedupe済み粗passとBrush 2のfine pass、
  Skipは0 stroke。pass境界を一つのRPCが跨がない
- 視覚順序はposed surfaceのlocal Zではなくprofile bind/reference Zを使い、
  頭頂から足へrow化、同一rowはcamera-rightの左から右へstable sortする
- reflected `PaintAtUVWithBrush` への自動 fallback: なし
- per-stroke internal commonへの自動 fallback: なし
- old compact/adaptive/send-custom 経路への自動 fallback: なし
- resolver/preflight failure: 最初の RPC より前に fail-closed

旧 6/75 route は両方向の multiplayer 実機試験で動作した。現在の default
20/50 paired packed lane は host 単体で検証済みだが、joining
client では未再検証である。したがって default slider 設定を joining-client
で検証済みと release note や issue に書いてはいけない。

## 状態ラベル

- `VERIFIED`: runtime、dump、event-watch、checksum、または disassembly で確認済み。
- `IMPLEMENTED`: working tree に実装済み。commit 済みという意味ではない。
- `TESTED`: 記載した build/runtime artifact で試験済み。
- `UNVERIFIED`: 妥当な仮説または未試験の最新 source。
- `BLOCKER`: 次の production 判断より前に修正と再試験が必要。

## 調査環境

### Multiplayer topology

- host: 物理 Windows 側。
- joining client: 過去の二台試験ではHyper-V Windows 11 VM。現在はofflineで、
  今回の追加検証には使用していない。
- current single-host game PID: `25268`（再起動後は再取得すること）。
- PID は再起動後には再取得すること。

VM は passwordless helper `~/.local/bin/hyperv-ssh` で管理できる。資格情報はこの文書へ追加しないこと。

### Executable identity

両環境の Shipping executable SHA-256:

```text
70D4F1F1730AEE27BA30782902935793824787676D555A7A090D1114A8495542
```

host executable path:

```text
C:\Program Files (x86)\Steam\steamapps\common\MECCHA CHAMELEON\Chameleon\Binaries\Win64\PenguinHotel-Win64-Shipping.exe
```

legacy exact-build gate:

| Field | Value |
| --- | --- |
| PE timestamp | `0x534F2F4B` |
| `SizeOfImage` | `0x0AAFE000` |
| PE checksum | `0x0A735A7C` |
| raw `.text` size | `0x07AA3200` |
| raw `.text` FNV-1a64 | `0x085F72BE8C58A9E6` |

game update 後はこれらをコピーして使わず、すべて再取得すること。

current Steam exact-build gate:

| Field | Value |
| --- | --- |
| PE timestamp | `0xB76CD1AF` |
| `SizeOfImage` | `0x0AAFD000` |
| PE checksum | `0x0A7394E2` |
| raw `.text` size | `0x07AA2800` |
| raw `.text` FNV-1a64 | `0x4200B2CAF330F8C3` |

## Architecture map

通常の制御経路:

```text
WebHost / HostSession
  -> BridgePayloadBuilder
  -> RuntimeBridgeService (unique staged DLL + injector ownership)
  -> BridgeClient (GUID/token HELLO over authenticated loopback)
  -> native bridge listener handler
  -> paint_full_route_native queue
  -> Win32 message hook on the game thread
  -> mesh-first synchronous planner
  -> async paired packed RPC + local receiver scheduler
  -> UE ProcessEvent(server only) + exact native receiver implementation(local)
```

主な source:

- `src/csharp/MecchaCamouflage.Controller/RuntimeBridgeService.cs`
- `src/csharp/MecchaCamouflage.Controller/BridgeClient.cs`
- `src/csharp/MecchaCamouflage.Controller/HostSession.cs`
- `src/csharp/MecchaCamouflage.WebHost/ResearchRunner.cs`
- `src/native/bridge/bridge.cpp`
- `src/native/bridge/bridge_json.inc`
- `src/native/include/sdk.hpp`
- `src/native/include/runtime_contract.hpp`
- `src/native/tests/transform_validation_test.cpp`
- `scripts/research/build-replication-runner.ps1`
- `scripts/research/README.md`

関連設計文書:

- `docs/runtime-direct-bridge.md`
- `docs/runtime-paint-replication-research.md`
- `docs/runtime-maintenance.md`
- `docs/release-checklist.md`

## Reverse engineering facts

### Internal no-resend route

`VERIFIED` current-build RVAs:

| Item | RVA |
| --- | ---: |
| reflected thunk | `0x50E59E0` |
| native implementation | `0x50FF650` |
| `FPaintStroke` constructor | `0x50DB260` |
| common paint routine | `0x50EEC80` |

Additional facts:

- constructor は `0xE0` byte の leaf/POD initialization。個別 destructor は不要。
- `FPaintStroke` size は `0xE0`。
- `Uv@0x00`, `BrushSettings@0x68`, `ChannelData@0x90`, `TargetChannel@0xB0`。
- normal implementation は common を第三引数 `false` で呼ぶ。
- common `+0x376` の分岐で `true` は realtime send/record 系を skip し、target は common `+0x49B`。
- `true` は単なる send bit ではない。normal frame deferral/record/autoflush の一部も bypass するため、direct call は最大 6/tick に制限している。
- component `+0x2C8` の counter は common `+0x968` で一 call あたり `+1`。
- counter `+1` は「non-empty generated geometry 後、channel dispatch-attempt 後の increment site に到達した」証拠であり、render-target write、pixel change、render-thread completion の証明ではない。

### Runtime paint data prerequisites

`VERIFIED` current-build offsets:

| Item | Offset |
| --- | ---: |
| Albedo render target | component `+0x148` |
| Metallic render target | component `+0x150` |
| Roughness render target | component `+0x158` |
| Height render target | component `+0x160` |
| runtime-triangle `TArray::Data` | component `+0x1C8` |
| runtime-triangle `TArray::Num` | component `+0x1D0` |
| runtime-triangle `TArray::Max` | component `+0x1D4` |
| component ready byte | component `+0x1E8` |
| runtime-triangle stride | `0xD0` |

`TargetChannel` mapping:

| Value | Meaning | Required targets |
| ---: | --- | --- |
| 0 | Albedo | `+0x148` |
| 1 | Metallic | `+0x150` |
| 2 | Roughness | `+0x158` |
| 3 | Height | `+0x160` |
| 4 | All | all four |
| 5 | AlbedoMetallicRoughness | `+0x148,+0x150,+0x158` |
| 6/other | invalid | reject |

Value 5 は元の `sdk.hpp` enum から欠落していたため、working tree で追加済み。

### Correct ObjectFlags interpretation

`VERIFIED`（2026-07-11 source fix）: `live_uobject_not_destroyed` は shared
`runtime_contract::uobject_flags_usable` を使う。`0x20000000` 単独は拒否
しない。object は `0x40018010`、class は `0x40018000` を reject する。

current Shipping disassembly は target UObject の `ObjectFlags@+0x08` bit 30 を検査する。

```text
common RVA 0x50EF275: mov rax,[rbx+0x148]
common RVA 0x50EF281: mov eax,[rax+0x08]
common RVA 0x50EF284: shr eax,0x1e
```

正しい値:

```cpp
RF_BeginDestroyed  = 0x00008000u;
RF_FinishDestroyed = 0x00010000u;
RF_MirroredGarbage = 0x40000000u;
```

native contract test は `0x20000000` の usable matrix と reject mask を
回帰対象にした。current host smoke は対象 flags が clear な live object での
成功証拠であり、matrix test は mask の契約証拠である。前者だけを flags
修正の証拠として扱ってはいけない。

### Transform/reflection decoder failure found after game restart

`VERIFIED` root cause:

- reflected `FProperty::ElementSize` を `+0x3C` から読んでいた。
- `+0x3C` は `PropertyFlags` high dword で、観測値 `0x180010`。
- supported build の正しい layout は `ArrayDim@0x30`, `ElementSize@0x34`, `PropertyFlags@0x38`, `Offset_Internal@0x44`。
- bad decoder は quaternion `(0,0,-2889820,11160673)`、length `11528732` を選択した。
- raw `[mesh+0x1E0]` は `(0,0,0.705717,0.708494)`、length `1`。
- K2 getter の static disassembly は raw `[mesh+0x1E0]` を copy しており、raw cache error は `0 cm`、bad selection は `76.632 cm`。

`IMPLEMENTED`:

- element-size offset を `0x34` に修正。
- `0x60` byte raw `FTransform` copy、Rotation/Translation/Scale 全要素要求、quaternion plausibility guard。
- native contract test と shared `runtime_contract.hpp`。

この修正後、K2/raw quaternion length は `1`、coordinate error は `0` になり、20/400 stroke tests が通った。

## Controlled experiments and conclusions

### Legacy duplicate-source attribution

`VERIFIED`:

- combined では explicit packed traffic と full-stroke traffic の両方が出た。
- packed-only は remote texture を変更したが painter local texture は変えなかった。
- local/realtime A/B で realtime native path を止めると full-stroke traffic が消え、packed traffic は残った。
- receiver full queue の drain が遅く、budget 6 control で概ね一 stroke/tick の挙動を観測した。

結論:

- local apply 自体を削除すると painter が見えない。
- explicit packed に加え、anti-echo/no-resend local apply が必要。
- EOS P2P tuning や render budget の恒久的書き換えを先に行うべきではない。

### Failed hypotheses that must not be repeated

- component `+0x350 > 0` を precondition にしてはいけない。これは call 中に clear/rebuild される generated-geometry scratch `Num`。
- `+0xC8` は `TextureOptions.SeamBleedPixels` であり、base draw の必須条件ではない。
- `+0x2C8 +1` を pixel/render completion と解釈してはいけない。
- packed-only を production fix としてはいけない。sender local texture が変化しない。
- issue comment の networking explanation を technical conclusion として採用しない。
- `paint_packed_replay_probe` はこの調査では使用していない。今後も明示的な危険操作なしに使用しない。
- partial commit 後の自動 retry をしてはいけない。duplicate paint の可能性がある。

誤った `+0x350` guard を入れた一回の artifact は、最初の 6 packed strokes 後に local failure となった。metadata の `partial_commit=true`, `automatic_retry_safe=false` は正しく働いたが、この run は成功証拠ではない。

## Multiplayer runtime evidence

### Host -> joining client, normal combined 20

Artifacts:

- host: `%LOCALAPPDATA%\MecchaResearch\runs\host-production-combined20-fixed-20260711T0220`
- VM observer: `C:\MecchaResearch\runs\vm-production-combined20-fixed-observer-20260711T0220`
- WSL copy: `/tmp/vm-production-combined20-fixed-observer-20260711T0220`

Results:

- host applied `20/20`。
- `ServerPackedPaintBatch`: 4 calls / 20 strokes。
- VM `MulticastPackedPaintBatch`: 4 calls / 20 strokes。
- `PaintAtUVWithBrush`: 0 observed calls。
- legacy `MulticastPaintBatch`: 0。
- host local internal calls: 20 returned/postcondition-validated, exceptions 0。
- VM remote Albedo hash: `15499128298749170360 -> 8103443904372447560`。

### Joining client -> host, normal combined 20

Artifacts:

- VM sender: `C:\MecchaResearch\runs\vm-production-combined20-20260711T0240`
- WSL copy: `/tmp/vm-production-combined20-20260711T0240`
- host observer: `%LOCALAPPDATA%\MecchaResearch\runs\host-observe-vm-production-combined20-20260711T0240`

Results:

- VM applied `20/20`。
- `ServerPackedPaintBatch`: 4 calls / 20 strokes。
- host `MulticastPackedPaintBatch`: 4 calls / 20 strokes。
- full/reflected paths: 0。
- VM local internal calls: 20 returned/postcondition-validated, exceptions 0。
- VM local Albedo hash: `15721146440976623552 -> 8510400991940386020`。
- host remote-player component Albedo hash: `3830424718519525275 -> 6998268980819666571`。

### Host -> joining client, normal combined 400 stress

Artifacts:

- host sender: `%LOCALAPPDATA%\MecchaResearch\runs\host-production-combined400-20260711T0300`
- VM observer: `C:\MecchaResearch\runs\vm-production-combined400-observer-20260711T0300`
- WSL copy: `/tmp/vm-production-combined400-observer-20260711T0300`
- VM later texture snapshot: `C:\MecchaResearch\runs\vm-production-combined400-texture-current-20260711T0320`

Sender results:

- applied `400/400`。
- 67 packed calls, 67 successes, 0 failures。
- effective batch cap 6。
- local calls 400, exceptions 0。
- `ServerPackedPaintBatch` and `MulticastPackedPaintBatch`: 67 calls / 400 strokes。
- reflected/full-stroke paths: 0。

Receiver pressure evidence:

- 279 samples at 250 ms。
- probe failures: 0。
- maximum queued batches: 1。
- maximum queued strokes: 6。
- non-zero samples: 3。
- final queued batches/strokes: 0/0。
- receiver `MulticastPackedPaintBatch`: 67 calls / 400 strokes。
- positive queue delta total: 400。
- VM remote texture hash changed from the prior 20-stroke state `8103443904372447560` to `7264852723993499012`。

### Packed-only 20 attribution

Artifacts:

- host: `%LOCALAPPDATA%\MecchaResearch\runs\host-packed-only20-20260711T0050`
- VM observer copy: `/tmp/vm-packed-only20-observer-20260711T0050`

Results:

- host painter-side Albedo checksum changes: none。
- VM remote Albedo hash: `11764730460316111552 -> 16439339244260173248`。

This is the direct evidence that explicit packed RPC alone is insufficient for painter-side visual state.

### Current-build preflight smoke on both roles

Host artifact:

```text
%LOCALAPPDATA%\MecchaResearch\runs\host-lifecycle-preflight-smoke2-20260711T0440
```

Joining-client artifact:

```text
C:\MecchaResearch\runs\vm-lifecycle-preflight-smoke-20260711T0505
/tmp/vm-lifecycle-preflight-smoke-20260711T0505
```

Both runs:

- paint success, shutdown success。
- `server_strokes_sent=1`, `local_stroke_success=1`。
- `local_apply_calls_returned=1`。
- `local_apply_calls_validated=1`。
- `local_apply_call_exceptions=0`。
- `local_apply_preflight_complete=true`。
- `local_apply_preflight_strokes_validated=1`。
- `local_apply_cpu_submission_ok=true`。
- `no_resend_resolver_status=resolved`。

Texture evidence:

- host local Albedo: `12484000411130797647 -> 10813120484104758558`。
- VM local Albedo: `8510400991940386020 -> 16995616519006205511`。

These hashes prove pixel data changed in these specific smoke runs. The CPU postcondition alone does not.

## Cancellation, shutdown, and reinjection evidence

### Cancel during synchronous planning

Artifact:

```text
%LOCALAPPDATA%\MecchaResearch\runs\host-cancel-planning-20260711T0520
```

Setup: 400-stroke combined request, `cancel_paint` after 100 ms.

Results:

- `cancelled_active_paint_jobs=1`。
- `cancelled_queued_paint_jobs=0`。
- paint terminal stage: `mesh_paint_cancelled`。
- cancel reason: `cancel_paint`。
- packed, multicast packed, reflected/full calls before cancel: 0。
- shutdown after terminal: `active_paint_quiescent=true`, `hook_callbacks_quiescent=true`。

Immediate no-restart recovery artifact:

```text
%LOCALAPPDATA%\MecchaResearch\runs\host-post-cancel-reinject-20260711T0525
```

Fresh injection and normal one-stroke paint both succeeded.

### Shutdown during synchronous planning

Artifact:

```text
%LOCALAPPDATA%\MecchaResearch\runs\host-shutdown-planning-20260711T0550
```

Setup: 400-stroke combined request, shutdown after 100 ms.

Results:

- shutdown success。
- `cancelled_active_paint_jobs=1`。
- `paint_request_was_in_progress=true`。
- `active_paint_quiescent=true`。
- `hook_callbacks_quiescent=true`。
- paint terminal stage: `mesh_paint_cancelled`、reason `shutdown`。
- packed/full/reflected calls before shutdown: 0。
- event-watch reached `event_watch_stopped`。

Immediate no-restart recovery artifact:

```text
%LOCALAPPDATA%\MecchaResearch\runs\host-post-active-shutdown-reinject-20260711T0555
```

Fresh injection, normal paint, and clean shutdown succeeded.

### Reinjection semantics

`VERIFIED` for unique staged bridge instances:

- old DLL が game process に resident のままでも fresh unique-path injection は成功する。
- cancel 後、active shutdown 後とも game restart は不要だった。
- broad "old bridge present -> reject/restart required" policy は戻していない。

`VERIFIED` for reusing the exact same already-loaded DLL module:

- source は STOPPING 中の `BridgeStartV1` を最大 8 秒待ち、clean rundown 後に再開始する方向へ変更済み。
- event-watch writer は generation-tagged joinable thread へ変更済み。final bridge hash
  `2ee12263602461eb57413815262e604eb312702384a903acd8f5143983f89bfc` を同一DLLパスで
  shutdown後10回再注入し、10/10でping、shutdown、callback rundownが成功した。

## Implemented working-tree changes

すべて未コミット。主要項目:

- normal `combined` を packed + internal no-resend local apply に変更。
- strict PE/text/signature/reflection/ABI resolver。
- FProperty `ElementSize@0x34` と robust `FTransform` decoder。
- remote `batch limit` 1--20 と `batch pacing` 50--500 ms の slider contract
  （default 20/50 ms）。
- local apply は最大 6 stroke/dispatch、4 ms CPU budget で adaptive yield。
- recurring scheduler の 0 ms repost を拒否し、最低 1 ms に clamp。
- mode/region partition 内の頭側から足側、左から右への stable scanline order。
- preflight: component, triangle cache, brush texture, selected channel targets。
- `local_apply_calls_returned` と `local_apply_calls_validated` の分離。
- CPU submission semantics と partial-commit/no-auto-retry metadata。
- component class/outer/pawn identity guards。
- game-thread enforcement。
- command admission close、paint request serialization、queue/planning/async/terminal tracking。
- listener-thread forced state mutation の除去。
- hook original target を resident DLL lifetime 中保持。
- C# bridge instance generation guard と state lock。
- C# shutdown timeout 10 秒（native shutdown budget より長い）。
- research runner: exact PID, event-watch, pressure, texture, A/B paint, cancel-after-ms, shutdown-after-ms。
- managed regression tests、native contract test、release/research docs。

## 2026-07-11 single-host implementation and verification update

### HISTORICAL (superseded): initial pacing-mode implementation

この subsection と後続の `AutoFast` / `ManualSlower` / `Compatibility` 名を含む
runtime 表は、slider 化前の build で得た比較証拠としてのみ残す。現在の操作・
payload contract では pacing mode は存在せず、後述の 2026-07-11 follow-up が
最新仕様である。

- `AutoFast` is the default persisted setting. Existing settings with no mode
  migrate to it; a retained `75 ms` value is preserved as the manual value.
- `ManualSlower` accepts only 50--500 ms. `Compatibility` is the old 6/75 ms
  lockstep implementation, not an approximation of it.
- Remote and local lanes own independent offsets, next-due times, and terminal
  state. Queue pressure delays only the remote lane. One scheduler wakeup is
  posted for the earliest due lane; the window/thread double-post path was
  removed.
- If reflected game limit properties cannot be read on a known executable, the
  bridge uses the known contract `20/20/24/6`; a non-matching executable still
  fails closed. Progress/reply metadata records
  `replication_pacing_fallback_reason=game_limit_property_unavailable`.
- The no-resend preflight now deduplicates brush/channel descriptors, processes
  at most 64 descriptors per game-thread turn, and polls cancellation before
  each bounded unit. Planning also polls after sample generation/capture and
  throughout stroke construction.
- Cancelling records the first request tick and returns phase, observation
  delay, server/local committed counts, and partial-commit classification. A
  cancellation response is never an automatic-retry authorization.

The descriptor list currently deduplicates `(brush texture, target channel)`
before validation. The shared component/cache invariant remains within the
descriptor validation helper rather than being a separately named one-time
function. This is a maintainability refinement, not evidence of an unsafe
per-stroke preflight loop.

### IMPLEMENTED: event-watch lifecycle and reinjection

The event-watch writer is now a generation-tagged joinable thread. Shutdown
order is admission stop -> writer disable/join -> paint drain -> hook removal
-> callback rundown -> watch-state reset -> thread-done. A start captures its
identity/path/generation by value; it cannot write after its generation is
inactive. Per-run pointers, counters, samples, queue, and manager state reset
only after writer/callback teardown. Unique staged-DLL reinjection remains
supported after complete teardown.

`same-module` (the exact already-loaded DLL path) was repeated ten times on the
final host bridge; all ten starts returned `listening`, authenticated ping
succeeded, and shutdown reported `active_paint_quiescent=true` and
`hook_callbacks_quiescent=true`.

### VERIFIED: current Steam build identity and resolver update

The current host executable is not the historical build recorded above:

| Field | Current value |
| --- | --- |
| SHA-256 | `1C116AB196771A26D709BF8269E0AC14F965E8FCD1493F8AAD592324F1751C9D` |
| PE timestamp | `0xB76CD1AF` |
| `SizeOfImage` | `0x0AAFD000` |
| PE checksum | `0x0A7394E2` |
| raw `.text` size | `0x07AA2800` |
| bridge raw-text FNV-1a64 | `0x4200B2CAF330F8C3` |

The bridge uses its nonstandard seed for that FNV calculation. An initial
standard-seed calculation was rejected by the strict identity gate and was
corrected; it must not be copied as a generic hash algorithm.

Current-build disassembly confirmed the former no-resend chain with a `-0xBC0`
RVA shift: thunk `0x50E4E20`, implementation `0x50FEA90`, constructor
`0x50DA6A0`, common routine `0x50EE0C0`. The resolver admits exactly the two
documented build identities and selects their corresponding RVAs; it has no
generic code-pattern fallback.

### VERIFIED: single-host runtime results

All paths below used the current host executable and authenticated research
bridge. They prove host-side behavior only.

| Run | Result |
| --- | --- |
| Legacy AutoFast, 1 stroke | success; remote 20/50, local 6/17 selected from reflected game limits; one preflight descriptor; no local exception; elapsed 38.2 ms |
| Legacy Compatibility, 20 strokes | success; 4 packed RPCs of 6/6/6/2 and 20 local strokes; 355 ms |
| Legacy AutoFast, 400 strokes | success; 20 packed RPCs / 400 remote strokes in 1304.561 ms; 67 local calls / 400 strokes in 3694.279 ms; total 3711.776 ms |
| Legacy Compatibility, 400 strokes | success; 67 packed RPCs / 400 remote strokes in 6052.608 ms; 67 local calls / 400 strokes in 7107.451 ms; total 7125.798 ms |
| Legacy AutoFast, 400 strokes, cancel after 100 ms | success; terminal `mesh_paint_cancelled`; planning/queued phase; observed 109 ms after first cancel request; server/local commits 0/0; `partial_commit=false`; writer stopped |

The legacy AutoFast remote lane met the planned 20-RPC and under-1.5-second host
submission target. The local lane did **not** realize its nominal 17 ms cadence:
67 local batches took about 3.69 seconds (roughly a 55--58 ms effective game
thread cadence). This is a verified host scheduler behavior, not an EOS result.
That legacy AutoFast build reduced end-to-end host completion from about 7.13 s
to 3.71 s relative to legacy Compatibility, but it is not a proof of a 17-ms
renderer cadence.

One-stroke earlier host and joining-client smoke artifacts include before/after
Albedo checksums, so the local texture-change criterion is met for that small
case. Production success continues to use CPU submission/terminal semantics,
not a pixel checksum.

The slider migration 前の final-source host-only legacy AutoFast texture run
independently repeated that small-case evidence:
`7574211450705487170 -> 9224129236169773063` for the
resolved component's Albedo export, with one packed RPC, one no-resend local
call, remote `20/50`, and local `6/17`. This is a host texture result only.

### VERIFIED: build gates after the final cancellation-response change

Current source was rebuilt after the final cancellation metadata change:

```text
powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File scripts/research/build-replication-runner.ps1 \
  -OutDir .build/final-current-source

powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File scripts/build.ps1 \
  -BuildMode DevLooseSelfContained \
  -OutDir .build/issue87-single-host-final2
```

- managed regression tests: **46/46 pass**.
- native runtime-contract/transform test: pass.
- native bridge/injector: pass.
- self-contained publish: pass.
- `git diff --check`: pass before the documentation append below; rerun after
  documentation edits as the final hygiene gate.

### Remaining evidence boundary

- **UNVERIFIED:** EOS P2P arrival, joining-client multicast pacing/order,
  remote queue pressure/drain, and remote texture changes for the then-current
  legacy AutoFast build. The VM is no longer available; do not attempt to infer
  these from host timing.
- **VERIFIED (host):** ten consecutive same-module reinjections on the final
  bridge. This does not imply any EOS or joining-client evidence.
- **VERIFIED (host representative phases):** planning/queued, bounded preflight
  (research-only independent lanes), and production paired server/local batch
  cancellation were exercised. Texture-sync phases remain out of the production
  route and were not treated as a separate cancellation claim.
- Pixel completion remains a research checksum, never the production success
  condition.

If a user reports late or missing remote paint, first reproduce the same
scene/replay with the sliders set to the former conservative values
`batch limit=6`, `batch pacing=75 ms`. That comparison is a rollback diagnostic,
not proof that the report's explanation is correct.

## 2026-07-11 slider, sustained-FPS, and spatial-order follow-up

### IMPLEMENTED: direct sliders and migration

- Pacing mode は削除した。production UI と research payload は remote lane の
  `batch limit`（1--20、default 20）と `batch pacing`（50--500 ms、default
  50 ms）を直接扱う。UI の faster end は default の 20/50 ms。
- 旧設定 migration は、旧 automatic を 20/50、旧 manual を 20/保存済み値、
  旧 compatibility を 6/75 へ写す。modeが導入される前のconfigに
  `packed_batch_delay_ms`がある場合も、その保存済み50--500 ms値を保持する。
  保存後は旧 mode/delay field を使わない。
- 0 ms pacing は入力にも scheduler recurring wakeup にも許可しない。
  recurring delay は最低 1 ms に clamp し、旧 immediate message repost の
  busy loop を再導入しない。
- remote slider は EOS または game-owned limit を書き換えない。既知 build で
  reflected limit を読めない場合だけ既知 contract を使い、実行ファイル不一致
  は従来どおり fail-closed。

### VERIFIED: sustained game-thread pressure attribution

以下は current host、100 ms sampling の process-thread CPU 観測である。
これは表示 FPS の直接計測ではないが、長時間 paint 中に game thread が飽和する
区間を比較するための同一条件 A/B である。artifact 名に残る `autofast` は旧実験
名であり、mode が現行仕様に残っている意味ではない。

| Run | GameThread CPU | Submission timing |
| --- | --- | --- |
| Slider化前 20/50 baseline, 400 | idle avg 46.7%, p95 62.9%; paint avg 65.5%, p95 100.1%, max 102.3% | remote 20 RPC / 1360.9 ms; local 67 dispatch / 3242.4 ms; total 3252.4 ms |
| Slider化前 conservative 6/75, 400 | idle avg 49.1%, p95 57.0%; paint avg 61.4%, p95 100.3% | remote 67 RPC / 5911.7 ms; local 67 dispatch / 6653.5 ms; total 6664.3 ms |
| Hard 1-local-stroke candidate, 400 | paint avg 59.3%, p95 87.7%, max 101.1% | local 400 dispatch / 14763.9 ms; rejected as too slow |
| **Rejected 4 ms adaptive candidate**, 400 | idle avg 48.5%, p95 59.1%; paint avg 58.1%, p95 88.3%, max 102.3% | remote 20 RPC / 1238.0 ms; local 205 dispatch / 8208.0 ms; total 8226.4 ms |
| Packed-only 20/50 attribution, 400 | idle avg 48.5%, p95 63.0%; paint avg 55.4%, p95 100.7% | remote 20 RPC / 1308.8 ms; no local apply; total remote 1308.8 ms |

This rejected candidate の local lane は最大 6 stroke/dispatch のまま、各 internal
common call 後に game-thread elapsed を測り、4 ms 到達時に yield する。400
stroke では 205 dispatch、CPU-budget yield 204 回、common-call total
1111.45 ms / max 9.43 ms、dispatch max 13.09 ms だった。`All` channel の
4 target write を数える soft cap は 24 writes/dispatch で、1600 writes、
write-cap yield 0 回だった。game manager の receiver-side
`MaxRenderTargetWritesPerFrame=6` を local stroke 数として誤用してはいない。

この A/B から verified と言えるのは、network pacing を 6/75 へ遅くするだけでは
p95 100% burst が消えず、synchronous local common apply が sustained pressure
の主要 contributor であること、4 ms adaptive yield が p95 を 100.1% から
88.3% へ下げたことである。一方、最大 burst は残り、完了時間は約 3.25 s から
約 8.23 s へ伸びた。したがって「FPS drop 解消」ではなく、視覚応答と総時間の
trade-off を選んだ緩和である。

別の初期 hitch として synchronous SceneCapture/readback 約 645 ms を含む
planning/capture 約 1 秒も観測している。これは long-running local apply の
sustained pressure とは別経路である。GPU WPR trace は current shell が非昇格で
`0xc5585011` となり取得できなかったため、GPU bottleneck の結論は出していない。

progress JSON は同一 phase の非終端 update を 250 ms 間隔へ抑え、candidate
400 では 32 write / 177 suppressed だった。scheduler は一 wakeup につき一度
だけ tick する。これらは game-thread 上の同期 I/O と余分な drain を減らすが、
上表は local common apply 自体が残る主要コストであることを示す。
Packed-only の短い paint sample は planning/capture 約 1 秒を含むため p95 は
100.7% だが、paint average と local-lane がない 1308.8 ms の submission が
sustained local cost の attribution に使える値である。

Relevant host artifacts:

```text
C:\MecchaResearch\perf\cpu-baseline-autofast400-20260711
C:\MecchaResearch\perf\cpu-fps-budget400-20260711
C:\MecchaResearch\perf\cpu-fps-budget400-candidate2-20260711
C:\MecchaResearch\perf\cpu-packed-only400-candidate2-20260711
C:\MecchaResearch\runs\fps-budget2-autofast400-20260711
C:\MecchaResearch\runs\fps-packed-only400-20260711
```

### SUPERSEDED TESTED: local-Z spatial presentation order

元の実装順は推測されていた Front-first ではなく、`Fill -> Paint` の各 pass で
`Back -> Side -> Front` だった。この phase/region 順は変更していない。
fill dedupe と stroke construction の後、各 mode/region partition 内の final
stroke list だけを component-local Z の上から下へ row 化し、camera-right の
左から右へ stable sort する。remote/local lane は同じ final list を消費する。

The superseded 400-stroke candidate は spatial partition violation 0、sort 約 5.34 ms
だった。これは submission order の証拠であって、render-thread completion、
remote 到着順、または最終 pixel の証拠ではない。overlap pixel は Override の
stroke order 変更により以前と変わる可能性がある。

### SUPERSEDED TESTED: internal-common slider-source host verification

Current single-host process は PID `27700`。最終 runtime-tested native bridge は
SHA-256
`32FC8FBEB5768E27F291C381E282C38233F56A2C58B3FE4D354BFB3913857AFB`
である。

| Run | Verified result |
| --- | --- |
| 1 stroke + texture snapshot | success。local eligible component の Albedo checksum が `10871384332519864912` から `17214513313828482521` へ変化。これは最終 metadata-only 修正前の bridge `96BE88D0...BC8159D` で取得した research evidence。Metallic/Roughness は不変。 |
| Slider 7/125, 20 stroke | success。requested/effective 7/125、packed RPC 3 回（20 stroke）、server 302.23 ms、local 12 dispatch / 20 stroke、CPU-budget yield 11、spatial violation 0。bridge `CF8E0441...EF348B6`。 |
| Default 20/50, 400 stroke | success。packed RPC 20/20、400/400 stroke、server 1193.441 ms、backoff 0。local 213 dispatch / 400 stroke、CPU-budget yield 212、total 8316.904 ms。common-call total/max 1180.995/8.404 ms、dispatch max 9.356 ms、spatial violation 0、sort 4.968 ms、progress write/suppressed 32/182。final bridge hash。 |
| Same-module reinjection | final bridge hash を shutdown 後に 10 回連続で再注入。10/10 で paint、shutdown、event-watch stopped が成功し、各回の instance GUID は unique。 |

Artifacts:

```text
C:\MecchaResearch\runs\slider-default-texture-20260711
C:\MecchaResearch\runs\slider-custom-7-125-20260711
C:\MecchaResearch\runs\slider-final-default400-20260711
C:\MecchaResearch\runs\slider-final-same-module-v4b-20260711
```

Final release build は `ReleaseSingleFile` で managed 50/50、native contract、
bridge/injector compile、native dependency allow-list、self-contained publish が
すべて pass。生成物は 55,457,394 bytes、SHA-256
`080EF194F324BA67423DE2E4C30A86F7280923A45523C3FE6104F846F9C156B0`。
embedded native bridge の hash は上記 runtime-tested bridge と同一。
managed regression には brush-only reset が二つの slider を保持することと、
数値入力の小数値を step=1 の最寄り整数へ量子化することも含む。

### TESTED: native packed local receiver + two-brush route

上記4 ms internal-common candidateはlocal completionを約8.2秒まで遅くしたため、
production標準から外した。current Steam executableのreceiver経路をdisassemblyし、
remoteと同じgame-native packed decoder/queueをlocalにも使う経路へ置換した。

Verified current Steam native chain:

```text
MulticastPackedPaintBatch UFunction thunk RVA 0x50E40E0
  -> component vtable +0x550 / RVA 0x50FBD80
  -> packed decoder RVA 0x5103A10
  -> compact-to-full/enqueue inner RVA 0x50F5EE0
  -> manager enqueue RVA 0x5109030
  -> queue coalescer RVA 0x5108E30
  -> exact RuntimePaintReplicationManager +0x68 queue
```

Receiverはpacked SourceIdがcomponent `+0x2A8`のself IDと一致するstrokeを捨てる。
local payloadだけself IDのhigh bitを反転したnonzero IDを使い、server payloadには
本物のself IDを維持する。reflected multicast UFunctionはlocal applyに使用しない。
component property、manager limit、EOS設定は変更しない。

Safety boundary:

- current Steam PE/text identity、reflected param layout、UFunction thunk、vtable、
  decoder、source filter、component-to-manager resolver、enqueue chainをbyte/RVA guard。
- component自身のworld/contextからexact managerを解決し、global first-instanceを
  success判定に使わない。
- 各server commit前にcomponent/source ID、exact manager、component queue getterを
  再確認。各local call後は同じmanagerのcomponent queueがbatch stroke数だけ増えた
  ことを必須postconditionにする。
- server/local offsetとbatch数をpairedにし、pass境界を一つのRPCが跨がない。
- painter-local receiver backlogはremote peerのpressureではないためserver backoffに
  使わない。game-owned queue/render budgetが非同期drainする。
- failure/cancelの`partial_commit`はserver/localどちらかに副作用が一件でもあれば
  true、`server_local_diverged`はcount不一致、automatic retryは常にfalse。

最初のresearch buildはsource-filter signature guardを`implementation+0xD7`と
誤記しfail-closedした。disassembly上の正しい開始offsetは`+0xD5`。artifactは
`packed-local-two-brush-1-20260711`で、RPC/local side effectは0だった。

Runtime-tested no-backoff research bridge SHA-256:

```text
D8F9D94F20F6EB5366956553D14E354F80296AD479F02F4255E8BC4BEAF9F08B
```

Current final ReleaseSingleFile hashes after the independent correctness review:

```text
bridge:   F9603930D046184B2B46E3AAB3B59E3DFF96CCA55D057B1A5AED74A585CB0125
exe:      1994689211C3186146A7D358866948D6AC7F34127061CB0E26A37E329EC40F7E
injector: 1CB8F79FEA4D33D9E9DBF1554183F1776D01F69DA5E78AB5624843702C6E9DAE
exe bytes: 55,471,589
artifact: .build/final-two-brush-packed-local-v3/meccha-camouflage.exe
```

| Run | Verified result |
| --- | --- |
| 1 stroke, guarded receiver | server/local 1/1、queue `0 -> 1`、raw manager `+0x170`は`+1`、exception/mismatch 0、queueは250 ms未満にdrain。選択点のtexture checksum差は0なのでpixel proofには使わない。 |
| 20 stroke, green Fill | server/local 1 batch・20/20、total 18.283 ms、queue `0 -> 20`、exception/mismatch 0。Albedo `10734646601858220931 -> 8318389312888345147`、Metallic `17818754540235457411 -> 14098363518217146147`、Roughness `7187816641027965827 -> 10801802151497500803`。event-watchはServerPacked 1、通常のself MulticastPacked 1、PaintAtUVWithBrush 0、full/compact/relay 0。direct local callはProcessEvent countを増やさない。 |
| 400 stroke, local-pressure gate experiment | paired 21 batch・400/400、pass-boundary split 1、local queue mismatch 0だがpainter-local backlogで48回backoffし4.456秒。remote pressureの根拠にならないためこのgate policyを棄却。 |
| 400 stroke, no local-backoff | paired 21 batch・400/400、pass-boundary split 1、backoff/exception/mismatch 0、server 1397.001 ms、local 1380.929 ms、total 1398.099 ms。last batch queue `213 -> 224`（11 strokes）。5秒hold後checksum変化を確認。 |
| 400 stroke + 100 ms CPU sampling | server/local 21 batch・400/400、total 1279.019 ms、backoff/exception/mismatch 0、last queue `220 -> 231`。idle game-thread avg/p95/max `49.349/50.734/57.204%`、active `57.453/89.514/102.451%`、post `48.038/62.928/63.549%`。FPS/GPU値ではない。 |
| Pre-review ReleaseSingleFile, 400 stroke | embedded bridgeをfresh inject。server/local 21 batch・400/400、server 1288.087 ms、local 1280.153 ms、total 1288.926 ms、backoff/exception/mismatch 0、pass split 1、spatial violation 0、last queue `221 -> 232`（11 strokes）。job完了約0.25秒後の212から減少し、約4.09秒後にqueue 0。Albedo checksum `15418461807976393871 -> 16648994653185883127`。event-watchはServerPacked/MulticastPacked各21・400、PaintAtUV/full/compact/relay 0。 |
| **Current final native bridge, 1500 stroke** | v2 packageから、v3とbyte-identicalな上記final bridge hashをno `--paint-mode`で注入。server/local 76 batch・1500/1500、total 5002.429 ms、backoff/queue-delta mismatch/spatial violation 0、pass-boundary split 2。Fill 109 + Coarse 1333の後にFine 58までruntime到達。job完了約0.51秒後のqueue 1062から減少し、約14.66秒後に0。Albedo `14845144400513423667 -> 15097353053852569605`、Roughness `8470915449664384061 -> 9386488232151764077`。event-watchの非0 callはServerPacked/MulticastPacked各76・1500だけ。 |
| **Current final cancel** | 2000 limit、1300 msでcancel。Fill境界のserver/local 6 batch・109/109までpaired commit後に停止。`partial_commit=true`、`server_local_diverged=false`、queue-delta mismatch 0、automatic retry false、terminal `mesh_paint_cancelled`。 |
| **Current final shutdown/reinject** | 2000 limit、1300 msでshutdown。server/local 6 batch・109/109、partial true/diverged false、active paint quiescent true、event-watch stopped。直後に同じbridge hashを再注入し、1 stroke 1/1、shutdown、event-watch stopが成功。 |
| **Current final v3 package smoke** | migration修正後の最終exeから上記final bridge hashを再注入。1 strokeでserver/local 1/1、queue-delta mismatch 0、pipeline version 2、shutdown/event-watch stopが成功。pre-mode configの保存済75 msがslider値として保持されたこともartifactで確認。 |

CPU A/Bの重要な結果は、slow 4 ms candidateのactive p95 `88.3%`とほぼ同じ
`89.5%`を保ちつつ、400 stroke submissionを約`8.23 s -> 1.28 s`へ短縮したこと。
最大瞬間値は約102%で残るため「FPS drop完全解消」とは言わない。planning中の
SceneCapture/readback約0.5--1秒も別のinitial hitchとして残る。

Two-brush plannerの同じ400 artifact（limit前）:

```text
total 5596 = Fill 109 + CoarsePaint 1333 + FinePaint 4154
Fill candidates/deduped: 2337 / 2228
Coarse candidates/deduped: 4154 / 2821
reference-position fallback: false (0 candidates)
spatial order violations: 0
```

400 limitはFill 109の後、CoarsePaintの途中まででFinePaintには到達しない。
current finalの1500 limitはFill 109 + CoarsePaint 1333を終え、FinePaintの先頭
58 strokeまでruntimeで到達した。したがってpass transition、paired batch境界、
queue drainはTESTEDである。一方、全5596 strokeの最終visual durationと完成画素は
UNVERIFIEDであり、1500 queue drainから単純外挿した値をrelease claimに使わない。

Independent final reviewでは、paint中にcomponentのcontext/managerが差し替わる
edge caseと、server RPC成功直後のcancelの競合を追加修正した。各RPCの直前に
component -> context -> exact managerを再解決し、captured managerと不一致なら
RPC前にfail-closedする。paired modeではcancelをRPC前か、server/localの同数commitと
queue-delta postcondition後にだけ観測する。また通常paintは
`brush_pipeline_version=2`を必須とし、欠落・旧・未知・小数versionを最初のRPC前に
`mesh_brush_pipeline_version_unsupported`で拒否する。preview/unpreviewはplannerを使わないため除外。

円状進行の原因はUVではなく、旧sortがposed runtime `local_position.Z`を使い、
上面視点で頭・胴体の表面高を等高線状に辿っていたこと。新sortはtriangle indexと
barycentricからprofile bind/reference positionを復元し、そのZを使う。pure native
contractはFill一回、Skipゼロ、coarse dedupe、fine全件、reference Z優先、同一rowの
camera-right順、Back/Side/Front、pass境界を検証済み。実際の見た目が完全に意図どおり
かはhost画面での人間による最終確認が必要。

Artifacts:

```text
C:\MecchaResearch\runs\packed-local-two-brush-1-v2-20260711
C:\MecchaResearch\runs\packed-local-two-brush-20-v2-20260711
C:\MecchaResearch\runs\packed-local-two-brush-400-v2-20260711
C:\MecchaResearch\runs\packed-local-two-brush-400-v3-20260711
C:\MecchaResearch\runs\packed-local-two-brush-400-cpu-v3-20260711
C:\MecchaResearch\perf\cpu-packed-local400-v3-20260711
C:\MecchaResearch\runs\packed-local-two-brush-final400-20260711
C:\MecchaResearch\runs\packed-local-two-brush-final-cancel-20260711
C:\MecchaResearch\runs\packed-local-two-brush-final-shutdown-20260711
C:\MecchaResearch\runs\packed-local-two-brush-final-reinject-20260711
C:\MecchaResearch\runs\packed-local-two-brush-final-v2-transition1500-20260711
C:\MecchaResearch\runs\packed-local-two-brush-final-v2-cancel-20260711
C:\MecchaResearch\runs\packed-local-two-brush-final-v2-shutdown-20260711
C:\MecchaResearch\runs\packed-local-two-brush-final-v2-reinject-20260711
C:\MecchaResearch\runs\packed-local-two-brush-final-v3-packaging-smoke-20260711
```

### Evidence boundary after this follow-up

- **VERIFIED on host:** two-brush payload/migration/UI、20/50 packed submission、
  paired local receiver enqueue/exact queue delta、queue drain、local texture checksum、
  400-stroke timing、1500-stroke coarse-to-fine transition、game-thread CPU sampling、
  reference-Z list order、paired cancel/shutdown/reinject。
- **UNVERIFIED:** EOS P2P arrival、joining-client multicast/order、receiver queue、
  low-FPS client での pressure、remote texture/pixel result。今回Hyper-Vは使っていない。
- CPU sampling は FPS counter または GPU trace ではない。p95 改善から display
  FPS の絶対値を推定しない。
- Pixel completion は引き続き research checksum のみで確認し、production
  success 条件にはしない。
- 低速・欠落報告の比較値は mode ではなく slider の 6/75。default 20/50 を
  joining-client で検証済みとは記載しない。

## 2026-07-11 single-host CPU/GPU follow-up

Hyper-Vを使わず、hostのゲームPID `30772`だけで追加計測を行った。対象gameは
UE5.6 `CL-44394996`、SHA-256は既存のcurrent Steam identity
`1C116AB196771A26D709BF8269E0AC14F965E8FCD1493F8AAD592324F1751C9D`、runnerは
`1994689211C3186146A7D358866948D6AC7F34127061CB0E26A37E329EC40F7E`、native bridgeは
`F9603930D046184B2B46E3AAB3B59E3DFF96CCA55D057B1A5AED74A585CB0125`である。

### VERIFIED: single-host runtime still works without the VM

- authenticated passive probe: success。manager/component queueは開始・終了とも0、
  event-watch start/stopとも成功。
- 400 stroke、batch `20/50`、texture import/readbackなし: server/local `400/400`、
  packed RPC `21`、server elapsed `1326.335 ms`、local visual-sync enqueue
  elapsed `1311.296 ms`、queueはterminal probe時点で `222 -> 233` strokeだった。
  runnerの5秒hold後にpressure probeでqueue `0`を確認した。
- 全5596 stroke、batch `20/50`、texture import/readbackなし: server/local
  `5596/5596`、packed RPC `281`、server elapsed `18656.044 ms`、local enqueue
  elapsed `18647.349 ms`。terminal probeではqueue `4089 -> 4103`、観測drain rate
  `72.574 stroke/s`、推定170.96 tickだった。60秒hold後のpressure probeでは
  manager/component queueとも0になった。
- local queueへ積む処理と、その後のgame-owned receiver/render drainは別時間である。
  `mesh_first_paint_done`はqueue drain完了を意味しない。

### VERIFIED: non-elevated CPU sampling

既存の100 ms process-thread samplerで最大累積CPU thread（bridgeのgame-thread call
thread、TID `47908`）を追跡した。`hold=0`でpaint区間を分離した結果は、idleの
平均/p95/max `45.046/62.741/62.878%`に対し、activeは
`60.533/75.779/101.322%`、postは`47.333/62.754/63.043%`だった。これは
GameThread側にpaint中の追加負荷があることの証拠だが、FPS値でも全thread合計でも
ない。process全体のactive CPU deltaは約`4703 ms / 2757 ms`で、複数threadが動いて
いることも確認できる。

### RESEARCH-ONLY: GPU Engine counter

管理者権限を要求しない `GPU Engine(*)\Utilization Percentage` sampler
`scripts/research/sample-process-gpu.ps1`を追加した。400 strokeのactive区間で
対象processの3D engine値は取得サンプルが `10.54%`, `25.822%`, `15.388%`、idleは
`25.324%`, `25.820%`、postは約`20.7--24.8%`だった。copy engineはほぼ0%だった。

これはGPUが長時間100%で飽和していることを支持しない弱い観測である。ただしWindows
performance counterの取得自体が約1.3秒かかり、activeサンプル数が少ないため、
RenderThread/GPU bottleneckを否定する正式なtraceではない。FPS、frame time、GPU
queue待ち時間は未計測であり、GPU root causeは`UNVERIFIED`のままとする。

全5596 strokeのCPU samplerでも、追跡GameThread TID `47908`はidle平均/p95/max
`46.791/56.659/63.287%`、active `49.065/74.812/101.574%`、post
`45.887/50.998/62.990%`だった。active区間は約`20117 ms`、process全体CPU deltaは
約`45406 ms`、追跡GameThread deltaは約`9859 ms`である。これは全CPUが飽和している
結果ではなく、単一GameThreadの瞬間的なburstと、別threadを含むCPU処理が同時にある
ことを示す。FPSの直接値ではないため、thread数変更の効果をこの数値だけから推定
しない。

### 結論と次の実装境界

このsingle-host evidenceでは、worker thread数を増やすだけで現行manager queueの
per-stroke RT commitが並列化される根拠はない。GameThread上のUObject/queue境界と、
同じRenderTargetへの順序付きwriteが残るため、`-USEALLAVAILABLECORES`や別tick
threadはproduction fixにしない。

texture importを使わない根本候補は、同じ20-stroke境界を保ったprogressive local
batch renderである。CPU workerはstrokeのdecode/partitionだけを担当し、GameThreadは
batch descriptorを渡し、RenderThread/RHI側でtarget/channelごとの一括drawまたは
compute passを実行する。各batch完了後に進捗を更新できるため、最後に一括importする
方式ではない。既存game helperのABI、blend順、channel semantics、checksum一致を
確認するまでは実装・production切替を行わない。

### STATIC VERIFIED: current-build per-stroke paint helper shape

current Steam buildのno-resend common RVA `0x50EE0C0`を再逆アセンブルした。
commonはcomponent context/managerを解決した後、各strokeのdescriptorを構築し、
componentのAlbedo/Metallic/Roughness/Height target (`+0x148/+0x150/+0x158/+0x160`)
をchannelごとに選択する。その後、同じstrokeについて内部helper RVA
`0x50F4F30`または`0x50F52B0`を呼び、`All`では複数channelのdispatchを繰り返す。
一方のhelperはさらに`0x50F4920`のbrush/pixel候補生成ループを呼び、両helperとも
stack上の一時配列・descriptorの構築と解放を行う。

このchainにTaskGraph worker dispatchや複数RenderThreadへの分岐は見つからなかった。
これは「全処理がGPUで実行される」という意味ではなく、GameThreadから呼ばれる
per-stroke CPU preparation/command constructionが存在することの証拠である。helperの
引数はlive component、target UObject、内部配列を含むため、任意workerから直接呼ぶ
案は安全性未証明でproductionには使わない。batch化するならこのper-stroke helperを
別threadで単純に並列呼び出すのではなく、順序を保ったdescriptor生成と、UEの
RenderThread/RHI同期を伴う専用batch passが必要になる。

Artifacts:

```text
C:\\tmp\\meccha-camouflage\\artifacts\\research\\single-host-passive-20260711
C:\\tmp\\meccha-camouflage\\artifacts\\research\\single-host-paint400-20260711
C:\\tmp\\meccha-camouflage\\artifacts\\research\\single-host-cpu400-nohold-20260711
C:\\tmp\\meccha-camouflage\\artifacts\\research\\single-host-gpu400-v2-20260711
```

## 2026-07-11 Issue #87 stabilization implementation and final host matrix

This section records the implementation performed after the single-host plan was
accepted. It supersedes only the earlier baseline numbers; the evidence boundary
below is unchanged.

### Source changes

- Added a bounded `ReplayPassWindow` contract and native progress/reply metadata:
  `replay_pass_order`, fill/coarse/fine boundaries, server/local offsets, and
  current pass plus its `[start,end)` range. The paired production route now makes
  it possible to distinguish Fill, CoarsePaint (Brush 1), and FinePaint (Brush 2)
  while the job is still running.
- Added the same pass fields to the managed progress snapshot and runtime log.
- Coalesced listener/timer/ProcessEvent scheduler wakeups with a pending-message
  gate. This removes duplicate queued wake messages without changing UE worker
  thread counts, game limits, or render-target semantics.
- Removed legacy `server_batch_delay_ms`, `local_batch_delay_ms`, and
  `legacy_server_pacing_mode` from new user-visible native metadata. Legacy input
  parsing remains read-only migration compatibility; C# settings/payload/save data
  continue to expose only Batch limit and Batch pacing.
- The normal non-preview route still requires `ServerPackedPaintBatch` and uses
  the exact native packed receiver queue. Local texture import and
  `PaintAtUVWithBrush` are not selected on that route (`local_texture_import_started`
  stayed false in every packed-local-queue run).
- Added native pass-window/managed progress contract tests. The final Windows
  build ran **59/59 managed tests**, native transform/contract validation,
  bridge/injector compilation, dependency allow-list checks, and self-contained
  publish successfully. `git diff --check` is clean.

### Final build identity

The final host runtime was built with `DevLooseSelfContained`:

```text
app exe:       7c4f925e7b2be8bc39756fd41e199d4013f845c3194af91dbe9ed531a467be20
runtime bridge:2ee12263602461eb57413815262e604eb312702384a903acd8f5143983f89bfc
injector:      1cb8f79fea4d33d9e9dbf1554183f1776d01f69da5e78ab5624843702c6e9dae
game exe:      1C116AB196771A26D709BF8269E0AC14F965E8FCD1493F8AAD592324F1751C9D
game PID:      30772
UE build:      ++UE5+Release-5.6-CL-44394996
```

### Final host runtime matrix

All rows used `research_route_mode=packed-local-queue`, no texture snapshot/import,
and the authenticated bridge above. `server/local` are completed stroke counts;
`queue mismatch` is the exact local receiver queue-delta check. **Correction:**
these rows prove planning, submission, enqueue, and drain only. They did not prove
painted texel coverage. The world-radius investigation below demonstrated that
this matrix was a visual false positive even though every counter was correct.

| setting | strokes | RPCs | server/local | elapsed | queue mismatch |
| --- | ---: | ---: | ---: | ---: | ---: |
| 20/50 | 1 | 1 | 1/1 | 8.874 ms | 0 |
| 20/50 | 20 | 1 | 20/20 | 9.052 ms | 0 |
| 20/50 | 400 | 21 | 400/400 | 1265.112 ms | 0 |
| 20/50 | 5596 | 281 | 5596/5596 | 18527.657 ms | 0 |
| 6/75 | 1 | 1 | 1/1 | 6.780 ms | 0 |
| 6/75 | 20 | 4 | 20/20 | 279.958 ms | 0 |
| 6/75 | 400 | 68 | 400/400 | 6072.521 ms | 0 |
| 6/75 | 5596 | 935 | 5596/5596 | 84166.144 ms | 0 |

The full 5596 runs held the bridge for 60 seconds after paint. The post-hold
pressure probe reported zero queued batches and zero queued strokes for both
settings. New replies reported `server_batch_pacing_ms`/`local_batch_pacing_ms`
only; the old `*_delay_ms` fields were absent. The planner metadata reported:

```text
pass order:  fill,coarse_paint,fine_paint
boundaries:  fill_end=109, coarse_end=1442, fine_begin=1442
source pass counts: Fill=109, Brush1 coarse=1333, Brush2 fine=4154
```

The 400-stroke limit ends inside the coarse pass (`coarse_end=400` in the
effective job), so it is also a direct runtime boundary test rather than only a
terminal all-fine case. Final matrix artifacts:

```text
artifacts/research/issue87-final-1-fast-20260711
artifacts/research/issue87-final-20-fast-20260711
artifacts/research/issue87-final-400-fast-20260711
artifacts/research/issue87-final-5596-fast-20260711
artifacts/research/issue87-final-1-compat-20260711
artifacts/research/issue87-final-20-compat-20260711
artifacts/research/issue87-final-400-compat-20260711
artifacts/research/issue87-final-5596-compat-20260711
```

### Lifecycle/cancel evidence

- `cancel-after-ms=1` stopped in `planning_or_queued` with zero committed
  strokes, `partial_commit=false`, and `automatic_retry_safe=false`.
- `cancel-after-ms=3000` on the production paired route stopped in
  `server_batch`: 649 server and 649 local strokes, `server_local_diverged=false`,
  `partial_commit=true`, `automatic_retry_safe=false`, and zero queue-delta
  mismatches.
- `shutdown-after-ms=3000` stopped in `server_batch`: 629/629 strokes,
  `server_local_diverged=false`; shutdown reported
  `active_paint_quiescent=true` and `hook_callbacks_quiescent=true`.
- A separate `combined-no-resend` research run reached preflight before cancel;
  its independent lanes intentionally showed 709 server vs 96 local strokes.
  This is not the production packed-local-queue route and is retained only as
  evidence that divergence is classified and automatic retry remains disabled.
- The exact final bridge DLL path was then reused after shutdown for 10 direct
  injector starts. All 10 returned `listening`, authenticated `ping` succeeded,
  and shutdown returned `active_paint_quiescent=true` plus
  `hook_callbacks_quiescent=true`; all 10 sidecars reached
  `event_watch_stopped` with distinct generations; no game restart was used.

Artifacts:

```text
artifacts/research/issue87-stabilization-cancel-1ms-20260711
artifacts/research/issue87-stabilization-cancel-3000ms-20260711
artifacts/research/issue87-stabilization-shutdown-3000ms-20260711
artifacts/research/issue87-stabilization-preflight-cancel-3000ms-20260711
artifacts/research/issue87-final-same-module-20260711
```

The repeat harness is `scripts/research/same-module-reinject.ps1`; it uses the
already-built injector and the bridge's authenticated hello/shutdown protocol.

### CPU/FPS boundary

No UE worker-thread count, manager tick thread, game-internal limit, GPU batch
route, or final texture import was added. The safe fix implemented here is
wakeup coalescing plus bounded progress writes; the remaining per-stroke
GameThread/UObject/render-target work is an ABI constraint, so this report does
not claim complete FPS elimination. Runtime evidence measures submission time,
queue drain, and native thread/queue counters instead.

### Evidence boundary

The host-only matrix verifies planner pass transitions, packed Server RPC
submission, native local receiver queue, pacing, cancellation, shutdown, and
fresh authenticated reinjection. It does **not** verify EOS P2P arrival,
joining-client multicast order, joining-client queue growth, or remote texture
completion. The 20/50 default is therefore implemented and host-validated but
must not be described as joining-client verified until a second live client is
available.

## 2026-07-11 packed mesh-anchor first partial fix (superseded below)

### User-visible incident and correction of earlier evidence

At 17:56 JST, host PID `12692` reported a nominally successful full job:

```text
pass complete 5596-5596 | batch 20/20 | pacing 50ms |
queue 4115 strokes (drain 70/s) | elapsed 18s | completed
```

The user observed that the character was not painted except for a few tiny dots.
This observation was correct. The earlier pass metadata, equal server/local
counts, exact queue deltas, queue drain, and texture hash changes were all real,
but none established painted area. A few changed pixels are sufficient to change
a whole-texture hash. Consequently, all earlier statements that inferred visual
Brush 1/Brush 2 success from those counters alone are superseded by this section.

### VERIFIED first root cause, but not the complete coverage fix

Our generated mesh-anchor stroke copied the normalized UV radius into
`FPaintStroke::EffectiveBrushWorldRadius`:

```text
Brush 1: 20 / 1024 = 0.01953125
Brush 2: 10 / 1024 = 0.009765625
```

Exact-build disassembly establishes the packed receive semantics:

- packed decoder RVA `0x5103A10` decodes the UV brush radius into compact-stroke
  offset `+0x0c`, and `EffectiveBrushWorldRadius` into `+0x18`;
- compact-to-`FPaintStroke` expander RVA `0x50F65A0` copies those fields to
  `FPaintStroke +0x68` and `+0xb4` respectively, then calls skeletal preflight
  RVA `0x50F6110`;
- preflight derives the mesh-correct world radius from mesh scale/bounds and the
  UV brush radius **only when `FPaintStroke +0xb4 <= 0`**;
- the positive normalized value therefore disabled the game's intended
  conversion and was interpreted as an extremely small world-space radius.

The source assignment itself predates this worktree: blame traces it to
`55222250` (2026-06-26), while skeletal mesh-anchor replay was added by
`adffd486` (2026-06-29). The visible regression nevertheless began during the
current two-brush work because production host-local drawing was changed from
the older internal-common path to the same native packed receiver used by a
joining client. The route-only A/B below verifies that this change exposed the
pre-existing packed-field bug on the host. Therefore the user's timing was
accurate even though the bad assignment was older than the Brush 1/Brush 2
planner.

### First partial fix and fail-closed guard

Mesh-anchor strokes now set `EffectiveBrushWorldRadius` to the verified
non-positive auto-conversion sentinel (`0.0f`). The packed encoder also rejects
generated mesh-anchor strokes whose world-radius field is positive, so a future
refactor cannot silently reintroduce tiny-dot painting. Native contract coverage
asserts that `0.0f` requests conversion while `20/1024` does not. Runtime metadata
now records the first stroke's world radius and per-pass brush/world-radius and
effective subdivision values. The earlier field described as `packed diameter`
was later proven to be `EffectiveSubdivisionLevel`; the correction is recorded
in the next section.

No texture import, reflected `PaintAtUVWithBrush`, game limit rewrite, or GPU
route was used by the fix.

### VERIFIED controlled host A/B

The research texture probe was extended to retain the resolved component's
previous exported Albedo/Metallic/Roughness bytes and report actual changed
texels. Both A/B runs used the same host, camera, red base texture, Front=Paint,
Side/Back=Skip, Brush 1=20, Brush 2=10, batch `20/50`, and the same planned work:

```text
Brush 1 coarse: 645 strokes
Brush 2 fine:  2337 strokes
total:          2982 strokes
RPCs:            150
server/local: 2982/2982
post-hold queue: 0
```

| build | bridge SHA-256 | changed texels | ratio |
| --- | --- | ---: | ---: |
| legacy UV-as-world comparison | `eb49c205f53732348da4c12607813537ca67e5e8b1766258872bd274aba436dc` | 95 / 1,048,576 | 0.0091% |
| fixed game-derived sentinel | `db5d5f7b087a46acfe4c3e8226a282cb8ecc9013b2c56a9258155f92094f9e59` | 61,352 / 1,048,576 | 5.8510% |

The fixed run changed about **646 times** as many texels with identical pass,
RPC, and queue counts. This isolates the radius field from the planner and
scheduler. A separate fixed all-region Fill run used 403 strokes/21 RPCs and
changed 634,814 / 1,048,576 texels (60.5406%), independently confirming that
large brushes paint an area rather than points.

### VERIFIED route-only regression attribution

To test the user's regression timeline directly, the same legacy comparison
bridge (`eb49...36dc`) with the same positive UV-as-world field, red base,
camera, 645/2337 pass split, and 2,982 strokes was run through the old
`combined-no-resend` local internal-common route. Only the local apply route
changed:

| legacy build route | elapsed | changed texels | ratio |
| --- | ---: | ---: | ---: |
| native packed local receiver | 9,998 ms | 95 / 1,048,576 | 0.0091% |
| old internal-common no-resend | 57,256 ms | 276,588 / 1,048,576 | 26.3775% |

This proves that the two-pass plan itself was not the tiny-dot cause. The old
local common route painted a broad area despite the bad packed field, while the
new host-local packed receiver interpreted that field and collapsed the
strokes. The correct fix is to keep the fast/receiver-equivalent packed route
and serialize the auto-conversion sentinel, not to roll production back to the
57-second internal-common path.

Artifacts in this workspace:

```text
artifacts/research/issue87-world-radius-20260711/issue87-radius-fix-fill/run-20260711T091055471Z-eb2f66d204974e1cbea4ea7eafe028d9
artifacts/research/issue87-world-radius-20260711/issue87-radius-fix-brushes/run-20260711T091424561Z-af0ad53345c949f6b0ee210d9cc5affd
artifacts/research/issue87-world-radius-20260711/issue87-ab-reset-red
artifacts/research/issue87-world-radius-20260711/issue87-ab-legacy-radius/run-20260711T091928492Z-776bd0b2fb9f4700823dcf230016d41f
artifacts/research/issue87-world-radius-20260711/issue87-ab-fixed-radius/run-20260711T092101603Z-0ca8f1ddbd0347f8b945938dbcd1e281
artifacts/research/issue87-world-radius-20260711/issue87-route-ab-reset-red
artifacts/research/issue87-world-radius-20260711/issue87-route-ab-legacy-common/run-20260711T093250434Z-6b8c8ef67d6f4546a592bb1a496749d0
```

This proves host-side packed decode, local queue drain, and actual texture
coverage after the fix. Because a joining client is unavailable, EOS delivery
and remote texture coverage remain unverified. The shared decoder semantics make
the remote fix strongly supported, but it must still be described as an
inference until a live joining-client run is repeated.

### Final-source build and pass-boundary smoke

After adding the fail-closed encoder guard and per-pass radius metadata, the
current source passed the full `DevLooseSelfContained` build:

```text
managed tests: 59/59
native contract: pass
bridge/injector: pass
dependency allow-list: pass
DevLoose self-contained publish: pass
app SHA-256:      7c4f925e7b2be8bc39756fd41e199d4013f845c3194af91dbe9ed531a467be20
bridge SHA-256:   2e1f2fa67f86a6b154d876340051d32d3809ffcb86a2cb1db3e512ceef8597d8
injector SHA-256: 1cb8f79fea4d33d9e9dbf1554183f1776d01f69da5e78ab5624843702c6e9dae
```

A 700-stroke final-source host smoke crossed the Brush 1/Brush 2 boundary:

```text
effective boundary: coarse [0,645), fine [645,700)
server/local: 700/700, 36 RPCs
Brush 1: UV radius 0.019531, world-radius sentinel 0
Brush 2: UV radius 0.009766, world-radius sentinel 0
post-hold component/global queue: 0/0
shutdown/event-watch stop: pass
```

Artifacts:

```text
artifacts/research/issue87-world-radius-20260711/issue87-final-source-smoke-v2/run-20260711T093016980Z-eb390702334147a1b4b668fefe9c3ad4
artifacts/research/issue87-world-radius-20260711/issue87-final-source-fine-boundary-v2/run-20260711T093046001Z-e5642a84017d44d7a8a91a2d8e60d39e
```

## 2026-07-11 packed coverage root cause, calibrated fix, and FPS finding

This section supersedes any earlier statement that the world-radius sentinel
alone fully fixed visual coverage. It fixed the catastrophic 95-texel failure,
but queue-zero RGBA dumps still showed a grid of small dots with large gaps.

### VERIFIED compact field contract

Exact-build decoder/encoder disassembly establishes the final four compact
bytes after `EffectiveBrushWorldRadius` as:

```text
EffectiveSubdivisionLevel       u8
EffectiveSubdivisionPixelSize   u8
EffectiveTemplateResolution     u16 little-endian
```

The old custom encoder instead placed brush-diameter-derived data into these
fields. Brush 1/Brush 2/Fill therefore requested levels roughly `40/20/160`,
pixel size `1`, and template resolution `1024`, instead of native preflight.
Live component defaults are pixel size `2`, maximum level `20`, and template
resolution `1024`. The encoder now emits the exact auto-preflight tail
`00 00 00 00`; production serialization and the native contract test share the
same constexpr four-byte helper. This removes bogus high subdivision levels and
an avoidable CPU cost, but did not alone close the paint gaps.

### VERIFIED why packed dots were smaller

The selected `SkeletalMeshComponent` bounds radius is a `double` at `+0x138`:

```text
SphereRadius: 25.100000381469727
diameter:     50.200000762939454
```

Native preflight RVA `0x50F6110` derives mesh footprint radius as
`2 * SphereRadius * BrushSettings.Radius` when the world-radius field is
non-positive. The generator then uses that value as the actual surface brush
radius. There is no hidden factor two in compact encoding: direct UV, native
compact encode/decode, and expansion copy `BrushSettings.Radius` unchanged.

For this runtime mesh, bounds normalization and cached triangle local/UV scale
differ. Brush 2 radius 10 consequently produced typical 6--7 px dots on a
10 px grid. Production now computes one scale per validated mesh/job:

```text
UV-area-weighted mean(max singular value of local-position / UV Jacobian)
--------------------------------------------------------------------------
                    2 * live mesh SphereRadius
```

The scale is frozen for the whole job and applied only to packed mesh-anchor
Brush 1, Brush 2, and Fill radii. Planner spacing, order, stroke count, UI
values, preview, and direct-UV routes are unchanged. Missing/degenerate
triangles, unreadable bounds, a non-finite/out-of-range scale, or a calibrated
normalized radius over 1 fail closed.

Final host values:

```text
valid unique triangles:       2784
invalid triangles:               0
weighted local units / UV: 175.997734
bounds diameter:             50.200001
effective scale:              3.505931
```

### VERIFIED texture A/B

Packed Back comparisons used constant green over a clean red baseline,
Back=Paint, Front/Side=Skip, Brush 1=20, Brush 2=10, 2995 strokes, 150 RPCs,
server/local 2995/2995, and queue zero before the RGBA dump. The old scale-1
baseline contained 87 pre-existing green pixels, so its 107,113 diff undercounts
the final green mask by 87 without affecting later A/B conclusions. Direct UV
is only a local-only research reference (0 RPC/server, 2995 local).

| packed policy | changed texels | Jaccard vs direct UV | only packed | missing vs direct |
| --- | ---: | ---: | ---: | ---: |
| old scale 1 | 107,113 | — | — | — |
| scale 2 | 236,612 | — | — | — |
| scale 3 | 263,546 | 0.89155 | 314 | 31,707 |
| scale 3.4 | 272,733 | 0.91680 | 1,217 | 23,423 |
| scale 4 | 286,901 | 0.90224 | 10,932 | 18,970 |
| per-packet triangle maximum | 300,651 | 0.86803 | 23,895 | 18,183 |
| final dynamic uniform scale | 275,226 | **0.91929** | 2,132 | 21,845 |
| direct UV radius 10 | 294,939 | 1.0 | 0 | 0 |

Scale 1 covered only 46.57% of grid-derived Back support and left 53.43%
holes. Scale 3 covered 99.18%. The final dynamic scale is visually solid and
has the best tested direct-UV mask agreement. Scale 4 and per-triangle maxima
created visible thin arcs outside the direct-UV Back mask, so they were rejected.

A final production-shaped constant-color run used Front=Fill, Side=Paint,
Back=Paint:

```text
Fill:            109 strokes  [0,109)
Coarse/Brush 1: 1333 strokes  [109,1442)
Fine/Brush 2:   4154 strokes  [1442,5596)
RPCs:             281
server/local: 5596/5596
submission:     18.622 s
queue at reply:     14
post-hold queue:      0
changed Albedo: 634,819 / 1,048,576 texels
```

The dump is solid over intended atlas islands; the prior point-grid gaps are
absent. Research-build hashes for the final source are:

```text
app:      2e7496d0a886e89456cd869db9625f17e680d5a3f52b63a5cff065f098fd7206
bridge:   650ae727cd5e9e14a6d2d191af31a4a62c678b924bf992aab1a6ea8acefbdc62
injector: 1cb8f79fea4d33d9e9dbf1554183f1776d01f69da5e78ab5624843702c6e9dae
```

The same source passed the full `DevLooseSelfContained` build: managed `59/59`,
native contract, bridge/injector, restore/publish, and self-contained output.

```text
full app:    7c4f925e7b2be8bc39756fd41e199d4013f845c3194af91dbe9ed531a467be20
full bridge: 650ae727cd5e9e14a6d2d191af31a4a62c678b924bf992aab1a6ea8acefbdc62
```

### VERIFIED batching/queue mechanism; CPU attribution boundary

Exact-build homogeneous-batch selection compares target channel, brush
radius/hardness/opacity/falloff, all quantized ChannelData values, apply mode,
world-position presence, effective world radius/subdivision fields, and other
effective limits. Any mismatch sends the whole receiver batch through the
per-stroke generator fallback. Normal camouflage has differing captured color
bytes, so a 20-stroke network packet is normally not a 20-stroke render batch.

Runtime evidence agrees:

```text
constant-color 5596 reply queue:   14 strokes
varied-color   5596 reply queue: 4095 strokes
varied-color observed drain:     89.59 strokes/s
constant-color observed drain:  315.72 strokes/s
```

This is the strongest verified mechanism consistent with the long host FPS
drop: the game-owned receiver/render queue drains heterogeneous strokes through
per-stroke surface generation. The varied-color CPU sample rose from 41.9%
average on the tracked busiest thread before submission to 52.6% during the
run and remained 51.5% in the two seconds immediately after a 4,095-stroke
queue was observed.
That sampler is not Unreal Insights and does not prove the exact frame-time
share; a later passive sampler began after queue zero and cannot be used as a
drain-vs-idle A/B. Therefore FPS elimination is not claimed.

The subdivision correction removes avoidable work; wakeup coalescing/progress
throttling avoid extra host work. Safely moving this UObject/render-target path
to another thread is not supported by the known ABI. Grouping by color would
destroy requested head-to-feet order, and palette quantization would change
camouflage colors, so neither is silently enabled.

### Evidence boundary and artifacts

Verified here: host planner, packed submission, exact local queue delta, runtime
calibration, queue drain, texture coverage, and exact-build control flow.
Still unverified: EOS delivery, joining-client calibration/queue/texture,
low-FPS remote behavior, and remote multicast ordering.

Primary artifacts:

```text
artifacts/research/issue87-live-coverage/back-full-auto-scale2
artifacts/research/issue87-live-coverage/back-full-auto-scale3
artifacts/research/issue87-live-coverage/back-full-auto-scale34
artifacts/research/issue87-live-coverage/back-full-auto-scale4
artifacts/research/issue87-live-coverage/back-full-local-only-scale1
artifacts/research/issue87-live-coverage/back-full-triangle-batch-radius
artifacts/research/issue87-live-coverage/back-full-dynamic-local-final
artifacts/research/issue87-live-coverage/production-mixed-dynamic-final
artifacts/research/issue87-live-coverage/cpu-varied-dynamic-final
```

## Worktree state

このレポート作成時点で commit/push は行っていない。dirty working tree は調査成果であり、reset/checkout で破棄しないこと。

Modified tracked areas:

- runtime/release/research docs
- `scripts/build.ps1`
- C# controller, tests, WebHost entry
- native bridge, JSON metadata, SDK

Untracked investigation sources:

- `scripts/research/build-replication-runner.ps1`
- `scripts/research/sample-process-gpu.ps1`
- `src/csharp/MecchaCamouflage.WebHost/ResearchRunner.cs`
- `scripts/research/same-module-reinject.ps1`
- `src/native/include/runtime_contract.hpp`
- `src/native/tests/`

Current final sourceは`DevLooseSelfContained`でmanaged **59/59**、native contract、
bridge/injector compile、dependency allow-list、self-contained publishがpass。
Web UIの`node --check`、localization JSON parse、文書更新後の
`git diff --check`もpass。

## Artifact locations

Host artifact root:

```text
%LOCALAPPDATA%\MecchaResearch\runs
```

VM artifact root:

```text
C:\MecchaResearch\runs
```

WSL `/tmp` copies are convenient but ephemeral。長期証拠の primary location として扱わず、重要な数値はこの文書へ転記済み。

重要 directory names:

```text
host-packed-only20-20260711T0050
host-production-combined20-fixed-20260711T0220
host-observe-vm-production-combined20-20260711T0240
host-production-combined400-20260711T0300
host-lifecycle-preflight-smoke2-20260711T0440
host-cancel-planning-20260711T0520
host-post-cancel-reinject-20260711T0525
host-shutdown-planning-20260711T0550
host-post-active-shutdown-reinject-20260711T0555
host-autofast-steam-smoke2-20260711
host-compat20-steam-20260711
host-autofast400-threadwake-20260711
host-compat400-steam-20260711
host-autofast400-cancel-final-20260711
vm-packed-only20-observer-20260711T0050
vm-production-combined20-fixed-observer-20260711T0220
vm-production-combined20-20260711T0240
vm-production-combined400-observer-20260711T0300
vm-production-combined400-texture-current-20260711T0320
vm-lifecycle-preflight-smoke-20260711T0505
```

各 directory の nested `run-*` に次がある。

- `run-summary.json`
- `paint-request.json`
- `paint-reply.json`
- `eventwatch-*.json`
- `pressure-*.json`
- optional `texture-before.json` / `texture-after.json`
- `cancel-paint-reply.json`
- `shutdown-reply.json` または `shutdown-during-paint-reply.json`

## Next-session sequence

1. `git status`, `git diff --check`, and this report. Preserve the existing
   worktree; do not reset/checkout its investigation changes.
2. Review the independent-lane scheduler's unreachable legacy tail and factor
   it only with a focused regression test; it is not a reason to repeat
   multiplayer experiments.
3. If only host is available, repeat at most the specific host test affected by
   a source change: one-stroke checksum, 400-stroke pacing, cancel/shutdown, or
   fresh-path reinjection. Record build hash and artifact directory.
4. When a joining client returns, run the 1/20/400 matrix with the default
   slider values 20/50 in both directions with receiver event-watch, pressure
   sampling, and texture checksums. Compare the exact replay against slider
   values 6/75 before attributing any delay to EOS.
5. If lifecycle code changes again, repeat the exact same-module reinjection
   test after confirming each preceding teardown reaches writer joined + callback rundown.
6. Run the full build (all managed tests, native contract, bridge/injector,
   self-contained publish) after every material source change.
7. Consider commit/PR only after the final diff review; never auto-push.

## Useful commands

Research build:

```text
powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File scripts/research/build-replication-runner.ps1
```

Full build:

```text
powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File scripts/build.ps1 \
  -BuildMode DevLooseSelfContained \
  -OutDir .build/verify-issue87
```

Research runner shape:

```text
meccha-camouflage.exe --research-replication --pid <exact-pid> \
  --role host|joining-client --out <artifact-dir> \
  [--paint] [--paint-mode packed-local-queue|combined|combined-no-resend|local-only|packed-only] \
  [--batch-limit 1..20] [--batch-pacing-ms 50..500] \
  [--stroke-limit N] [--texture-snapshot] \
  [--front-mode paint|fill|skip] [--side-mode paint|fill|skip] \
  [--back-mode paint|fill|skip] \
  [--hold-seconds N] [--pressure-sample-ms N] \
  [--cancel-after-ms N | --shutdown-after-ms N]
```

The runner requires `MECCHA_RESEARCH_ARTIFACTS=1`. Do not log HELLO tokens or other credentials.

## Suggested skills for continuation

- `diagnose`: reproduce -> instrument -> fix -> regression-test discipline。
- `tdd`: lifecycle/managed regression tests before fixes where an observable contract exists。
- `zoom-out`: only if the bridge/Controller ownership boundary must be remapped。
- `handoff`: update this report and create a redacted temporary handoff before changing sessions。

## Release claim boundary

What is complete:

- Issue #87 の duplicate replication root-cause attribution。
- exact-build internal anti-echo call chain の reverse engineering。
- host/client 両方向の packed + local no-resend proof。
- 400-stroke receiver pressure proof。
- transform decoder root cause/fix proof。
- planning cancel/shutdown と no-restart reinjection の実機 proof（tested binary snapshot）。
- repeatable authenticated research harness と artifacts。
- batch-limit/batch-pacing sliders、Brush 1/2 UI/migration、Fill -> Coarse -> Fine planner、
  bind/reference-Z scanline order、strict known-build receiver/manager resolver。
- server packed RPCとgame-native packed local receiver queueのpaired submission、
  per-batch exact-manager continuity guard、paired commit-boundary cancellation。
- bounded descriptor preflight、planner cancellation checkpoints、
  generation-joined event-watch writer、full teardown後の再注入。
- slider 化前 source の full build（managed 46/46）と、current-host の旧
  AutoFast / Compatibility / cancel runtime evidence。
- slider/default 20/50 candidate の host pacing、sustained CPU、adaptive local、
  spatial-order runtime evidence。
- current final sourceのfull build（managed 59/59、native contract、bridge/injector、
  dependency allow-list、ReleaseSingleFile publish）。
- default 400-stroke CPU/timing、current final 1500-stroke Fill/Coarse/Fine transition、
  cancel/shutdown/reinjectのhost runtime evidence。
- packed mesh-anchor world-radius のdecoder/preflight root cause、sentinel fix、
  legacy 95 texel vs fixed 61,352 texel のhost A/B visual-coverage proof。
- compact effective subdivision tail `00 00 00 00`、runtime mesh/bounds radius
  calibration、scale 1/2/3/3.4/4/Jacobian/direct-UV A/B、最終5596 strokeの
  queue-zero RGBA coverage proof。
- heterogeneous color がgame homogeneous batchをper-stroke fallbackさせる
  exact-build control flowと、constant/varied queue差のhost runtime proof。
- current `20/50` buildのhost -> joining-client packed multicast実機試験。
  joining clientは281 RPC / 5596 strokeを受信し、exact manager queueへ5596
  stroke追加、queue-zero、対象componentのAlbedo/Metallic/Roughness checksum変更を確認。

What is not complete:

- raw EOS packet captureとEOS API境界でのpacket-level attribution。
- 低FPS joining client、packet loss、高latencyでの20/50 pressure/recovery。
- 複数hardware/sessionでの反復remote verification。
- release/commit/PR。

したがって、host単体のplanner/local queue/render/server RPC submissionに加え、
default 20/50のjoining-client game-level multicast/queue/texture反映を1セッションで
確認済みである。ただしraw EOS packet、低FPS/損失環境、複数環境での再現性は
未確認なので、それらまで一般化しない。

## 2026-07-11 receiver-drain completion、pass progress、joining-client再検証

### User-visible symptom and verified root cause

ユーザー報告は以下だった。

- 見た目のBrush 2が続いているのに、一周目の終端で`Paint: completed`が出る。
- Brush 1中のETAが一周目の残り時間としては過大。
- 見た目のBrush 2中にF1を押すと、次jobのBrush 1/2がqueueへ追加される。
- 稀にBrush 2の途中からBrush 1が再開したように見える。

20:47の旧traceではsubmission terminal時点でreceiver queueが4119 stroke残っていた。
pass境界は次の通りである。

```text
Fill:             [0, 109)       109 strokes
Brush 1 / Coarse: [109, 1442)   1333 strokes
Brush 2 / Fine:   [1442, 5596)  4154 strokes
```

`5596 - 4119 = 1477`なので、旧`completed`時点の実receiver cursorはBrush 2へ
わずか35 stroke入った位置だった。15秒後のF1は新jobとして受理され、次のterminal
queueは7420まで増えた。exact-build queue coalescerは同component entry末尾へstrokeを
appendし、consumer cursorはforward-onlyである。したがって同一job内のpass巻戻りでは
なく、以下のcross-job列が実際に作られていた。

```text
old Brush 2 remainder -> new Fill -> new Brush 1 -> new Brush 2
```

別の表示-only要因として、native progress writerが`CREATE_ALWAYS`でJSONを直接
truncate/writeし、C# parse failure時に別bridge instanceの新しいprogress fileへ
fallbackしていた。partial JSONと他instanceのstale passを表示できる競合だった。

### Implemented completion/progress contract

- production packed-local routeに`LocalQueueDrain` phaseを追加。
- paired server/local submission終了後も、exact component queueを250msごとに読む。
- `rendered = submitted - queued`を単調cursorとして保持する。
- queue=0を2回連続観測して初めてsuccess/100%/`completed`にする。
- job/F1 admissionはreceiver drain終了まで保持する。
- 新job preflight時にexact component queueが非zero、負値、または取得不能なら
  side effect前にfail-closedする。
- pass sourceをsubmission offsetではなくreceiver cursorへ統一し、
  `Fill / Brush 1 / Brush 2 / Complete`、pass内件数、pass ETA、total ETAを分離。
- packed-localのBrush 1 ETAは、最初のlocal enqueueからの
  `rendered / elapsed`という累積exact queue rateを使う。250ms positive sampleだけの
  EWMAと累積job fixed-costの過大評価を使用しない。
- drain中にqueue cursorが1秒止まれば古いrateを無効化してETAをunknownへ戻す。
  120秒forward progressなしはterminal failure。drain scheduler自体はidle基準だが、
  listener側には最終safety timeout 900秒が残る。このため進行中でも全jobが15分を
  超える極端な低速環境は既知の上限としてterminal化される。
- user cancelがsubmission後ならcommitted queueをdrainするまでownershipを保持。
  shutdown/request timeoutはidle drainを500ms retry付きCASで安全にterminal化する。
- submitted/rendered/remainingを別metadataにし、automatic retryは許可しない。
- progress sidecarはunique tempへの全byte write、`FlushFileBuffers`、
  `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`によるatomic replaceへ変更。
- authenticated bridgeのpreferred progress pathがmissing/malformed/staleなら、
  他instanceへfallbackせずそのtickの表示を隠す。
- global hotkeyは`MOD_NOREPEAT`を使用する。

game内部limit、UE worker thread数、texture import/sync、full/compact/reflected fallbackは
変更していない。queue-zeroはgame managerがstroke処理を終えた証拠であり、GPUの最終
pixel presentationそのものをsuccess条件にはしていない。

### Build verification

Source baseはdirty worktree上の`992bf7cebc629b9f3e1b279f60e9728861414292`
（`origin/main`, `v1.6.0-beta.5`）で、既存調査変更を保持した。

```text
managed tests:                 66/66 PASS
native transform/contract:     PASS
native bridge/injector compile: PASS
native dependency allow-list:  PASS
DevLoose self-contained publish: PASS
git diff --check:              clean
```

Current tested artifact hashes:

```text
runtime-bridge.dll  3130af531fb2d05974aee03f0e68fdd0c2ae9d53c40f35b7279f046ac0846f50
controller dll      4f15bd36cc0a05df62b736106b7c822e5c1def5d621bb31f81e568078aec94b8
webhost dll          38816cdb26ae8db99ec421c9e58839f8f47a69ad08f2e6698267f04fd599c582
core dll             f34f03c0d9de38305d3b412288c1e0cc9a34bffd455b45224a31ccd40d491dc0
launcher exe         7c4f925e7b2be8bc39756fd41e199d4013f845c3194af91dbe9ed531a467be20
```

### Host runtime: completion and F1 regression

Default varied-color full jobのqueue-drain試験:

```text
server/local:           5596/5596
RPCs:                    281
submission:              about 18.622 s
terminal elapsed:        71.315 s
terminal queue:          0
zero observations:       2
drain polls:              201
paint rendered:          5596/5596
```

途中sidecarはsubmission済み5596/5596でも、実描画1816/5596、Brush 2
374/4154、queue 3780、pass ETA約46.1秒を示し、100%/terminalにはならなかった。

GUIで実際のF1 hotkey handlerへWM_HOTKEYを送り、Brush 2中にもう一度F1を送った
traceは次の通り。

```text
21:54:30 Paint: started.
21:54:31 Paint: pass Fill (painting).
21:54:31 Paint: pass Brush 1 (painting).
21:54:49 Paint: pass Brush 2 (painting).
21:54:59 Paint: already running.
21:55:42 Paint: pass Complete (painting).
21:55:42 Paint: overall 100% ... queue 0 ... elapsed 1m 11s
21:55:42 Paint: completed.
```

二回目F1後もserver batch callsは281、server/localは5596/5596のままで、progressは
Brush 2内を2866 -> 3531へ前進し、次jobのFill/Brush 1は追加されなかった。

### Host -> Hyper-V joining-client runtime

同一buildを両環境へ配置した。

```text
host game PID:           52924
joining-client game PID: 14388
batch/pacing:             20 / 50 ms
regions:                  Front Fill, Side Paint, Back Paint
brushes:                  20 / 10
strokes:                  109 + 1333 + 4154 = 5596
```

色を既存textureと区別するためresearch overrideでFill=`#FF00FF`、Paint=`#00FFFF`
とした。host結果:

```text
281 packed RPC
server/local 5596/5596
paint rendered 5596
queue 0, zero observations 2
terminal elapsed 19.670 s
```

joining client event-watchのVERIFIED結果:

```text
MulticastPackedPaintBatch calls:         281
decoded packed strokes:                 5596
exact receiver queue positive delta:    5596
queue-delta observations:                281
MulticastPaintBatch calls:                 0
MulticastCompactPaintBatch calls:          0
MulticastSyncChannelData calls:             0
MulticastSyncCompressedChannelData calls:   0
```

250ms pressure samplingではglobal joining-client queueの最大観測値は20 stroke / 1 batch。
最後の20-stroke観測は`12:50:22.101Z`、最初のqueue-zeroは`12:50:22.391Z`
（約290ms後）で、その後も0を維持した。

Multicast receiver object `0x20444ba2770`のchannel exportは全て変化した。

```text
Albedo:    10734646601858220931 -> 6065757884515075350
Metallic:  17818754540235457411 -> 12319355475083315566
Roughness:  7187816641027965827 -> 16694602342927063148
```

別component `0x20450c713c0`は三channelとも不変だったため、全componentが一様に
変化したのではなく、実Multicast receiver objectとtexture変化が対応している。
observer shutdownは`active_paint_quiescent=true`、`hook_callbacks_quiescent=true`、
event-watch writer stoppedを満たした。

#### Variable-color production-equivalent follow-up

上記`#00FFFF` runはtransport/controlとして有効だが、同色batchはgame側consumerが
速い。production相当のsource colorを保持した同一5596-stroke jobを追加実行した。

host:

```text
281 packed RPC, server/local 5596/5596
paint rendered 5596, terminal queue 0
host drain rate 71.839 stroke/s
host terminal elapsed 71.320 s
```

joining clientは再び281 RPC / 5596 strokeを全件受信し、packed queue deltaも5596、
full/compact/texture-sync trafficは0だった。ただしremote render queueはcontrolと
大きく異なった。

```text
first nonzero: 13:00:27.813Z, queue 53
maximum:       13:00:46.347Z, queue 4917
13:01:40.089Z: queue 2988
13:02:10.006Z: queue 2087
last sample:   13:02:12.711Z, queue 2001
next probe:    13:03:24Z,     queue 0
```

したがって欠落ではなく、joining clientのgame-owned render drainが律速である。
max後の観測drainは概ね30--36 stroke/sで、hostが71.3秒でterminalになった時点でも
remote描画は継続していた。remote queue-zero時刻は`13:02:12.711Z`より後、
`13:03:24Z`以前の区間までしか絞れていない。

remote multicast receiver component `0x20444ba2770`の三channel checksumは再び変化した。

```text
Albedo:    8556978018579755350 -> 5536668930708609230
Metallic: 12319355475083315566 -> 13827957233744589150
Roughness: 16694602342927063148 -> 9094199141862472108
```

これは`20/50`がnetwork/game RPC上限内で全strokeを届けることと、joining clientが
同じ速度で描画を完了することは別contractだと実証する。hostはremote queueを直接
観測できないため、現在のproduction successはremote visual completionを意味しない。
visual lagの切り分けにはBatch pacing sliderを遅くした比較が必要である。

Artifacts:

```text
artifacts/research/issue87-progress-final-20260711/
artifacts/research/issue87-host-to-joining-final/
artifacts/research/issue87-joining-final-run/
artifacts/research/issue87-host-to-joining-varied/
artifacts/research/issue87-joining-varied-run/
artifacts/research/issue87-joining-varied-drain-run/
```

### Evidence boundary after this run

VERIFIED:

- host planner/pass boundary、packed RPC submission、paired local receiver、queue drain。
- production 20/50のjoining-client game-level packed multicast到着。
- joining-client exact queue enqueue/drainと対象component channel checksum変化。
- variable-color production相当runで全5596 stroke到着とremote queue backlog/drain。
- full/compact/texture-sync fallback trafficが0。
- Brush 2中のF1再押下が次jobをappendしないこと。

NOT VERIFIED:

- EOS socket/API境界のraw packet capture、retransmit、loss behavior。
- remote queue-zeroの厳密時刻とFPS/frame-timeとの相関。
- 別の低FPS client、high latency、packet loss時のqueue pressure。
- 別hardware/別sessionでの反復性。
- queue-zero直後のGPU presentation fence。

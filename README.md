# OBS Comp Delay

Windows-first OBS plugin for switching a live stream into a controlled competition delay without stopping OBS streaming output.

## Current implementation

- Stand-alone OBS plugin scaffold based on `obs-plugintemplate`.
- Plugin settings dialog opened from OBS' `Tools` menu as `Comp Delay Settings`.
- OBS source type `comp_delay_playback_source` / `Comp Delay Playback`.
- Scene selection for source, transition, and delay scenes.
- User-selectable FFmpeg encoders for the internal transport:
  - H.264 video encoders, for example `libx264` or `h264_mf` when available.
  - AAC audio encoders, for example `aac` or `aac_mf` when available.
- Delay target clamp at `0-300s`.
- Scene guardrails:
  - source, transition, and delay scenes must be distinct.
  - configured scenes must exist.
  - source scene must not contain the playback source.
  - delay scene must contain the `Comp Delay Playback` source.
  - estimated encoded bitrate must fit the default `600 MB` cap.
  - the configured H.264/AAC encoder names must be available in the bundled FFmpeg runtime.
  - capture/encoder failure and encoded-buffer underrun clear playback, switch to transition, and surface the error in the settings dialog.
  - delayed playback is not entered until the buffer has both target depth and a decodable video keyframe for the target timestamp.
- Frontend save/load callback for scene collection settings.
- Frontend hotkeys:
  - `Comp Delay: Apply configured delay`
  - `Comp Delay: Go live / 0 delay`
  - preset delay hotkeys for `30s`, `60s`, and `300s`.
  - hotkey requests are queued onto the Qt runtime timer before they touch OBS frontend scene state, so websocket-triggered hotkeys do not block inside OBS scene switching.
- OBS-independent unit tests for delay clamp, state machine, ringbuffer eviction, and FFmpeg H.264/AAC encode/decode.
- Encoded A/V ringbuffer primitives:
  - time-indexed packet retention.
  - timestamp-sorted insertion for interleaved OBS audio/video callbacks.
  - keyframe lookup at or before the target delay timestamp.
  - decode batch selection from the correct video keyframe.
  - explicit packet dropping for delay reduction.
- A real encoded scene capture/playback path:
  - video from the selected source scene is rendered offscreen with OBS graphics APIs (`gs_texrender` + staging surface), copied as a transient RGBA frame, encoded with the selected H.264 encoder, and stored as packets in the shared ringbuffer.
  - source-scene audio is mixed by an internal OBS aux view, copied from the scene mix buffer on OBS audio cadence, accumulated to the selected AAC encoder's frame size, encoded, and stored as packets in the same ringbuffer.
  - the playback source incrementally decodes due H.264/AAC packets and emits OBS async video plus synced source audio.
  - the encoded ringbuffer keeps GOP preroll beyond the user-visible target delay, so `300s` delay remains decodable at the maximum setting.
- Active delay retargeting:
  - changing only the delay value while capture is active reuses the existing encoded buffer.
  - increasing delay switches to the transition scene until enough extra encoded depth is available.
  - decreasing delay trims skipped packets back to the nearest safe video keyframe, briefly shows the transition scene, then resumes delayed playback.
- Settings status includes target delay, current buffer depth, and fill countdown while waiting.

## Validation status

The core implementation builds, its unit/codec tests pass locally, and OBS portable integration tests pass on Windows:

- `0 -> 3s` with synthetic A/V media.
- `0 -> 60s` with synthetic A/V media; transition scene stayed live until delay scene at `60.50s`.
- `0 -> 120s` with synthetic A/V media.
- `0 -> 300s` with synthetic A/V media; transition scene stayed live until delay scene at `300.56s`, with playback A/V sync base delta logged at `25.71 ms`.
- `3s -> 30s` retarget increase with synthetic A/V media.
- `60s -> 30s` retarget decrease with synthetic A/V media; transition scene was visible briefly and delay scene was reached again at `1.25s`.
- `60s -> 300s` retarget increase with synthetic A/V media; transition scene stayed live for the additional fill and delay scene was reached again after `240.15s`, with playback A/V sync base delta logged at `-22.13 ms`.
- `300s -> 0` via `Go live`, verified by the integration script returning to the source scene after the 300s test.
- active streaming output against a local RTMP listener; stream stayed active through initial delay, retarget, and `Go live`.
- A/V sync telemetry verification; the integration test fails if playback audio/video base delta exceeds the configured tolerance.
- runtime encoded-buffer underrun; the integration test reaches delayed playback, forces a tight encoded byte cap, verifies fallback to the transition scene, and checks the logged `Buffer underrun` runtime failure.
- guardrail rejection without starting capture for:
  - duplicate source/transition scene.
  - missing configured source scene.
  - delay scene missing the playback source.
  - source scene containing the playback source.
  - unavailable video encoder.
  - unavailable audio encoder.
  - memory cap too small for configured retention.

Remaining manual release checks:

- confirm a production streaming service behaves like the local RTMP streaming-output smoke test.
- confirm selected hardware encoders and production-like sources under the operator's actual OBS profile.

## OBS setup

1. Create a normal source scene, for example `Game`.
2. Create a transition scene, for example `Delay Transition`.
3. Create a delay scene, for example `Delayed Program`.
4. Add a `Comp Delay Playback` source to the delay scene.
5. Open `Tools` -> `Comp Delay Settings`, select all three scenes, choose encoders, and set a delay from `0` to `300` seconds.

## Build

This repository uses the OBS plugin template presets.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
```

## Repository layout

The repository is intentionally Windows-first for v1:

- `.github/workflows/push.yaml`: builds and publishes the Windows setup artifact.
- `cmake/common` and `cmake/windows`: minimal CMake bootstrap needed to fetch OBS/Qt deps and build on Windows.
- `installer/windows`: Inno Setup script for the user-facing installer.
- `src`, `data`, and `tests`: plugin source, OBS plugin data, and verification tests.

Generated folders such as `build_x64`, `.deps`, and `release` are local build outputs and are not committed.

## Windows installer

GitHub Actions builds a normal Windows setup executable on every push to `main`.

- Main branch builds upload `obs-comp-delay-windows-x64-setup` as a workflow artifact.
- Version tags such as `v0.1.0` or `0.1.0` also create a draft GitHub Release with the setup executable attached.
- The installer targets a standard OBS Studio install directory and installs:
  - `obs-comp-delay.dll` into `obs-plugins\64bit`.
  - plugin data and locale files into `data\obs-plugins\obs-comp-delay`.

Build the installer locally after `cmake --build --preset windows-x64` with:

```powershell
$buildSpec = Get-Content -Path buildspec.json -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path release | Out-Null
iscc "/DMyAppVersion=$($buildSpec.version)" `
  "/DBuildDir=$((Resolve-Path 'build_x64\RelWithDebInfo').Path)" `
  "/DRundir=$((Resolve-Path 'build_x64\rundir\RelWithDebInfo').Path)" `
  "/DOutputDir=$((Resolve-Path 'release').Path)" `
  "installer\windows\obs-comp-delay.iss"
```

Current local verification:

- CMake is installed at `C:\Program Files\CMake\bin\cmake.exe`.
- Visual Studio Build Tools 2022 are installed and usable by the `windows-x64` preset.
- Windows SDK `10.0.22621.0` is installed and selected by CMake.
- OBS prebuilt deps, Qt6 deps, and OBS `31.1.1` sources have been downloaded into `.deps`.
- Full configure passes with `cmake --preset windows-x64`.
- Full build passes with `cmake --build --preset windows-x64`.
- The plugin DLL is produced at `build_x64\RelWithDebInfo\obs-comp-delay.dll`.
- CTest passes with `ctest --test-dir build_x64 -C RelWithDebInfo --output-on-failure`.
- OBS portable 31.1.1 smoke test passes: after copying the plugin into `.deps\obs-portable-31.1.1`, the OBS log reports `[obs-comp-delay] plugin loaded successfully` and lists `obs-comp-delay.dll` under loaded modules.
- OBS portable 31.1.1 integration smoke test passes with:

```powershell
powershell -ExecutionPolicy Bypass -File tests\obs-portable-integration-smoke.ps1 -DelaySeconds 3
```

  The test creates source/transition/delay scenes through obs-websocket, adds `Comp Delay Playback`, triggers `obs_comp_delay.apply_configured_delay`, verifies transition scene is shown during fill, verifies the delay scene is reached after the configured delay, and triggers `obs_comp_delay.go_live`.
- A/V and long-delay variants pass with:

```powershell
powershell -ExecutionPolicy Bypass -File tests\obs-portable-integration-smoke.ps1 -DelaySeconds 300 -UseMediaSource
powershell -ExecutionPolicy Bypass -File tests\obs-portable-integration-smoke.ps1 -DelaySeconds 60 -RetargetPresetSeconds 300 -UseMediaSource -VerifyAvSync
powershell -ExecutionPolicy Bypass -File tests\obs-portable-integration-smoke.ps1 -DelaySeconds 3 -RetargetPresetSeconds 30 -UseMediaSource -VerifyStreaming
powershell -ExecutionPolicy Bypass -File tests\obs-portable-integration-smoke.ps1 -DelaySeconds 3 -UseMediaSource -VerifyAvSync
powershell -ExecutionPolicy Bypass -File tests\obs-portable-integration-smoke.ps1 -DelaySeconds 2 -Scenario RuntimeUnderrun -UseMediaSource
```

- Guardrail variants pass with:

```powershell
powershell -ExecutionPolicy Bypass -File tests\obs-portable-integration-smoke.ps1 -DelaySeconds 3 -Scenario SameScene
powershell -ExecutionPolicy Bypass -File tests\obs-portable-integration-smoke.ps1 -DelaySeconds 3 -Scenario MissingSourceScene
powershell -ExecutionPolicy Bypass -File tests\obs-portable-integration-smoke.ps1 -DelaySeconds 3 -Scenario MissingDelayPlayback
powershell -ExecutionPolicy Bypass -File tests\obs-portable-integration-smoke.ps1 -DelaySeconds 3 -Scenario SourceContainsPlayback
powershell -ExecutionPolicy Bypass -File tests\obs-portable-integration-smoke.ps1 -DelaySeconds 3 -Scenario InvalidVideoEncoder
powershell -ExecutionPolicy Bypass -File tests\obs-portable-integration-smoke.ps1 -DelaySeconds 3 -Scenario InvalidAudioEncoder
powershell -ExecutionPolicy Bypass -File tests\obs-portable-integration-smoke.ps1 -DelaySeconds 3 -Scenario MemoryCap
```

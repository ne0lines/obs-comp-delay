param(
    [string]$PortableObsPath = ".deps\obs-portable-31.1.1",
    [int]$DelaySeconds = 3,
    [ValidateSet("Normal", "SameScene", "MissingSourceScene", "MissingDelayPlayback", "SourceContainsPlayback", "InvalidVideoEncoder", "InvalidAudioEncoder", "MemoryCap", "RuntimeUnderrun")]
    [string]$Scenario = "Normal",
    [switch]$UseMediaSource,
    [switch]$VerifyStreaming,
    [switch]$VerifyAvSync,
    [double]$AvSyncToleranceMs = 50.0,
    [int]$RtmpPort = 19350,
    [int]$WebSocketPort = 4455,
    [string]$MediaFile = ".deps\obs-comp-delay-av-smoke.mp4"
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$portable = Resolve-Path (Join-Path $repoRoot $PortableObsPath)
$scenePath = Join-Path $portable "config\obs-studio\basic\scenes\Untitled.json"
$websocketConfigPath = Join-Path $portable "config\obs-studio\plugin_config\obs-websocket\config.json"
$pluginBin = Join-Path $portable "obs-plugins\64bit"
$pluginData = Join-Path $portable "data\obs-plugins\obs-comp-delay"
$obsExe = Join-Path $portable "bin\64bit\obs64.exe"
$obsWorkDir = Split-Path $obsExe
$resolvedMediaFile = Join-Path $repoRoot $MediaFile
$rtmpLog = Join-Path $repoRoot ".deps\obs-comp-delay-rtmp-listener.log"
$rtmpOut = Join-Path $repoRoot ".deps\obs-comp-delay-rtmp-listener.out"
$rtmpProc = $null

if (-not (Test-Path $obsExe)) {
    throw "OBS portable executable not found at $obsExe"
}

if (($UseMediaSource -or $VerifyStreaming) -and -not (Test-Path $resolvedMediaFile)) {
    $ffmpegExe = Join-Path $repoRoot ".deps\obs-deps-2025-07-11-x64\bin\ffmpeg.exe"
    if (-not (Test-Path $ffmpegExe)) {
        throw "FFmpeg executable not found at $ffmpegExe"
    }

    New-Item -ItemType Directory -Path (Split-Path $resolvedMediaFile) -Force | Out-Null
    & $ffmpegExe -y `
        -f lavfi -i "testsrc2=size=1280x720:rate=30" `
        -f lavfi -i "sine=frequency=1000:sample_rate=48000" `
        -t 20 `
        -pix_fmt yuv420p `
        -c:v libx264 -preset veryfast -g 30 `
        -c:a aac -b:a 128k `
        $resolvedMediaFile
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to generate A/V smoke media at $resolvedMediaFile"
    }
}

if ($VerifyStreaming) {
    $ffmpegExe = Join-Path $repoRoot ".deps\obs-deps-2025-07-11-x64\bin\ffmpeg.exe"
    if (-not (Test-Path $ffmpegExe)) {
        throw "FFmpeg executable not found at $ffmpegExe"
    }

    Remove-Item -LiteralPath $rtmpLog -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $rtmpOut -Force -ErrorAction SilentlyContinue
    $rtmpProc = Start-Process -FilePath $ffmpegExe -ArgumentList @(
        "-hide_banner",
        "-loglevel", "info",
        "-listen", "1",
        "-i", "rtmp://127.0.0.1:$RtmpPort/live/test",
        "-f", "null",
        "-"
    ) -RedirectStandardError $rtmpLog -RedirectStandardOutput $rtmpOut -WindowStyle Hidden -PassThru
}

$runId = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds().ToString()
$sourceScene = "CD Source Smoke $runId"
$transitionScene = "CD Transition Smoke $runId"
$delayScene = "CD Delay Smoke $runId"
$configuredSourceScene = if ($Scenario -eq "MissingSourceScene") { "CD Missing Source Smoke $runId" } else { $sourceScene }
$configuredTransitionScene = if ($Scenario -eq "SameScene") { $sourceScene } else { $transitionScene }
$configuredDelayScene = $delayScene
$configuredVideoEncoder = if ($Scenario -eq "InvalidVideoEncoder") { "definitely_missing_h264_encoder" } else { "libx264" }
$configuredAudioEncoder = if ($Scenario -eq "InvalidAudioEncoder") { "definitely_missing_aac_encoder" } else { "aac" }
$configuredVideoBitrate = if ($Scenario -eq "RuntimeUnderrun") { 16 } else { 1500 }
$configuredAudioBitrate = if ($Scenario -eq "RuntimeUnderrun") { 16 } else { 128 }
$configuredMemoryCap = if ($Scenario -eq "MemoryCap") {
    1024
} elseif ($Scenario -eq "RuntimeUnderrun") {
    28000
} else {
    629145600
}
$colorInput = "CD Color Smoke $runId"
$mediaInput = "CD Media Smoke $runId"
$countdownInput = "CD Countdown Smoke $runId"
$sourcePlaybackInput = "Comp Delay Playback Recursive Smoke $runId"
$delayPlaybackInput = "Comp Delay Playback Smoke $runId"

$websocketConfig = Get-Content $websocketConfigPath -Raw | ConvertFrom-Json
$websocketConfig.server_enabled = $true
$websocketConfig.auth_required = $false
$websocketConfig.alerts_enabled = $false
$websocketConfig.server_port = $WebSocketPort
$websocketConfig | ConvertTo-Json -Depth 20 | Set-Content $websocketConfigPath -Encoding UTF8

$sceneJson = Get-Content $scenePath -Raw | ConvertFrom-Json
if (-not $sceneJson.modules) {
    $sceneJson | Add-Member -MemberType NoteProperty -Name modules -Value ([pscustomobject]@{})
}
$sceneJson.modules.comp_delay = [pscustomobject]@{
    source_scene = $configuredSourceScene
    transition_scene = $configuredTransitionScene
    delay_scene = $configuredDelayScene
    video_encoder = $configuredVideoEncoder
    audio_encoder = $configuredAudioEncoder
    target_delay_seconds = $DelaySeconds
    video_bitrate_kbps = $configuredVideoBitrate
    audio_bitrate_kbps = $configuredAudioBitrate
    keyframe_interval_seconds = 1
    memory_cap_bytes = $configuredMemoryCap
    apply_hotkey = @()
    go_live_hotkey = @()
}
$sceneJson | ConvertTo-Json -Depth 100 | Set-Content $scenePath -Encoding UTF8

New-Item -ItemType Directory -Path $pluginBin -Force | Out-Null
New-Item -ItemType Directory -Path $pluginData -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot "build_x64\RelWithDebInfo\obs-comp-delay.dll") -Destination (Join-Path $pluginBin "obs-comp-delay.dll") -Force
Copy-Item -Path (Join-Path $repoRoot "build_x64\rundir\RelWithDebInfo\obs-comp-delay\*") -Destination $pluginData -Recurse -Force

$env:CD_SOURCE_SCENE = $sourceScene
$env:CD_TRANSITION_SCENE = $transitionScene
$env:CD_DELAY_SCENE = $delayScene
$env:CD_DELAY_SECONDS = [string]$DelaySeconds
$env:CD_SCENARIO = $Scenario
$env:CD_USE_MEDIA_SOURCE = if ($UseMediaSource) { "1" } else { "0" }
$env:CD_MEDIA_FILE = $resolvedMediaFile
$env:CD_VERIFY_STREAMING = if ($VerifyStreaming) { "1" } else { "0" }
$env:CD_VERIFY_AV_SYNC = if ($VerifyAvSync) { "1" } else { "0" }
$env:CD_AV_SYNC_TOLERANCE_MS = [string]$AvSyncToleranceMs
$env:CD_RTMP_PORT = [string]$RtmpPort
$env:CD_WEBSOCKET_PORT = [string]$WebSocketPort
$env:CD_COLOR_INPUT = $colorInput
$env:CD_MEDIA_INPUT = $mediaInput
$env:CD_COUNTDOWN_INPUT = $countdownInput
$env:CD_SOURCE_PLAYBACK_INPUT = $sourcePlaybackInput
$env:CD_DELAY_PLAYBACK_INPUT = $delayPlaybackInput

$proc = Start-Process -FilePath $obsExe -WorkingDirectory $obsWorkDir -ArgumentList @(
    "--portable",
    "--disable-updater",
    "--disable-shutdown-check",
    "--disable-missing-files-check",
    "--minimize-to-tray"
) -WindowStyle Hidden -PassThru

try {
    Start-Sleep -Seconds 8

    $nodeScript = @'
const sourceScene = process.env.CD_SOURCE_SCENE;
const transitionScene = process.env.CD_TRANSITION_SCENE;
const delayScene = process.env.CD_DELAY_SCENE;
const delaySeconds = Number(process.env.CD_DELAY_SECONDS || '3');
const scenario = process.env.CD_SCENARIO || 'Normal';
const useMediaSource = process.env.CD_USE_MEDIA_SOURCE === '1';
const verifyStreaming = process.env.CD_VERIFY_STREAMING === '1';
const rtmpPort = Number(process.env.CD_RTMP_PORT || '19350');
const websocketPort = Number(process.env.CD_WEBSOCKET_PORT || '4455');
const mediaFile = process.env.CD_MEDIA_FILE;
const colorInput = process.env.CD_COLOR_INPUT;
const mediaInput = process.env.CD_MEDIA_INPUT;
const countdownInput = process.env.CD_COUNTDOWN_INPUT;
const sourcePlaybackInput = process.env.CD_SOURCE_PLAYBACK_INPUT;
const delayPlaybackInput = process.env.CD_DELAY_PLAYBACK_INPUT;
const ws = new WebSocket(`ws://127.0.0.1:${websocketPort}`);
let nextId = 1;
const pending = new Map();
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

function send(payload) {
  ws.send(JSON.stringify(payload));
}

function request(requestType, requestData = {}) {
  const requestId = String(nextId++);
  send({op: 6, d: {requestType, requestId, requestData}});
  return new Promise((resolve, reject) => {
    pending.set(requestId, {resolve, reject});
    setTimeout(() => {
      if (pending.delete(requestId)) reject(new Error(`timeout ${requestType}`));
    }, 8000);
  });
}

async function triggerHotkeyByName(hotkeyName) {
  try {
    await request('TriggerHotkeyByName', {hotkeyName});
  } catch (error) {
    if (!String(error.message).includes('timeout TriggerHotkeyByName')) {
      throw error;
    }
  }
}

async function createScene(sceneName) {
  try {
    await request('CreateScene', {sceneName});
  } catch (error) {
    if (!String(error.message).includes('already exists')) throw error;
  }
}

async function createInput(sceneName, inputName, inputKind, inputSettings = {}) {
  try {
    await request('CreateInput', {sceneName, inputName, inputKind, inputSettings, sceneItemEnabled: true});
  } catch (error) {
    if (!String(error.message).includes('already exists')) throw error;
  }
}

async function createCountdownTextInput() {
  const settings = {
    text: 'Delay starts in %delay_countdown%',
    font: {face: 'Arial', size: 72, style: 'Regular'},
    color: 4294967295,
  };
  const candidates = ['text_gdiplus_v3', 'text_gdiplus_v2', 'text_gdiplus', 'text_ft2_source_v2', 'text_ft2_source'];
  const errors = [];
  for (const inputKind of candidates) {
    try {
      await createInput(transitionScene, countdownInput, inputKind, settings);
      await request('SetInputSettings', {inputName: countdownInput, inputSettings: settings, overlay: false});
      return inputKind;
    } catch (error) {
      errors.push(`${inputKind}: ${error.message}`);
    }
  }
  throw new Error(`could not create countdown text source: ${errors.join('; ')}`);
}

async function getCountdownText() {
  const settings = await request('GetInputSettings', {inputName: countdownInput});
  const text = settings?.inputSettings?.text;
  return typeof text === 'string' ? text : null;
}

async function ensureMediaInput(sceneName) {
  const settings = {
    is_local_file: true,
    local_file: mediaFile,
    looping: true,
    restart_on_activate: true,
    close_when_inactive: false,
    clear_on_media_end: false
  };
  await createInput(sceneName, mediaInput, 'ffmpeg_source', settings);
  await request('SetInputSettings', {inputName: mediaInput, inputSettings: settings, overlay: false});
}

async function waitForDelayScene(expectedDelaySeconds) {
  const timeline = [];
  const countdownSamples = [];
  let reachedDelay = false;
  const start = Date.now();
  const timeoutMs = Math.max(10000, (expectedDelaySeconds + 20) * 1000);
  while (Date.now() - start < timeoutMs) {
    await sleep(250);
    const current = await request('GetCurrentProgramScene');
    if (countdownInput) {
      const settings = await request('GetInputSettings', {inputName: countdownInput}).catch(() => null);
      const text = settings?.inputSettings?.text;
      if (typeof text === 'string') {
        countdownSamples.push({
          t: Number(((Date.now() - start) / 1000).toFixed(2)),
          text,
        });
      }
    }
    timeline.push({t: Number(((Date.now() - start) / 1000).toFixed(2)), scene: current.currentProgramSceneName});
    if (current.currentProgramSceneName === delayScene) {
      reachedDelay = true;
      break;
    }
  }

  return {
    transitionObserved: timeline.some(item => item.scene === transitionScene),
    reachedDelay,
    reachedAt: reachedDelay && timeline.length ? timeline[timeline.length - 1].t : null,
    samples: timeline.length,
    firstSamples: timeline.slice(0, 5),
    lastSamples: timeline.slice(-5),
    countdownUpdated: countdownSamples.some(item => /^Delay starts in \d+$/.test(item.text)),
    countdownSamples: countdownSamples.slice(0, 8)
  };
}

async function waitForScene(sceneName, timeoutMs = 5000) {
  const start = Date.now();
  let current = null;
  while (Date.now() - start < timeoutMs) {
    await sleep(250);
    current = await request('GetCurrentProgramScene');
    if (current.currentProgramSceneName === sceneName) {
      return current;
    }
  }
  return current || await request('GetCurrentProgramScene');
}

async function waitForSceneAfter(sceneName, timeoutMs = 10000) {
  const timeline = [];
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    await sleep(250);
    const current = await request('GetCurrentProgramScene');
    timeline.push({t: Number(((Date.now() - start) / 1000).toFixed(2)), scene: current.currentProgramSceneName});
    if (current.currentProgramSceneName === sceneName) {
      return {reached: true, reachedAt: timeline[timeline.length - 1].t, samples: timeline.length, timeline: timeline.slice(-20)};
    }
  }
  return {reached: false, reachedAt: null, samples: timeline.length, timeline: timeline.slice(-20)};
}

async function waitForStreamActive() {
  const samples = [];
  for (let i = 0; i < 40; i++) {
    await sleep(500);
    const status = await request('GetStreamStatus');
    samples.push(status);
    if (status.outputActive) {
      return {active: true, status, samples: samples.slice(-5)};
    }
  }
  return {active: false, status: samples.at(-1) || null, samples: samples.slice(-5)};
}

async function assertStreamStillActive(label, previousBytes = 0) {
  const status = await request('GetStreamStatus');
  const result = {
    label,
    outputActive: Boolean(status.outputActive),
    outputBytes: Number(status.outputBytes || 0),
    outputDuration: Number(status.outputDuration || 0),
    outputSkippedFrames: Number(status.outputSkippedFrames || 0)
  };
  if (!result.outputActive || result.outputBytes < previousBytes) {
    throw new Error(`stream inactive during ${label}: ${JSON.stringify(result)}`);
  }
  return result;
}

function identify() {
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => reject(new Error('identify timeout')), 10000);
    ws.onmessage = event => {
      const message = JSON.parse(event.data);
      if (message.op === 0) {
        send({op: 1, d: {rpcVersion: 1}});
      } else if (message.op === 2) {
        clearTimeout(timeout);
        resolve();
      } else if (message.op === 7) {
        const item = pending.get(message.d.requestId);
        if (!item) return;
        pending.delete(message.d.requestId);
        if (message.d.requestStatus?.result) {
          item.resolve(message.d.responseData || {});
        } else {
          item.reject(new Error(`${message.d.requestType} failed: ${message.d.requestStatus?.comment || message.d.requestStatus?.code}`));
        }
      }
    };
    ws.onerror = error => reject(new Error(error.message || 'websocket error'));
  });
}

async function main() {
  const expectRuntimeUnderrun = scenario === 'RuntimeUnderrun';
  const guardrailMode = scenario !== 'Normal' && !expectRuntimeUnderrun;
  await identify();
  await createScene(sourceScene);
  await createScene(transitionScene);
  await createScene(delayScene);
  await createInput(sourceScene, colorInput, 'color_source_v3', {color: 4278190335, width: 1280, height: 720});
  const countdownInputKind = await createCountdownTextInput();
  if (useMediaSource) {
    await ensureMediaInput(sourceScene);
  }
  if (scenario === 'SourceContainsPlayback') {
    await createInput(sourceScene, sourcePlaybackInput, 'comp_delay_playback_source', {});
  }
  if (scenario !== 'MissingDelayPlayback') {
    await createInput(delayScene, delayPlaybackInput, 'comp_delay_playback_source', {});
  }
  await request('SetCurrentProgramScene', {sceneName: sourceScene});
  let stream = null;
  if (verifyStreaming) {
    await request('SetStreamServiceSettings', {
      streamServiceType: 'rtmp_custom',
      streamServiceSettings: {
        server: `rtmp://127.0.0.1:${rtmpPort}/live`,
        key: 'test',
        use_auth: false
      }
    });
    await request('StartStream');
    const started = await waitForStreamActive();
    if (!started.active) {
      throw new Error(`stream did not become active: ${JSON.stringify(started)}`);
    }
    stream = {
      started: {
        outputActive: started.status.outputActive,
        outputBytes: Number(started.status.outputBytes || 0),
        outputDuration: Number(started.status.outputDuration || 0)
      },
      checks: []
    };
  }

  await triggerHotkeyByName('obs_comp_delay.apply_configured_delay');

  if (guardrailMode) {
    await sleep(2500);
    const current = await request('GetCurrentProgramScene');
    if (verifyStreaming) {
      stream.checks.push(await assertStreamStillActive('after rejected guardrail apply', stream.started.outputBytes));
      await request('StopStream').catch(() => {});
    }
    const result = {
      scenario,
      guardrailRejected: current.currentProgramSceneName !== delayScene,
      currentScene: current.currentProgramSceneName,
      stream
    };
    console.log(JSON.stringify(result, null, 2));
    if (!result.guardrailRejected) {
      process.exitCode = 3;
    }
    ws.close();
    return;
  }

  const initial = await waitForDelayScene(delaySeconds);
  const countdownAfterDelayScene = await getCountdownText();
  await sleep(5500);
  const countdownAfterRestoreDelay = await getCountdownText();
  if (verifyStreaming) {
    stream.checks.push(await assertStreamStillActive('after initial delay', stream.started.outputBytes));
  }

  if (expectRuntimeUnderrun) {
    const runtimeUnderrun = initial.reachedDelay
      ? await waitForSceneAfter(transitionScene, 15000)
      : {reached: false, reachedAt: null, samples: 0, timeline: []};
    await triggerHotkeyByName('obs_comp_delay.go_live').catch(() => {});
    const afterGoLive = await waitForScene(sourceScene);
    const result = {
      scenario,
      transitionObserved: initial.transitionObserved,
      reachedDelay: initial.reachedDelay,
      initial,
      runtimeUnderrun,
      stream,
      afterGoLiveScene: afterGoLive.currentProgramSceneName,
    };
    console.log(JSON.stringify(result, null, 2));
    if (!result.transitionObserved || !result.reachedDelay || !runtimeUnderrun.reached || result.afterGoLiveScene !== sourceScene) {
      process.exitCode = 3;
    }
    ws.close();
    return;
  }

  await triggerHotkeyByName('obs_comp_delay.deactivate_after_delay').catch(() => {});
  const afterGoLive = await waitForScene(sourceScene, Math.max(5000, (delaySeconds + 5) * 1000));
  if (verifyStreaming) {
    stream.checks.push(await assertStreamStillActive('after go live', stream.checks.at(-1)?.outputBytes || 0));
    await request('StopStream').catch(() => {});
  }
  const result = {
    transitionObserved: initial.transitionObserved,
    reachedDelay: initial.reachedDelay,
    countdownUpdated: initial.countdownUpdated,
    countdownBlankedAfterDelayScene: typeof countdownAfterDelayScene === 'string' && countdownAfterDelayScene.trim() === '',
    countdownRestoredAfterDelay: countdownAfterRestoreDelay === 'Delay starts in %delay_countdown%',
    countdownInputKind,
    initial,
    countdownAfterDelayScene,
    countdownAfterRestoreDelay,
    stream,
    afterGoLiveScene: afterGoLive.currentProgramSceneName,
  };
  console.log(JSON.stringify(result, null, 2));
  if (!result.transitionObserved || !result.reachedDelay || !result.countdownUpdated ||
      !result.countdownBlankedAfterDelayScene || !result.countdownRestoredAfterDelay ||
      result.afterGoLiveScene !== sourceScene) {
    process.exitCode = 3;
  }
  ws.close();
}

main().then(() => process.exit(process.exitCode || 0)).catch(error => {
  console.error(error.stack || error.message);
  try { ws.close(); } catch {}
  process.exit(1);
});
'@

    $nodeScript | node -
    if ($LASTEXITCODE -ne 0) {
        throw "OBS websocket smoke failed with exit code $LASTEXITCODE"
    }

    $latestLog = Get-ChildItem -Path (Join-Path $portable "config\obs-studio\logs") -Filter "*.txt" |
        Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
    Write-Host "LOG=$($latestLog.FullName)"
    $pluginLogLines = @(
        Select-String -Path $latestLog.FullName -Pattern "\[obs-comp-delay\]|Failed to load.*obs-comp-delay|Failed to initialize module.*obs-comp-delay" |
            ForEach-Object { $_.Line }
    )
    if ($pluginLogLines.Count -le 35) {
        $pluginLogLines
    } else {
        $pluginLogLines | Select-Object -First 15
        Write-Host "... $($pluginLogLines.Count) matching log lines total ..."
        $pluginLogLines | Select-Object -Last 20
    }

    $guardrailScenario = $Scenario -notin @("Normal", "RuntimeUnderrun")
    if ($guardrailScenario) {
        $captureStarted = Select-String -Path $latestLog.FullName -Pattern "\[obs-comp-delay\] capture started"
        if ($captureStarted) {
            throw "Guardrail scenario '$Scenario' unexpectedly started capture. See $($latestLog.FullName)"
        }
    }

    if ($Scenario -eq "RuntimeUnderrun") {
        $runtimeUnderrun = Select-String -Path $latestLog.FullName -Pattern "\[obs-comp-delay\] runtime failure: Buffer underrun"
        if (-not $runtimeUnderrun) {
            throw "Runtime underrun scenario did not log a buffer underrun. See $($latestLog.FullName)"
        }
    }

    if ($UseMediaSource -and $Scenario -in @("Normal", "RuntimeUnderrun")) {
        $audioPacketLines = Select-String -Path $latestLog.FullName -Pattern "\[obs-comp-delay\] buffer depth .*audio_packets=([1-9][0-9]*)"
        if (-not $audioPacketLines) {
            throw "Media-source smoke did not encode audio packets. See $($latestLog.FullName)"
        }
    }

    if ($VerifyAvSync -and $Scenario -eq "Normal") {
        $syncLine = Select-String -Path $latestLog.FullName -Pattern "\[obs-comp-delay\] playback A/V sync base: .*delta=([-0-9.]+) ms" |
            Select-Object -Last 1
        if (-not $syncLine) {
            throw "A/V sync verification did not find playback sync telemetry. See $($latestLog.FullName)"
        }

        $match = [regex]::Match($syncLine.Line, "delta=([-0-9.]+) ms")
        if (-not $match.Success) {
            throw "Could not parse A/V sync delta from: $($syncLine.Line)"
        }

        $deltaMs = [double]$match.Groups[1].Value
        if ([Math]::Abs($deltaMs) -gt $AvSyncToleranceMs) {
            throw "A/V sync delta $deltaMs ms exceeds tolerance $AvSyncToleranceMs ms. See $($latestLog.FullName)"
        }
    }
} finally {
    if (-not $proc.HasExited) {
        $proc.CloseMainWindow() | Out-Null
        Start-Sleep -Seconds 3
        if (-not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force
        }
    }
    if ($rtmpProc -and -not $rtmpProc.HasExited) {
        Stop-Process -Id $rtmpProc.Id -Force
    }
}

# OBS Comp Delay

OBS Comp Delay is a Windows-first OBS Studio plugin for adding or removing a stream delay while OBS keeps streaming.

The plugin delays one selected source scene. When you activate delay, OBS immediately switches to your transition scene while the internal buffer fills. When the configured delay time has passed, OBS switches to your delay scene, where the `Comp Delay Playback` source plays the delayed source scene.

## What It Does

- Adds up to `300` seconds of delay without stopping the OBS streaming output.
- Uses the delay value configured in `Tools` -> `Comp Delay Settings`.
- Adds an `Activate delay` / `Deactivate delay` button to OBS' `Controls` dock.
- Provides hotkeys for `Comp Delay: Activate delay` and `Comp Delay: Deactivate delay`.
- Lets you choose the video and audio encoders used by the delay buffer.
- Supports a transition-scene countdown text token: `%delay_countdown%`.

## Install

1. Open the repository's GitHub Releases page.
2. Download the latest Windows setup file:
   `obs-comp-delay-<version>-windows-x64-setup.exe`
3. Close OBS Studio.
4. Run the setup file.
5. Start OBS Studio again.

The installer places the plugin DLL and plugin data in the normal OBS Studio plugin folders.

## OBS Setup

Create three scenes:

1. `Source scene`
   This is the scene you want viewers to see with delay, for example your game or program feed.

2. `Transition scene`
   This is shown live while the delay buffer fills. Use it for a waiting screen, break screen, sponsor screen, or countdown.

3. `Delay scene`
   This scene must contain a `Comp Delay Playback` source. This is what plays the delayed version of your source scene.

Then open `Tools` -> `Comp Delay Settings` and select:

- `Source scene`
- `Transition scene`
- `Delay scene`
- Video encoder
- Audio encoder
- Delay in seconds

## How To Use

1. Set the wanted delay in `Comp Delay Settings`.
2. Click `Activate delay` in OBS' `Controls` dock, the settings dialog, or your assigned hotkey.
3. OBS switches to the transition scene immediately.
4. When the configured delay time has passed, OBS switches to the delay scene automatically.
5. Click `Deactivate delay` to drop the buffer and return to the source scene live.

Changing the delay while it is already active uses the same configured delay workflow. The plugin shows the transition scene while it adjusts the buffer, then returns to delayed playback.

## Countdown Text

You can add a countdown to the transition scene with a normal OBS text source.

Example text:

```text
Delay starts in %delay_countdown%
```

While the transition scene is active, the plugin replaces `%delay_countdown%` with the remaining buffer-fill time in whole seconds. When the transition is done, the original text template is restored.

## Audio

Any audio that should be delayed must be part of, or routed into, the selected source scene. The plugin delays the selected scene, not every arbitrary OBS audio source in the profile.

## FAQ

**Does this stop or restart my stream?**

No. OBS streaming output stays active while the plugin changes scenes and fills the delay buffer.

**Why do I need a delay scene?**

OBS needs a scene that contains the `Comp Delay Playback` source. That source is responsible for showing and playing the delayed buffer.

**Why do I need a transition scene?**

The transition scene gives viewers something live to see while the delay buffer is filling. Without it, viewers would see an awkward jump or incomplete delay setup.

**Can I use different fixed buttons like 30s, 60s, and 300s?**

No. The plugin uses the delay value from `Comp Delay Settings`. This keeps operation simple and avoids activating the wrong delay during a live production.

**Can I change the delay while it is active?**

Yes. Change the value in `Comp Delay Settings` and activate/apply it again. The plugin will use the transition scene while it adjusts.

**What happens if the buffer fails or a scene is missing?**

The plugin falls back to the transition scene and shows the error in `Comp Delay Settings`.

**Does `%delay_countdown%` work in every source type?**

It is intended for OBS text sources in the transition scene.

**Is this cross-platform?**

V1 is Windows-first.

## Developer Notes

Build locally on Windows:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
ctest --test-dir build_x64 -C RelWithDebInfo --output-on-failure
```

Run the portable OBS smoke test:

```powershell
powershell -ExecutionPolicy Bypass -File tests\obs-portable-integration-smoke.ps1 -DelaySeconds 3
```

If another OBS instance already uses obs-websocket port `4455`, run the smoke test on another port:

```powershell
powershell -ExecutionPolicy Bypass -File tests\obs-portable-integration-smoke.ps1 -DelaySeconds 3 -WebSocketPort 4456
```

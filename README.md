# OBS Comp Delay

OBS Comp Delay is a Windows-first OBS Studio plugin for adding or removing a stream delay while OBS keeps streaming.

The plugin delays one selected source scene. When you activate delay, OBS immediately switches to your `Delay Transition Scene` while the internal buffer fills. When the configured delay time has passed, OBS switches to your delay scene, where the `Comp Delay Playback` source plays the delayed source scene.

## What It Does

- Adds up to `300` seconds of delay without stopping the OBS streaming output.
- Uses the delay value configured in `Tools` -> `Comp Delay Settings`.
- Adds an `Activate delay` / `Deactivate delay` button to OBS' `Controls` dock.
- Shows the Controls button in green when delay is inactive and red when delay is active.
- Provides hotkeys for `Comp Delay: Activate delay` and `Comp Delay: Deactivate delay`.
- Lets you choose the video and audio encoders used by the delay buffer.
- Supports a `Delay Transition Scene` countdown text token: `%delay_countdown%`.

## Install

1. Open the repository's GitHub Releases page.
2. Download the latest Windows setup file:
   `obs-comp-delay-<version>-windows-x64-setup.exe`
3. Close OBS Studio.
4. Run the setup file.
5. Start OBS Studio again.

The installer places the plugin DLL and plugin data in the normal OBS Studio plugin folders.

## OBS Setup

You normally already have a source scene, for example your game or program feed. Keep using that as `Source scene`.

Create two additional scenes:

1. `Delay Transition Scene`
   This is shown live while the delay buffer fills. Use it for a waiting screen, break screen, sponsor screen, or countdown.

2. `Delay scene`
   This scene must contain a `Comp Delay Playback` source. This is what plays the delayed version of your source scene.

Then open `Tools` -> `Comp Delay Settings` and select:

- `Source scene`
- `Delay Transition Scene`
- `Delay scene`
- Video encoder
- Audio encoder
- Delay in seconds

Click `Apply` after changing settings. The button is only needed when the dialog has unapplied changes.

## How To Use

1. Set the wanted delay in `Comp Delay Settings`.
2. Click `Apply` if the settings dialog shows unapplied changes.
3. Click `Activate delay` in OBS' `Controls` dock, or use your assigned hotkey.
4. OBS switches to `Delay Transition Scene` immediately.
5. When the configured delay has filled, OBS switches to the delay scene automatically.
6. Click `Deactivate delay` to drop the buffer and return to the source scene live.

Changing the delay while it is already active uses the same configured delay workflow. The plugin shows `Delay Transition Scene` while it adjusts the buffer, then returns to delayed playback.

## Countdown Text

You can add a countdown to `Delay Transition Scene` with a normal OBS text source.

Example text:

```text
Delay starts in %delay_countdown%
```

While `Delay Transition Scene` is active, the plugin replaces `%delay_countdown%` with the remaining buffer-fill time in whole seconds. When the transition is done, the countdown text is kept blank for about five seconds before the original text template is restored.

The countdown starts when the plugin activates `Delay Transition Scene` as part of `Activate delay`. The delay scene switch is based on the encoded buffer reaching the configured delay, so the plugin does not switch early if the buffer is not ready yet.

## Audio

Any audio that should be delayed must be part of, or routed into, the selected source scene. The plugin delays the selected scene, not every arbitrary OBS audio source in the profile.

## FAQ

### Does This Stop Or Restart My Stream?

No. OBS streaming output stays active while the plugin changes scenes and fills the delay buffer.

### Why Do I Need A Delay Scene?

OBS needs a scene that contains the `Comp Delay Playback` source. That source is responsible for showing and playing the delayed buffer.

### Why Do I Need A Delay Transition Scene?

`Delay Transition Scene` gives viewers something live to see while the delay buffer is filling. Without it, viewers would see an awkward jump or incomplete delay setup.

### Can I Use Different Fixed Buttons Like 30s, 60s, And 300s?

No. The plugin uses the delay value from `Comp Delay Settings`. This keeps operation simple and avoids activating the wrong delay during a live production.

### Can I Change The Delay While It Is Active?

Yes. Change the value in `Comp Delay Settings` and click `Apply`. The plugin will use `Delay Transition Scene` while it adjusts.

### Does Manually Switching To Delay Transition Scene Start Delay?

No. Delay starts from `Activate delay`, because the plugin also has to start capture and fill the encoded buffer.

### What Happens If The Buffer Fails Or A Scene Is Missing?

The plugin falls back to `Delay Transition Scene` and shows the error in `Comp Delay Settings`.

### Does `%delay_countdown%` Work In Every Source Type?

It is intended for OBS text sources in `Delay Transition Scene`.

### Is This Cross-platform?

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

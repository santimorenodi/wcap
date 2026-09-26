---
name: setup-wcap
description: Build wcap and wcap-cli from source on Windows, and configure the wcap tray app (hotkeys, 3-2-1 countdown, output folder, codecs). Use when wcap-cli or wcap.exe is missing, needs rebuilding after source changes, or the user wants to change wcap settings or shortcuts.
---

# Build and configure wcap

## Build from source

Requirements: Windows 10 1903+ and Visual Studio 2022 (or Build Tools) with the C++ compiler
(`Microsoft.VisualStudio.Component.VC.Tools.x86.x64`) and a Windows SDK.

```bash
git clone https://github.com/santimorenodi/wcap.git
```

Build from `cmd.exe` inside the repo folder. Use `.\build.cmd`, because some shells do not search the
current folder for executables:

```bash
cmd /c "cd /d C:\path\to\wcap && .\build.cmd"
```

- `.\build.cmd` builds for the host architecture, `.\build.cmd arm64` / `x64` picks one, add `debug` for a debug build.
- Output in the repo root: `wcap-<arch>.exe` (tray app) and `wcap-cli-<arch>.exe` (console recorder).
- A `"vswhere.exe" no se reconoce` / `is not recognized` line printed by VsDevCmd is harmless.
- Success = no `error` lines and both exes exist. The build uses `/WX`, so warnings fail it.

Optionally add the repo folder to `PATH` or set `WCAP_CLI` to the full path of `wcap-cli-<arch>.exe`
so the `record-screen` skill finds it.

## Tray app (wcap.exe)

Runs in the tray. Double-click or right-click the tray icon for the settings dialog.

Default shortcuts (press the same one again to stop):

| Shortcut | Action |
|---|---|
| Ctrl + PrintScreen | record monitor under the mouse cursor |
| Ctrl + Win + PrintScreen | record active window |
| Ctrl + Shift + PrintScreen | select a region, ENTER to start, ESC to cancel |

Before recording starts, a 3, 2, 1 countdown is shown centered over the target. It is excluded from
capture, so it never appears in the video, and pressing any wcap shortcut cancels it.

If wcap says it cannot register its shortcuts, another app owns them. Change them in the settings
dialog, or check which combinations are free with `RegisterHotKey` from PowerShell before choosing.

## Configuration file

Settings live in an `.ini` next to the exe, named after it (`wcap-x64.ini`, `wcap-cli-x64.ini`), in
section `[wcap]`. Keys not in the file use defaults. Useful keys:

| Key | Default | Meaning |
|---|---|---|
| `OutputFolder` | Videos folder | where recordings go |
| `OpenFolder` | 1 | open Explorer on the file after recording (tray app) |
| `Countdown` | 3 | countdown seconds before recording, 0 disables (tray app only) |
| `CaptureAudio` / `ApplicationLocalAudio` | 1 / 1 | record audio, only the captured app's audio for window capture |
| `MouseCursor` / `ShowRecordingBorder` | 1 / 1 | cursor and yellow border |
| `VideoCodec` | H264 | also H265 / AV1 when the GPU supports it |
| `VideoMaxWidth` / `VideoMaxHeight` / `VideoMaxFramerate` | 1920 / 1080 / 60 | downscaling and fps limits |
| `VideoBitrate` / `AudioBitrate` | 8000 / 160 | kbit/s |
| `FragmentedOutput` | 0 | fragmented mp4 (H264 only) |
| `ShortcutMonitor` / `ShortcutWindow` / `ShortcutRect` | see above | `virtual key | (modifiers << 24)`, 0 disables; modifiers: Alt=1, Ctrl=2, Shift=4, Win=8 |

Example: Ctrl+Alt+F9 for monitor recording is `0x78 | (3 << 24)` = `50331768`.

Restart the tray app after editing the file by hand (settings saved from the dialog apply immediately).

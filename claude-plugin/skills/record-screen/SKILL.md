---
name: record-screen
description: Record the screen, a monitor, a window or a region to an mp4 video on Windows with wcap-cli. Use when the user asks to record, capture video, film or make a screencast of their screen, an app window or part of the screen, or when a video of UI behaviour would help verify a change.
---

# Record the screen with wcap-cli

`wcap-cli` is the console build of [wcap](https://github.com/santimorenodi/wcap). It records with
Windows.Graphics.Capture and hardware Media Foundation encoders (H264 + AAC by default), starts
immediately (no countdown, no tray icon, no hotkeys) and can run next to the tray app `wcap.exe`.

## 1. Find the executable

Look for it in this order and use the first hit:

1. `WCAP_CLI` environment variable (full path to the exe)
2. `wcap-cli-x64.exe` / `wcap-cli-arm64.exe` / `wcap-cli.exe` on `PATH`
3. The folder of a local checkout of the wcap repo (the build puts `wcap-cli-<arch>.exe` in the repo root)

```powershell
if ($env:WCAP_CLI) { $env:WCAP_CLI } else { (Get-Command wcap-cli-x64.exe, wcap-cli-arm64.exe, wcap-cli.exe -ErrorAction SilentlyContinue | Select-Object -First 1).Source }
```

If it is not found, ask the user where it is, or build it with the `setup-wcap` skill.

## 2. Pick the target

```bash
wcap-cli-x64.exe list
```

Prints monitors (`monitor N: WxH at X,Y \\.\DISPLAYn (primary)`) and capturable windows
(`window 0xHANDLE: WxH pid=N "title"`). Choose one target:

| Target | Meaning |
|---|---|
| *(none)* | primary monitor |
| `--monitor N` | monitor index from `list` |
| `--window 0x1234ab` | exact window handle from `list` (preferred, unambiguous) |
| `--window "Chrome"` | first window whose title contains the text (case-insensitive, warns if several match) |
| `--region X,Y,W,H` | rectangle in virtual screen coordinates, must lie on one monitor (odd sizes are rounded down to even) |
| `--crop X,Y,W,H` | crop of the captured area, relative to its top-left corner (of the window, or of the monitor/region). Prefer this over `--region` for a part of a window: it keeps working if the window moves |

## 3. Record

Always pass `-o` with an absolute path and, when possible, `-d` so the file is finalized cleanly.

```bash
wcap-cli-x64.exe record --window 0x1234ab -d 10 -o "C:\path\clip.mp4"
```

Options:

- `-o, --output FILE` output mp4 (default: `<Videos>\<timestamp>.mp4`)
- `-d, --duration SEC` stop after SEC seconds (integer)
- `--fps N` max framerate (0 = monitor refresh rate, default 60)
- `--bitrate KBPS` video bitrate (default 8000)
- `--max-width N` / `--max-height N` downscale limit (default 1920x1080, 0 = no limit)
- `--audio` / `--no-audio` system audio; for window capture only that app's audio is recorded
- `--no-cursor` hide mouse cursor, `--no-border` hide yellow capture border (Windows 11)
- `--fragmented` fragmented mp4 that stays playable even if the process is killed (H264 only)
- `--fps match` / `--vfr` only new frames: frames identical to the previous one are not written (variable framerate). Use this when the source renders below the monitor refresh rate, otherwise the clip contains repeated frames
- `--json` print the result as one JSON object: `{"saved":...,"frames":N,"unique_frames":N,"duplicated":N,"written_frames":N,"effective_fps":F,"dropped":N,"warnings":[...]}`
- `--timestamps` write `<clip>.frames.json` with the capture time of every frame (`qpc` ticks, `t` seconds since first frame = mp4 pts, `unique`, `out` = frame index in the clip or -1)
- `--measure-flicker` (+ `--measure-region X,Y,W,H`, repeatable, relative to the captured frame before `--max-width` scaling) mean absolute difference between consecutive captured frames, measured on uncompressed frames, in the summary / JSON `flicker`
- `--lossless` / `--frames-dir DIR` write a PNG sequence (`frame_000000.png`, ...) instead of mp4, no compression noise
- `--start-on event:NAME|file:PATH` prepare everything and start capturing only when the event is signaled (`wcap-cli signal NAME`) or the file exists; `--start-timeout SEC` to give up. Prints `waiting: ...` when ready (stdout, or stderr with `--json`)

Output on stdout, exit code 0 on success:

```
recording: C:\path\clip.mp4
video: 1920x1080 @ 60.00 fps
saved: C:\path\clip.mp4
size: 999950 bytes
dropped_frames: 0
```

Errors go to stderr as `error: ...` with exit code 1.

## Recording while doing something else

To record while you drive the UI (clicks, typing, running a program), start the recorder in the
background, do the work, then stop it:

1. Start: run `wcap-cli-x64.exe record ... -o FILE` as a background command.
2. Perform the actions.
3. Stop: `wcap-cli-x64.exe stop` (stops every running `wcap-cli record`), then wait for the background command to exit and read its `saved:` line.

Never kill the recorder process to stop it: the mp4 is only finalized on a clean stop
(`-d`, `wcap-cli stop` or Ctrl+C). If a hard kill is possible, record with `--fragmented`.

## Checking the result

If ffmpeg is available, verify the file and look at a frame instead of assuming it worked:

```bash
ffprobe -v error -show_entries format=duration:stream=codec_name,width,height -of compact clip.mp4
ffmpeg -v error -y -ss 1 -i clip.mp4 -frames:v 1 -vf scale=640:-1 frame.png
```

Then open `frame.png` with the Read tool to see it.

## Other commands

```bash
wcap-cli-x64.exe list --json --filter "Unity"          # windows/monitors as JSON, only titles containing the text
wcap-cli-x64.exe snapshot --window 0x20b0c --crop 8,40,1280,720 -o "C:\path\frame.png"   # one png, no video needed
wcap-cli-x64.exe diff before.mp4 after.mp4 --region 0,0,400,300 --json   # frame to frame difference, per region
wcap-cli-x64.exe sheet before.mp4 after.mp4 --at 0.6,3.0 -o "C:\path\compare.png"       # rows = clips, columns = times
wcap-cli-x64.exe sheet clip.mp4 --count 6 --grid -o sheet.png                              # contact sheet
```

`diff` numbers are the mean absolute difference per channel (0..255) between consecutive frames: `mean` counts identical frames as 0,
`mean_nonzero` only frames that changed. With two clips it also gives the difference of frame N of A against frame N of B and the ratio B/A.

## Notes

- Recordings show whatever is on screen, which may include private information. Save them where the user expects and do not upload them anywhere unless asked.
- Windows that set display affinity (DRM video, some password managers) cannot be captured.
- Defaults for all options come from `wcap-cli-<arch>.ini` next to the exe (same keys as the tray app's `wcap.ini`).

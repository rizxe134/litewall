## LiteWall v0.3.2

### Changes
- Lighter playback defaults: `video-sync=audio` (frames are shown on the video's own clock instead of display-locked redraws), `hr-seek=no`, `video-latency-hacks=yes`, `blend-subtitles=no`.
- New `--mpv-opts "key=value,key=value"` switch passes options to the player for testing. Applied options and any rejected ones are logged with `--log`.
- New `--exit-after-stats` and `--stat-secs N` switches, plus a `SUMMARY` log line with average RAM and CPU from 10 s onward.
- New `tools\ab-test.ps1` script that compares option sets in one run and saves `litewall-ab.csv` to the Desktop.

### Download
| File | Platform |
|---|---|
| LiteWall-v0.3.2-win-x64.zip | Windows 10/11 x64 |

### Install
1. Quit LiteWall (tray icon → Quit).
2. Extract the zip over your existing LiteWall folder. Your library and settings are kept.
3. Run `LiteWall.exe`.

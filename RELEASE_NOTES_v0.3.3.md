## LiteWall v0.3.3

### Changes
- New playback defaults, based on A/B results: `hwdec-extra-frames=0`, `vd-lavc-dr=no`, and a 30 fps display cap.
- New **Frame rate** setting: 30 fps (default), 60 fps, or Source. With Optimize on, videos above the cap are re-encoded at the capped rate, so the cap also saves decode work.
- New **Multi-monitor quality** setting: Full 100%, Balanced 75% (default), or Light 50%. When one wallpaper covers several monitors, the combined image is drawn at this scale and then stretched to fit your screens. This cuts its memory by about 44% at 75% and 75% at 50%. Full gives the sharpest picture.
- Lighter player settings: `video-sync=audio`, `hr-seek=no`, `video-latency-hacks=yes`, `blend-subtitles=no`.
- New `--mpv-opts "key=value,..."`, `--exit-after-stats`, and `--stat-secs N` switches for testing, plus a `SUMMARY` log line with average RAM and CPU.
- Measured on a 3-monitor laptop with an iGPU: about 230 MB RAM and 4% CPU with the defaults, down from about 310 MB in v0.3.1.
- New `--canvas-scale N` and `--fps-cap N` switches for testing. The log `SUMMARY` line now includes the canvas scale and fps cap.
- New `tools\ab-test.ps1` script compares canvas scales, fps caps, and per-monitor mode.
- The library window is taller so the new settings fit.

### Download
| File | Platform |
|---|---|
| LiteWall-v0.3.3-win-x64.zip | Windows 10/11 x64 |

### Install
1. Quit LiteWall (tray icon → Quit).
2. Extract the zip over your existing LiteWall folder. Your library and settings are kept.
3. Run `LiteWall.exe`. Optimized copies are re-made once for the new frame-rate setting.

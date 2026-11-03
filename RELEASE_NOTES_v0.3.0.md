## LiteWall v0.3.0

### Much lower RAM on multi-monitor and 4K setups
- **Optimize for my monitors** (new setting, on by default): videos larger than your biggest monitor, or above 25 Mbps, are transcoded once in the background to an H.264 copy at that monitor's resolution. LiteWall plays the copy and keeps your original.
  - Uses hardware encoding when available (AMD AMF, NVIDIA NVENC, Intel QSV, Media Foundation), otherwise x264.
  - Pauses while a game or fullscreen app is running.
  - Progress shows under the thumbnail.
  - Re-done automatically if you connect a bigger monitor.
- Every monitor still shows the full picture, decoded on the GPU with no CPU copies (D3D11VA).
- Trimmed per-player memory: 1 spare decoder frame (was 6), single-buffered flip-model swapchain, 8-bit output, 2 MiB read-ahead.
- **Span one wallpaper across monitors** (new setting): one player stretched over all screens, for the lowest memory use.
- `--log` diagnostics: writes `%APPDATA%\LiteWall\litewall.log` with the mpv log, rejected options, and RAM/CPU samples.

### Download
| File | Platform |
|---|---|
| LiteWall-v0.3.0-win-x64.zip | Windows 10/11 x64 |

### Install
1. Quit LiteWall (tray icon → Quit).
2. Extract the zip over your existing LiteWall folder. Your library and settings are kept.
3. Run `LiteWall.exe`. Oversized videos in your library are optimized automatically in the background.

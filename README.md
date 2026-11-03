# LiteWall

A tiny live-wallpaper app for Windows 10/11, meant as a lightweight Wallpaper Engine alternative. It plays videos (MP4, WebM, MKV, MOV; H.264, HEVC, VP9, AV1), animated GIFs, and still images behind your desktop icons at each monitor's native resolution, including 4K. It pauses itself whenever you're gaming.

* **No Electron, no browser.** It's a 30 KB native Win32 exe, and video goes through [libmpv](https://mpv.io) with GPU hardware decoding (D3D11VA).
* **One window and one decoder per monitor.**
* **Gets out of the way.** It runs at below-normal CPU priority and fully pauses when a game or other app is fullscreen or maximized, when the PC is locked, or (optionally) on battery.

## Library window (v0.2.0)
This is a dark, native Win32 window, modeled on Wallpaper Engine. It opens when LiteWall starts and when you left-click the tray icon.
* Your wallpapers appear as a thumbnail grid. Thumbnails are the first frame (rendered by libmpv) and are cached in `%APPDATA%\LiteWall\thumbs`.
* Use **+ Add wallpaper** or drag files onto the window. Files are copied into `%APPDATA%\LiteWall\library`.
* The search box filters the grid by name.
* The side panel shows a preview, the name, and the resolution. **Apply** sets the wallpaper on all monitors or just monitor N. Double-clicking a thumbnail also applies it. **Remove** (or the Del key) deletes it from your library.
* Settings: Fill/fit, pause when maximized, pause on battery, start with Windows, mute, and playback speed.
* Closing the window *destroys* it and frees all thumbnails and other UI resources, then trims memory. LiteWall stays in the tray. Starting with Windows launches it straight to the tray.

## Install

1. Download `LiteWall-v0.3.4-win-x64.zip` from Releases and extract it anywhere, for example `C:\Apps\LiteWall`. Keep `LiteWall.exe` and `libmpv-2.dll` together.
2. Run `LiteWall.exe`, drag a video, GIF, or image into the library, and click **Apply**. That's the whole setup.
3. Right-click the tray icon to change the wallpaper, pause, change auto-pause behavior, toggle **Start with Windows**, or quit.

Settings are saved to `%APPDATA%\LiteWall\settings.ini`. "Start with Windows" adds a per-user entry under `HKCU\...\Run`, and no admin rights are needed. To uninstall, untick Start with Windows, quit, and delete the folder.

## Optimize for my monitors (v0.3.0, on by default)
A 4K video on 1080p screens wastes memory: each player keeps 4K decoder surfaces and a 4K render pipeline (about 260 MB per screen). When you import a video that is larger than your biggest monitor, or above 25 Mbps, LiteWall transcodes it once in the background. It makes an H.264 copy at that monitor's resolution, stored in `library\.optimized`, and plays that copy. The original is kept.
* Encoders are tried in this order: AMD AMF, NVIDIA NVENC, Intel QSV, Media Foundation, then x264 (CPU).
* Encoding pauses whenever a game or fullscreen app is running.
* Progress appears under the thumbnail.
* If you connect a bigger monitor, the copy is re-made at the new size.

**One player for all monitors** (v0.3.1, default): when several monitors show the same wallpaper, LiteWall runs a single player in one window that covers them all. A small GPU shader draws the full picture onto each monitor's area, so every screen shows the whole wallpaper while there's only one decoder and one swapchain. Use `--per-monitor` to fall back to one player per screen.

**Span across monitors** (setting): one player covers all your monitors with a single stretched picture. This uses the least memory, but each screen shows only part of the image.

## Diagnostics
Run `LiteWall.exe --tray --log` to write `%APPDATA%\LiteWall\litewall.log`. It contains mpv's log, any rejected options, the window layout, and `STAT` lines with RAM/CPU once a second for 30 s. `--no-video`, `--per-monitor` and `--span` are for A/B tests.

## How it stays light

| Technique | Effect |
|---|---|
| libmpv with `hwdec=d3d11va` (zero-copy), `vo=gpu`, `gpu-api=d3d11`, flip model, 1-deep swapchain, rgba8 | Decoding runs on the GPU's video engine, not the CPU |
| Audio disabled, scripts/OSC/ytdl/config off, `profile=fast`, bilinear scaling | Nothing extra loaded or computed |
| 16 MiB demuxer buffer, no cache | Small memory footprint |
| 1 Hz auto-pause check (EnumWindows plus `SHQueryUserNotificationState`) | Negligible overhead |
| Pause when fullscreen/maximized/locked/on battery | ~0% CPU/GPU while you play |
| EcoQoS and working-set trim while paused | Windows schedules it on efficiency cores and reclaims RAM |
| `BELOW_NORMAL_PRIORITY_CLASS` | Games always get the CPU first |

### Auto-pause rules (per monitor)
* A visible window covers the whole monitor (borderless or windowed-fullscreen games, fullscreen video), or a window is maximized on it (can be toggled). Only that monitor pauses.
* An exclusive-fullscreen D3D game or presentation mode is detected, or the session is locked, or the PC is on battery (can be toggled). All monitors pause.

## Build (from Linux)
Requires [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) in `third_party/llvm-mingw` and the `mpv-dev-x86_64` package from [shinchiro/mpv-winbuild-cmake](https://github.com/shinchiro/mpv-winbuild-cmake/releases), extracted to `third_party/mpv`. Then run `./build.sh`, and the zip lands in `dist/`.

## Known limitations
* No wallpaper "workshop" or interactive/web wallpapers. Only videos, GIFs, and images.
* Frame rate follows the source file. Use a 30 fps file if you want even less GPU use.
* Windows 11 24H2 changed the desktop window layout. LiteWall handles both layouts, but future Explorer updates could break this.
* `libmpv-2.dll` is ~120 MB on disk because it bundles FFmpeg. Only the parts in use get loaded into RAM.

## License
GPL-3.0 (libmpv/FFmpeg in the bundled build are GPL).

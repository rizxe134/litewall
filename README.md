# LiteWall

A tiny live-wallpaper app for Windows 10/11, meant as a lightweight Wallpaper Engine alternative. It plays videos (MP4, WebM, MKV, MOV; H.264, HEVC, VP9, AV1), animated GIFs, and still images behind your desktop icons at each monitor's native resolution, including 4K. It pauses itself whenever you're gaming.

* **No Electron, no browser.** It's a 30 KB native Win32 exe, and video goes through [libmpv](https://mpv.io) with GPU hardware decoding (D3D11VA).
* **One window and one decoder per monitor.**
* **Gets out of the way.** It runs at below-normal CPU priority and fully pauses when a game or other app is fullscreen or maximized, when the PC is locked, or (optionally) on battery.

## Install

1. Download `LiteWall-v0.1.0-win-x64.zip` from Releases and extract it anywhere, for example `C:\Apps\LiteWall`. Keep `LiteWall.exe` and `libmpv-2.dll` together.
2. Run `LiteWall.exe` and pick a video, GIF, or image. That's the whole setup.
3. Right-click the tray icon to change the wallpaper, pause, change auto-pause behavior, toggle **Start with Windows**, or quit.

Settings are saved to `%APPDATA%\LiteWall\settings.ini`. "Start with Windows" adds a per-user entry under `HKCU\...\Run`, and no admin rights are needed. To uninstall, untick Start with Windows, quit, and delete the folder.

## How it stays light

| Technique | Effect |
|---|---|
| libmpv with `hwdec=auto-safe`, `vo=gpu`, `gpu-api=d3d11` | Decoding runs on the GPU's video engine, not the CPU |
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
* Every monitor shows the same file.
* Frame rate follows the source file. Use a 30 fps file if you want even less GPU use.
* Windows 11 24H2 changed the desktop window layout. LiteWall handles both layouts, but future Explorer updates could break this.
* `libmpv-2.dll` is ~120 MB on disk because it bundles FFmpeg. Only the parts in use get loaded into RAM.

## License
GPL-3.0 (libmpv/FFmpeg in the bundled build are GPL).

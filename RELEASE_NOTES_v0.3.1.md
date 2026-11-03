## LiteWall v0.3.1

### Changes
- **One player for all your monitors.** When monitors share a wallpaper, a single player now covers them all, and a GPU shader draws the full picture on each monitor's area. That means one decoder and one swapchain instead of one per screen, which cuts RAM sharply on multi-monitor setups.
- Fill/fit applies per monitor, including monitors with different aspect ratios.
- Fixed: optimizing a video could pause for minutes when no game was running. Encoding now pauses only for real fullscreen apps or games, not for battery, a locked screen, or maximized windows.
- Fullscreen detection ignores click-through overlays (GPU, Steam, Discord), invisible layered windows, and LiteWall's own windows.
- `--log` now records why each monitor paused, naming the covering window.
- The GPU shader cache and automatic ICC profile loading are turned off.

### Download
| File | Platform |
|---|---|
| LiteWall-v0.3.1-win-x64.zip | Windows 10/11 x64 |

### Install
1. Quit LiteWall (tray icon → Quit).
2. Extract the zip over your existing LiteWall folder. Your library, settings, and optimized copies are kept.
3. Run `LiteWall.exe`.

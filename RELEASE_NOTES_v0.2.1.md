## LiteWall v0.2.1

### Fixes
- The live wallpaper no longer drops back to the static Windows wallpaper when you switch virtual desktops (Win+Ctrl+←/→, Task View) or create a new one.
  - LiteWall now watches for desktop switches, at no idle CPU cost, and puts the wallpaper back right after the switch and again once the animation finishes.
  - A once-a-second check also puts the wallpaper back if Explorer ever hides or replaces it.
- Task View and the Alt-Tab switcher no longer count as "fullscreen", so they won't pause the wallpaper.

### Download
| File | Platform |
|---|---|
| LiteWall-v0.2.1-win-x64.zip | Windows 10/11 x64 |

### Install
1. Quit LiteWall (tray icon → Quit).
2. Extract the zip over your existing LiteWall folder. Your library and settings in `%APPDATA%\LiteWall` are kept.
3. Run `LiteWall.exe`.

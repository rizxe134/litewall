## LiteWall v0.3.5

### Changes
- New: **Rotation** setting in the library window: Off (default), In order, Shuffle (everything plays once before repeating, never the same twice in a row), or Random (never the same twice in a row).
- New: **Change every** setting: after each loop, 1 / 5 / 15 / 30 minutes, 1 hour, 1 day, or on startup only.
- New: **Next wallpaper** in the tray menu, a **Next ▶** button in the library window, and a **Ctrl+Alt+Right** hotkey. The hotkey is skipped if another app already uses it.
- New: tray **Rotation** submenu to pick the mode and interval.
- New: **Include in rotation** checkbox for each wallpaper. All wallpapers are included by default.
- Switching reuses the running player, so it uses no extra RAM, and plays the optimized copies. The rotation timer stops while LiteWall is paused (game, lock screen, covered, battery), so a due switch waits until playback resumes. With rotation Off, nothing extra runs.
- New test switch `--rotate-secs N` changes the wallpaper every N seconds. Rotation events are logged with `--log`.
- Lower memory: imports and optimizing now run in a separate low-priority helper process (`LiteWall.exe --encode` / `--probe`), so LiteWall's own memory stays low while videos are optimized. The helper runs at idle priority with Efficiency mode, stops while a game is fullscreen, and closes when LiteWall quits.
- Fixed: changing the frame rate (or span / monitor layout) setting and then changing it back re-encoded the whole library. LiteWall now keeps the previous optimized copy too, and only re-encodes when a copy for the current monitors and frame rate is missing or the original file is newer.

### Download
| File | Platform |
|---|---|
| LiteWall-v0.3.5-win-x64.zip | Windows 10/11 x64 |

### Install
1. Quit LiteWall (tray icon → Quit).
2. Extract the zip over your existing LiteWall folder. Your library and settings are kept.
3. Run `LiteWall.exe`.

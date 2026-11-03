## LiteWall v0.3.6

### Changes
- Fixed: the wallpaper glitched, flashed black or stuttered when you changed settings. These changes now apply live, without restarting the wallpaper:
  - **Multi-monitor quality** and **Fill screen** update instantly.
  - **Frame rate** updates instantly. If a copy for the new frame rate doesn't exist yet, the current optimized copy keeps playing until the new one is ready, then it swaps in seamlessly at the same position. Previously the 4K original played in the meantime.
  - **Applying a wallpaper or image** from the library (and rotation, Choose file) switches on the running player.
  - A newly optimized copy swaps in seamlessly instead of restarting the wallpaper.
- Scrolling through the Frame rate or Multi-monitor quality list now applies only the final choice.
- When a full restart is still needed (span toggle, display change, per-monitor layout change), LiteWall reuses the desktop host window instead of asking Explorer to recreate it, so there's no extra Explorer repaint.
- New test switch `--test-settings` cycles these settings every 5 s and logs each step.

### Download
| File | Platform |
|---|---|
| LiteWall-v0.3.6-win-x64.zip | Windows 10/11 x64 |

### Install
1. Quit LiteWall (tray icon → Quit).
2. Extract the zip over your existing LiteWall folder. Your library and settings are kept.
3. Run `LiteWall.exe`.

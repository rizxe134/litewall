## LiteWall v0.2.0

### New: Library window
- Dark, native library window (no web engine) that opens on launch and when you click the tray icon
- Thumbnail grid made from each wallpaper's first frame (video, GIF, or image), cached in `%APPDATA%\LiteWall\thumbs`
- **Add wallpaper** button (multi-select) and drag-and-drop. Files are copied into `%APPDATA%\LiteWall\library`
- Search box that filters wallpapers by name
- Side panel with preview, name, resolution, and **Apply** to all monitors or a single monitor
- **Remove** from library (button or Del key)
- Settings in the window: fill/fit, pause when maximized, pause on battery, start with Windows, mute audio, playback speed (0.25×–2×)
- Closing the window destroys it and frees thumbnails and other UI memory. LiteWall keeps running in the tray

### Other changes
- Each monitor can have its own wallpaper
- Wallpapers can now play their audio (mute is on by default)
- Start with Windows now launches straight to the tray

### Download
| File | Platform |
|---|---|
| LiteWall-v0.2.0-win-x64.zip | Windows 10/11 x64 |

### Install
1. Quit any running LiteWall (tray icon → Quit).
2. Extract the zip to a folder, replacing the old files if you're updating.
3. Run `LiteWall.exe`, drag wallpapers into the library, select one, and click **Apply**.

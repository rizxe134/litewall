// LiteWall - lightweight live wallpaper for Windows 10/11.
// Renders video/GIF/images behind desktop icons via libmpv (hardware decoding).
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>
#include <wtsapi32.h>
#include <psapi.h>
#include <stdio.h>
#include <wchar.h>
#include <mpv/client.h>
#include <commctrl.h>

#define APP_NAME L"LiteWall"
#define WM_TRAY (WM_APP + 1)
#define WM_MPV_WAKE (WM_APP + 2)
#define ID_TIMER 1
enum { IDM_LIBRARY = 99, IDM_CHOOSE = 100, IDM_PAUSE, IDM_STARTUP, IDM_BATTERY, IDM_MAXIMIZED, IDM_FIT, IDM_QUIT };

#define MAX_MON 8
typedef struct { HWND hwnd; mpv_handle *mpv; RECT rc; int paused; } Screen;

static HINSTANCE g_inst;
static HWND g_msg;              // hidden message window
static HWND g_workerw;          // parent for wallpaper windows
static Screen g_scr[MAX_MON];
static int g_nscr;
static NOTIFYICONDATAW g_nid;
static UINT g_taskbarCreated;
static wchar_t g_ini[MAX_PATH];
static wchar_t g_file[MAX_PATH * 2];
static int g_userPaused, g_locked, g_pauseOnBattery = 1, g_pauseOnMaximized = 1, g_fill = 1;
static int g_allPaused = -1;
static wchar_t g_monFile[MAX_MON][MAX_PATH * 2]; // per-monitor override ("" = use g_file)
static int g_mute = 1, g_speedPct = 100;
static wchar_t g_appdir[MAX_PATH];
static const wchar_t *screen_file(int i) { return (i < MAX_MON && g_monFile[i][0]) ? g_monFile[i] : g_file; }
static void ui_open(void);

/* ---------- settings ---------- */
static void load_settings(void) {
    wchar_t dir[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, dir))) {
        wcscat(dir, L"\\LiteWall"); CreateDirectoryW(dir, NULL);
        swprintf(g_ini, MAX_PATH, L"%s\\settings.ini", dir); wcscpy(g_appdir, dir);
    }
    GetPrivateProfileStringW(L"main", L"file", L"", g_file, MAX_PATH * 2, g_ini);
    g_userPaused = GetPrivateProfileIntW(L"main", L"paused", 0, g_ini);
    g_pauseOnBattery = GetPrivateProfileIntW(L"main", L"pause_on_battery", 1, g_ini);
    g_pauseOnMaximized = GetPrivateProfileIntW(L"main", L"pause_on_maximized", 1, g_ini);
    g_fill = GetPrivateProfileIntW(L"main", L"fill", 1, g_ini);
    g_mute = GetPrivateProfileIntW(L"main", L"mute", 1, g_ini);
    g_speedPct = GetPrivateProfileIntW(L"main", L"speed_pct", 100, g_ini);
    for (int i = 0; i < MAX_MON; i++) { wchar_t k[16]; swprintf(k, 16, L"file%d", i); GetPrivateProfileStringW(L"main", k, L"", g_monFile[i], MAX_PATH * 2, g_ini); }
}
static void save_int(const wchar_t *k, int v) { wchar_t b[16]; swprintf(b, 16, L"%d", v); WritePrivateProfileStringW(L"main", k, b, g_ini); }
static void save_settings(void) {
    WritePrivateProfileStringW(L"main", L"file", g_file, g_ini);
    save_int(L"paused", g_userPaused); save_int(L"pause_on_battery", g_pauseOnBattery);
    save_int(L"pause_on_maximized", g_pauseOnMaximized); save_int(L"fill", g_fill); save_int(L"mute", g_mute); save_int(L"speed_pct", g_speedPct);
    for (int i = 0; i < MAX_MON; i++) { wchar_t k[16]; swprintf(k, 16, L"file%d", i); WritePrivateProfileStringW(L"main", k, g_monFile[i], g_ini); }
}

/* ---------- start with Windows ---------- */
static const wchar_t *RUNKEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static int startup_enabled(void) {
    HKEY k; int r = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        r = RegQueryValueExW(k, APP_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS; RegCloseKey(k);
    }
    return r;
}
static void set_startup(int on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH], v[MAX_PATH + 16];
        GetModuleFileNameW(NULL, exe, MAX_PATH); swprintf(v, MAX_PATH + 16, L"\"%s\" --tray", exe);
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (BYTE *)v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(k, APP_NAME);
    RegCloseKey(k);
}

/* ---------- WorkerW discovery ---------- */
static BOOL CALLBACK find_workerw(HWND top, LPARAM lp) {
    (void)lp;
    if (FindWindowExW(top, NULL, L"SHELLDLL_DefView", NULL)) {
        HWND w = FindWindowExW(NULL, top, L"WorkerW", NULL);   // Win10 / Win11 <24H2
        if (w) g_workerw = w;
    }
    return TRUE;
}
static HWND get_wallpaper_parent(void) {
    HWND progman = FindWindowW(L"Progman", NULL);
    DWORD_PTR res;
    // Ask Explorer to spawn the WorkerW that sits between icons and wallpaper.
    SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &res);
    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &res);
    g_workerw = NULL;
    EnumWindows(find_workerw, 0);
    if (!g_workerw) g_workerw = FindWindowExW(progman, NULL, L"WorkerW", NULL); // Win11 24H2+
    if (!g_workerw) g_workerw = progman;
    return g_workerw;
}

/* ---------- mpv ---------- */
static void on_mpv_wakeup(void *ctx) { PostMessageW(g_msg, WM_MPV_WAKE, 0, (LPARAM)ctx); }

static void to_utf8(const wchar_t *w, char *out, int n) { WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL); }

static void mpv_load(Screen *s) {
    const wchar_t *f = screen_file((int)(s - g_scr));
    if (!s->mpv || !f[0]) return;
    char path[MAX_PATH * 6]; to_utf8(f, path, sizeof path);
    const char *cmd[] = {"loadfile", path, NULL};
    mpv_command_async(s->mpv, 0, cmd);
}

static mpv_handle *mpv_start(HWND wnd) {
    mpv_handle *m = mpv_create();
    if (!m) return NULL;
    int64_t wid = (int64_t)(intptr_t)wnd;
    mpv_set_option(m, "wid", MPV_FORMAT_INT64, &wid);
    static const char *opts[][2] = {
        {"config", "no"}, {"terminal", "no"}, {"msg-level", "all=no"},
        {"input-default-bindings", "no"}, {"input-vo-keyboard", "no"}, {"input-cursor", "no"},
        {"cursor-autohide", "no"}, {"osc", "no"}, {"osd-level", "0"}, {"ytdl", "no"},
        {"load-scripts", "no"}, {"load-stats-overlay", "no"}, {"load-console", "no"},
        {"sub-auto", "no"}, {"load-auto-profiles", "no"},
        {"hwdec", "auto-safe"},                 // D3D11VA/DXVA2/NVDEC hardware decoding
        {"hwdec-codecs", "all"},
        {"vo", "gpu"}, {"gpu-api", "d3d11"}, {"profile", "fast"},
        {"vd-lavc-threads", "2"},               // cap SW decode threads if HW unavailable
        {"loop-file", "inf"}, {"image-display-duration", "inf"},
        {"keep-open", "yes"}, {"idle", "yes"}, {"force-window", "yes"},
        {"demuxer-max-bytes", "16MiB"}, {"demuxer-max-back-bytes", "0"}, {"cache", "no"},
        {"background-color", "#000000"}, {"stop-screensaver", "no"},
        {"video-sync", "display-desync"}, {"interpolation", "no"}, {"deband", "no"},
        {"dither-depth", "no"}, {"scale", "bilinear"}, {"dscale", "bilinear"}, {"cscale", "bilinear"},
        {"correct-downscaling", "no"}, {"sigmoid-upscaling", "no"}, {"hdr-compute-peak", "no"},
        {"swapchain-depth", "2"},
    };
    for (size_t i = 0; i < sizeof opts / sizeof opts[0]; i++) mpv_set_option_string(m, opts[i][0], opts[i][1]);
    mpv_set_option_string(m, "panscan", g_fill ? "1.0" : "0.0");
    mpv_set_option_string(m, "aid", g_mute ? "no" : "auto");
    { double sp = g_speedPct / 100.0; mpv_set_option(m, "speed", MPV_FORMAT_DOUBLE, &sp); }
    if (mpv_initialize(m) < 0) { mpv_terminate_destroy(m); return NULL; }
    mpv_set_wakeup_callback(m, on_mpv_wakeup, m);
    return m;
}

/* ---------- wallpaper windows ---------- */
static LRESULT CALLBACK wall_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) { RECT r; GetClientRect(h, &r); FillRect((HDC)wp, &r, GetStockObject(BLACK_BRUSH)); return 1; }
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, msg, wp, lp);
}

static BOOL CALLBACK enum_mon(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp) {
    (void)hm; (void)dc; (void)lp;
    if (g_nscr < MAX_MON) { g_scr[g_nscr].rc = *rc; g_scr[g_nscr].paused = -1; g_nscr++; }
    return TRUE;
}

static void destroy_screens(void) {
    for (int i = 0; i < g_nscr; i++) {
        if (g_scr[i].mpv) mpv_terminate_destroy(g_scr[i].mpv);
        if (g_scr[i].hwnd && IsWindow(g_scr[i].hwnd)) DestroyWindow(g_scr[i].hwnd);
    }
    memset(g_scr, 0, sizeof g_scr); g_nscr = 0; g_allPaused = -1;
}

static void create_screens(void) {
    destroy_screens();
    int any = g_file[0] != 0; for (int i = 0; i < MAX_MON; i++) any |= g_monFile[i][0] != 0;
    if (!any) return;
    HWND parent = get_wallpaper_parent();
    EnumDisplayMonitors(NULL, NULL, enum_mon, 0);
    for (int i = 0; i < g_nscr; i++) {
        Screen *s = &g_scr[i];
        POINT p = {s->rc.left, s->rc.top};
        MapWindowPoints(HWND_DESKTOP, parent, &p, 1);
        s->hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"LiteWallSurface", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                  p.x, p.y, s->rc.right - s->rc.left, s->rc.bottom - s->rc.top, parent, NULL, g_inst, NULL);
        if (!s->hwnd) continue;
        if (!screen_file(i)[0]) { ShowWindow(s->hwnd, SW_HIDE); continue; }
        // Win11 24H2: keep our surface beneath the icon view inside Progman.
        SetWindowPos(s->hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        s->mpv = mpv_start(s->hwnd);
        mpv_load(s);
    }
}

/* ---------- auto-pause logic ---------- */
static int is_shell_window(HWND w) {
    wchar_t cls[64]; GetClassNameW(w, cls, 64);
    return !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
           !wcscmp(cls, L"Shell_SecondaryTrayWnd") || !wcscmp(cls, L"LiteWallSurface") ||
           !wcscmp(cls, L"Windows.UI.Core.CoreWindow") || !wcscmp(cls, L"XamlExplorerHostIslandWindow");
}

// Does a visible top-level window cover (fullscreen) or maximize onto this monitor?
typedef struct { RECT mon; int covered; } CoverCtx;
static BOOL CALLBACK cover_enum(HWND w, LPARAM lp) {
    CoverCtx *c = (CoverCtx *)lp;
    if (!IsWindowVisible(w) || IsIconic(w) || is_shell_window(w)) return TRUE;
    BOOL cloaked = FALSE;
    typedef HRESULT(WINAPI * DwmGetAttr)(HWND, DWORD, PVOID, DWORD);
    static DwmGetAttr dga; static int tried;
    if (!tried) { tried = 1; HMODULE d = LoadLibraryW(L"dwmapi.dll"); if (d) dga = (DwmGetAttr)GetProcAddress(d, "DwmGetWindowAttribute"); }
    if (dga) dga(w, 14 /*DWMWA_CLOAKED*/, &cloaked, sizeof cloaked);
    if (cloaked) return TRUE;
    RECT r; if (!GetWindowRect(w, &r)) return TRUE;
    int full = r.left <= c->mon.left && r.top <= c->mon.top && r.right >= c->mon.right && r.bottom >= c->mon.bottom;
    int maxed = g_pauseOnMaximized && IsZoomed(w) && MonitorFromWindow(w, MONITOR_DEFAULTTONULL) == MonitorFromRect(&c->mon, MONITOR_DEFAULTTONULL);
    if (full || maxed) { c->covered = 1; return FALSE; }
    return TRUE;
}

static void apply_pause(void) {
    int global = g_userPaused || g_locked;
    if (!global && g_pauseOnBattery) {
        SYSTEM_POWER_STATUS ps;
        if (GetSystemPowerStatus(&ps) && ps.ACLineStatus == 0) global = 1;
    }
    if (!global) {
        QUERY_USER_NOTIFICATION_STATE st;
        if (SUCCEEDED(SHQueryUserNotificationState(&st)) &&
            (st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE)) global = 1; // exclusive-fullscreen games
    }
    int all = 1;
    for (int i = 0; i < g_nscr; i++) {
        Screen *s = &g_scr[i];
        int want = global;
        if (!want) { CoverCtx c = {s->rc, 0}; EnumWindows(cover_enum, (LPARAM)&c); want = c.covered; }
        if (want != s->paused && s->mpv) {
            int f = want; mpv_set_property(s->mpv, "pause", MPV_FORMAT_FLAG, &f); s->paused = want;
        }
        all &= want;
    }
    if (all != g_allPaused) {
        g_allPaused = all;
        // While everything is paused: ask Windows for EcoQoS + trim the working set.
        PROCESS_POWER_THROTTLING_STATE t = {PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                            PROCESS_POWER_THROTTLING_EXECUTION_SPEED, all ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0};
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &t, sizeof t);
        if (all) { Sleep(150); EmptyWorkingSet(GetCurrentProcess()); }
    }
}

/* ---------- tray ---------- */
static void tray_add(void) {
    memset(&g_nid, 0, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid; g_nid.hWnd = g_msg; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcscpy(g_nid.szTip, L"LiteWall");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static int choose_file(HWND owner) {
    wchar_t buf[MAX_PATH * 2] = L"";
    OPENFILENAMEW o = {sizeof o};
    o.hwndOwner = owner; o.lpstrFile = buf; o.nMaxFile = MAX_PATH * 2;
    o.lpstrFilter = L"Wallpapers (video, GIF, image)\0*.mp4;*.webm;*.mkv;*.mov;*.avi;*.m4v;*.wmv;*.gif;*.png;*.jpg;*.jpeg;*.bmp;*.webp\0All files\0*.*\0";
    o.lpstrTitle = L"LiteWall - choose a wallpaper";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&o)) return 0;
    wcscpy(g_file, buf); save_settings();
    return 1;
}

static void show_menu(HWND h) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_DEFAULT, IDM_LIBRARY, L"Open library...");
    AppendMenuW(m, MF_STRING, IDM_CHOOSE, L"Choose file...");
    AppendMenuW(m, MF_STRING | (g_userPaused ? MF_CHECKED : 0), IDM_PAUSE, L"Pause");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_fill ? MF_CHECKED : 0), IDM_FIT, L"Fill screen (crop)");
    AppendMenuW(m, MF_STRING | (g_pauseOnMaximized ? MF_CHECKED : 0), IDM_MAXIMIZED, L"Pause when a window is maximized");
    AppendMenuW(m, MF_STRING | (g_pauseOnBattery ? MF_CHECKED : 0), IDM_BATTERY, L"Pause on battery");
    AppendMenuW(m, MF_STRING | (startup_enabled() ? MF_CHECKED : 0), IDM_STARTUP, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_QUIT, L"Quit");
    POINT p; GetCursorPos(&p); SetForegroundWindow(h);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, h, NULL);
    PostMessageW(h, WM_NULL, 0, 0); DestroyMenu(m);
}

static void restore_static_wallpaper(void) {
    // Force Explorer to repaint the normal wallpaper after we leave.
    wchar_t wp[MAX_PATH] = L"";
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wp, 0);
    SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, wp, SPIF_SENDCHANGE);
}

#include "ui.c"

static LRESULT CALLBACK msg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && msg) { tray_add(); create_screens(); return 0; } // Explorer restarted
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP) show_menu(h); else if (LOWORD(lp) == WM_LBUTTONUP) ui_open();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_LIBRARY: ui_open(); return 0;
        case IDM_CHOOSE: if (choose_file(h)) { memset(g_monFile, 0, sizeof g_monFile); save_settings(); create_screens(); } break;
        case IDM_PAUSE: g_userPaused = !g_userPaused; save_settings(); break;
        case IDM_FIT: g_fill = !g_fill; save_settings();
            for (int i = 0; i < g_nscr; i++) if (g_scr[i].mpv) mpv_set_property_string(g_scr[i].mpv, "panscan", g_fill ? "1.0" : "0.0");
            break;
        case IDM_MAXIMIZED: g_pauseOnMaximized = !g_pauseOnMaximized; save_settings(); break;
        case IDM_BATTERY: g_pauseOnBattery = !g_pauseOnBattery; save_settings(); break;
        case IDM_STARTUP: set_startup(!startup_enabled()); break;
        case IDM_QUIT: DestroyWindow(h); return 0;
        }
        apply_pause();
        return 0;
    case WM_MPV_WAKE: {
        mpv_handle *m = (mpv_handle *)lp;
        for (int i = 0; i < g_nscr; i++) if (g_scr[i].mpv == m) {
            for (;;) { mpv_event *e = mpv_wait_event(m, 0); if (e->event_id == MPV_EVENT_NONE) break; }
        }
        return 0;
    }
    case WM_TIMER: apply_pause(); return 0;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) g_locked = 1; else if (wp == WTS_SESSION_UNLOCK) g_locked = 0;
        apply_pause(); return 0;
    case WM_POWERBROADCAST: apply_pause(); return TRUE;
    case WM_DISPLAYCHANGE: case WM_SETTINGCHANGE:
        if (msg == WM_DISPLAYCHANGE) SetTimer(h, 2, 1500, NULL);   // debounce, then rebuild
        return 0;
    case WM_DESTROY:
        KillTimer(h, ID_TIMER); WTSUnRegisterSessionNotification(h);
        Shell_NotifyIconW(NIM_DELETE, &g_nid); destroy_screens(); restore_static_wallpaper();
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK msg_proc_wrap(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == 2) { KillTimer(h, 2); create_screens(); apply_pause(); return 0; }
    return msg_proc(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;
    g_inst = inst;
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"LiteWall.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { MessageBoxW(NULL, L"LiteWall is already running (see the tray icon).", APP_NAME, MB_OK | MB_ICONINFORMATION); return 0; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS); // games always win the CPU
    load_settings();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = msg_proc_wrap; wc.hInstance = inst; wc.lpszClassName = L"LiteWallMsg";
    RegisterClassW(&wc);
    WNDCLASSW ws = {0};
    ws.lpfnWndProc = wall_proc; ws.hInstance = inst; ws.lpszClassName = L"LiteWallSurface";
    ws.hbrBackground = GetStockObject(BLACK_BRUSH); ws.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassW(&ws);

    g_msg = CreateWindowExW(0, L"LiteWallMsg", APP_NAME, 0, 0, 0, 0, 0, NULL, NULL, inst, NULL); // top-level hidden (gets broadcasts)
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    tray_add();
    WTSRegisterSessionNotification(g_msg, NOTIFY_FOR_THIS_SESSION);

    { INITCOMMONCONTROLSEX ic = {sizeof ic, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES}; InitCommonControlsEx(&ic); }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    create_screens();
    if (!wcsstr(GetCommandLineW(), L"--tray")) ui_open();   // autostart passes --tray
    apply_pause();
    SetTimer(g_msg, ID_TIMER, 1000, NULL);  // 1 Hz check - negligible cost

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    ReleaseMutex(mutex);
    return 0;
}

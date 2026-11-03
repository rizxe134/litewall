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
#include <stdarg.h>
#include <wchar.h>
#include <mpv/client.h>
#include <commctrl.h>

#define APP_NAME L"LiteWall"
#define WM_TRAY (WM_APP + 1)
#define WM_MPV_WAKE (WM_APP + 2)
#define ID_TIMER 1
enum { IDM_LIBRARY = 99, IDM_CHOOSE = 100, IDM_PAUSE, IDM_STARTUP, IDM_BATTERY, IDM_MAXIMIZED, IDM_FIT, IDM_QUIT };

#define MAX_MON 8
typedef struct { HWND hwnd; RECT rc, wr; int paused; int grp; } Screen; // rc = monitor, wr = our window
/* diagnostics / A-B modes (v0.2.3-diag) */
enum { MODE_TILE = 0, MODE_PERMON = 1, MODE_SPAN = 2 };
static int g_gameActive;
static int g_fpsCap = 30, g_canvasPct = 75, g_cliCanvas = -1, g_cliFps = -1;   // fps cap (0 = source), tile canvas internal scale %
static char g_mpvOpts[2048];   // --mpv-opts "k=v,k=v" passthrough (A/B testing)
static int g_statSecs = 30, g_exitAfterStats;
static double g_sumPriv, g_sumCpu; static int g_nSum;   // a fullscreen app / game is up (drives encoder pausing)
static void opt_kick(void);
static int g_optimize = 1, g_spanSetting = 0;
static int g_mode = MODE_PERMON, g_log = 0, g_noVideo = 0, g_statTicks = 0;
static FILE *g_logf;
static void logf_(const char *fmt, ...) {
    if (!g_logf) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_logf, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_logf, fmt, a); va_end(a); fputc('\n', g_logf); fflush(g_logf);
}
#define LOG(...) logf_(__VA_ARGS__)

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
    g_optimize = GetPrivateProfileIntW(L"main", L"optimize", 1, g_ini);
    g_spanSetting = GetPrivateProfileIntW(L"main", L"span", 0, g_ini);
    g_fpsCap = GetPrivateProfileIntW(L"main", L"fps_cap", 30, g_ini);
    g_canvasPct = GetPrivateProfileIntW(L"main", L"canvas_pct", 75, g_ini);
    for (int i = 0; i < MAX_MON; i++) { wchar_t k[16]; swprintf(k, 16, L"file%d", i); GetPrivateProfileStringW(L"main", k, L"", g_monFile[i], MAX_PATH * 2, g_ini); }
}
static void save_int(const wchar_t *k, int v) { wchar_t b[16]; swprintf(b, 16, L"%d", v); WritePrivateProfileStringW(L"main", k, b, g_ini); }
static void save_settings(void) {
    WritePrivateProfileStringW(L"main", L"file", g_file, g_ini);
    save_int(L"paused", g_userPaused); save_int(L"pause_on_battery", g_pauseOnBattery);
    save_int(L"pause_on_maximized", g_pauseOnMaximized); save_int(L"fill", g_fill); save_int(L"mute", g_mute); save_int(L"speed_pct", g_speedPct); save_int(L"optimize", g_optimize); save_int(L"span", g_spanSetting); save_int(L"fps_cap", g_fpsCap); save_int(L"canvas_pct", g_canvasPct);
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

/* ---------- mpv ----------
   v0.3.0: one native mpv window per monitor (vo=gpu, gpu-api=d3d11, zero-copy d3d11va), or one per
   group of monitors in span mode. Memory is kept low by trimming mpv buffers and, mainly, by playing
   an optimized copy at monitor resolution instead of a 4K original (see optimizer.c). */
#define MAX_GRP MAX_MON
typedef struct { mpv_handle *mpv; wchar_t file[MAX_PATH * 2]; int paused; } Group;
static Group g_grp[MAX_GRP];
static int g_ngrp;
static int g_maxW = 1920, g_maxH = 1080;          // largest playback target (monitor, or span box)
static void request_redraw(void) {}
static const wchar_t *optimized_for(const wchar_t *orig, int tw, int th, wchar_t *out); // optimizer.c

static void on_mpv_wakeup(void *ctx) { PostMessageW(g_msg, WM_MPV_WAKE, 0, (LPARAM)ctx); }
static void to_utf8(const wchar_t *w, char *out, int n) { WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL); }

static void players_set_string(const char *k, const char *v) { for (int i = 0; i < g_ngrp; i++) if (g_grp[i].mpv) mpv_set_property_string(g_grp[i].mpv, k, v); }
static void players_set_double(const char *k, double v) { for (int i = 0; i < g_ngrp; i++) if (g_grp[i].mpv) mpv_set_property(g_grp[i].mpv, k, MPV_FORMAT_DOUBLE, &v); }

static void setopt(mpv_handle *m, const char *k, const char *v) {
    int e = mpv_set_option_string(m, k, v);
    if (e < 0) LOG("mpv option REJECTED %s=%s (%s)", k, v, mpv_error_string(e));
}
static mpv_handle *mpv_start(HWND wid_wnd, const wchar_t *shader) {
    mpv_handle *m = mpv_create();
    if (!m) return NULL;
    static const char *opts[][2] = {
        {"config", "no"}, {"terminal", "no"}, {"msg-level", "all=no"},
        {"input-default-bindings", "no"}, {"input-vo-keyboard", "no"}, {"input-cursor", "no"}, {"cursor-autohide", "no"},
        {"osc", "no"}, {"osd-level", "0"}, {"ytdl", "no"}, {"load-scripts", "no"}, {"load-stats-overlay", "no"},
        {"load-console", "no"}, {"load-select", "no"}, {"load-commands", "no"}, {"load-positioning", "no"}, {"load-context-menu", "no"},
        {"sub-auto", "no"}, {"sid", "no"}, {"load-auto-profiles", "no"},
        {"vo", "gpu"}, {"gpu-api", "d3d11"}, {"hwdec", "d3d11va,auto-safe"}, {"hwdec-codecs", "all"},
        {"hwdec-extra-frames", "0"},             // A/B on target PC: -15 MB vs 1, no stutter
        {"vd-lavc-dr", "no"},
        {"vd-lavc-threads", "1"},
        {"d3d11-flip", "yes"}, {"swapchain-depth", "1"}, {"d3d11-output-format", "rgba8"},
        {"profile", "fast"}, {"fbo-format", "rgba8"}, {"interpolation", "no"}, {"deband", "no"},
        {"dither-depth", "no"}, {"scale", "bilinear"}, {"dscale", "bilinear"}, {"cscale", "bilinear"},
        {"correct-downscaling", "no"}, {"linear-downscaling", "no"}, {"sigmoid-upscaling", "no"},
        {"hdr-compute-peak", "no"}, {"video-sync", "audio"}, {"blend-subtitles", "no"}, {"hr-seek", "no"}, {"video-latency-hacks", "yes"}, {"force-window", "yes"},
        {"loop-file", "inf"}, {"image-display-duration", "inf"}, {"keep-open", "yes"}, {"idle", "yes"},
        {"cache", "no"}, {"demuxer-max-bytes", "2MiB"}, {"demuxer-max-back-bytes", "0"},
        {"demuxer-readahead-secs", "1"}, {"audio-buffer", "0.1"}, {"stop-screensaver", "no"},
    };
    for (size_t i = 0; i < sizeof opts / sizeof opts[0]; i++) setopt(m, opts[i][0], opts[i][1]);
    int64_t wid = (int64_t)(intptr_t)wid_wnd; mpv_set_option(m, "wid", MPV_FORMAT_INT64, &wid);
    if (shader) {   // tile mode: our GPU shader lays out one full picture per monitor on a single canvas
        char sp8[MAX_PATH * 6]; to_utf8(shader, sp8, sizeof sp8);
        setopt(m, "glsl-shaders", sp8); setopt(m, "keepaspect", "no"); setopt(m, "panscan", "0.0");
    } else setopt(m, "panscan", g_fill ? "1.0" : "0.0");
    if (g_fpsCap > 0) { char b[16]; snprintf(b, sizeof b, "%d", g_fpsCap); setopt(m, "override-display-fps", b); }
    setopt(m, "gpu-shader-cache", "no"); setopt(m, "icc-profile-auto", "no");
    if (g_mpvOpts[0]) {   // user overrides last, so they win
        char buf[2048]; strcpy(buf, g_mpvOpts);
        for (char *tok = strtok(buf, ",;"); tok; tok = strtok(NULL, ",;")) {
            while (*tok == ' ') tok++;
            char *eq = strchr(tok, '='); if (!eq) { setopt(m, tok, "yes"); LOG("mpv-opts: %s=yes", tok); continue; }
            *eq = 0; setopt(m, tok, eq + 1); LOG("mpv-opts: %s=%s", tok, eq + 1);
        }
    }
    setopt(m, "aid", g_mute ? "no" : "auto");
    { double sp = g_speedPct / 100.0; mpv_set_option(m, "speed", MPV_FORMAT_DOUBLE, &sp); }
    if (mpv_initialize(m) < 0) { LOG("mpv_initialize FAILED"); mpv_terminate_destroy(m); return NULL; }
    if (g_log) mpv_request_log_messages(m, "v");
    mpv_set_wakeup_callback(m, on_mpv_wakeup, m);
    return m;
}

static void mpv_load_group(Group *G, int tw, int th) {
    wchar_t opt[MAX_PATH * 2]; const wchar_t *f = G->file;
    if (g_optimize && optimized_for(G->file, tw, th, opt)) f = opt;
    LOG("load %ls%s", f, f == G->file ? "" : "  (optimized copy)");
    char path[MAX_PATH * 6]; to_utf8(f, path, sizeof path);
    const char *cmd[] = {"loadfile", path, NULL};
    mpv_command_async(G->mpv, 0, cmd);
}

/* ---------- wallpaper windows ---------- */
static LRESULT CALLBACK wall_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) { RECT r; GetClientRect(h, &r); FillRect((HDC)wp, &r, GetStockObject(BLACK_BRUSH)); return 1; }
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, msg, wp, lp);
}

static BOOL CALLBACK enum_mon(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp) {
    (void)hm; (void)dc; (void)lp;
    if (g_nscr < MAX_MON) { memset(&g_scr[g_nscr], 0, sizeof(Screen)); g_scr[g_nscr].rc = *rc; g_scr[g_nscr].paused = -1; g_scr[g_nscr].grp = -1; g_nscr++; }
    return TRUE;
}

static void destroy_screens(void) {
    for (int i = 0; i < g_ngrp; i++) if (g_grp[i].mpv) mpv_terminate_destroy(g_grp[i].mpv);
    memset(g_grp, 0, sizeof g_grp); g_ngrp = 0;
    for (int i = 0; i < g_nscr; i++) if (g_scr[i].hwnd && IsWindow(g_scr[i].hwnd)) DestroyWindow(g_scr[i].hwnd);
    memset(g_scr, 0, sizeof g_scr); g_nscr = 0; g_allPaused = -1;
}

/* ---------- virtual-desktop / Explorer resilience ---------- */
// Cheap check: is our surface still attached to a live, visible wallpaper host?
static int surfaces_healthy(void) {
    if (!g_nscr) return 1;
    if (!IsWindow(g_workerw) || !IsWindowVisible(g_workerw)) return 0;
    for (int i = 0; i < g_nscr; i++) {
        HWND w = g_scr[i].hwnd;
        if (!w || !screen_file(i)[0]) continue;
        if (!IsWindow(w) || GetParent(w) != g_workerw || !IsWindowVisible(w)) return 0;
    }
    return 1;
}
static void create_screens(void);
// Re-attach after a virtual-desktop switch: Explorer repaints the per-desktop static wallpaper
// and may hide/recreate the WorkerW. Re-show or re-parent without restarting playback if possible.
static void reattach_screens(void) {
    if (!g_nscr) return;
    HWND old = g_workerw;
    HWND parent = get_wallpaper_parent();     // re-sends 0x052C so the WorkerW exists again
    if (parent != old) {                      // host changed: move our surfaces (playback keeps going)
        for (int i = 0; i < g_nscr; i++) {
            if (!g_scr[i].hwnd) continue;
            if (!IsWindow(g_scr[i].hwnd)) { create_screens(); return; }
            SetParent(g_scr[i].hwnd, parent);
        }
    }
    if (!IsWindowVisible(parent)) ShowWindow(parent, SW_SHOWNA);
    for (int i = 0; i < g_nscr; i++) {
        Screen *s = &g_scr[i];
        if (!s->hwnd || !screen_file(i)[0]) continue;
        POINT p = {s->wr.left, s->wr.top}; MapWindowPoints(HWND_DESKTOP, parent, &p, 1);
        SetWindowPos(s->hwnd, HWND_BOTTOM, p.x, p.y, s->wr.right - s->wr.left, s->wr.bottom - s->wr.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        RedrawWindow(s->hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    request_redraw();
}

/* Tile mode: one player, one window covering the bounding box of the monitors that share a file.
   A user shader on MAIN resizes the frame to the canvas and, per output pixel, samples the video
   with cover (fill) or fit mapping for whichever monitor that pixel belongs to. GPU-only work. */
static const wchar_t *write_tile_shader(int grp, const RECT *box, wchar_t *path) {
    int n = 0; for (int i = 0; i < g_nscr; i++) if (g_scr[i].grp == grp) n++;
    if (n < 2) return NULL;                       // single monitor: plain playback, no shader
    swprintf(path, MAX_PATH, L"%s\\tile%d.glsl", g_appdir, grp);
    FILE *f = _wfopen(path, L"w"); if (!f) return NULL;
    int W = box->right - box->left, H = box->bottom - box->top;
    // The canvas is rendered at g_canvasPct of the window size and stretched to the window by mpv's
    // final output pass, so the big intermediate texture shrinks by pct^2 (75% -> 56% of the pixels).
    int pct = g_canvasPct < 25 ? 25 : g_canvasPct > 100 ? 100 : g_canvasPct;
    int CW = (W * pct / 100) & ~1, CH = (H * pct / 100) & ~1;
    LOG("tile canvas %dx%d (window %dx%d, %d%%)", CW, CH, W, H, pct);
    fprintf(f, "//!HOOK MAIN\n//!BIND HOOKED\n//!WIDTH %d\n//!HEIGHT %d\n//!DESC LiteWall per-monitor layout\n\n", CW, CH);
    fprintf(f, "vec4 lw_tile(vec2 px, vec4 r, float va) {\n"
               "    vec2 l = (px - r.xy) / r.zw; float ma = r.z / r.w; vec2 uv = l;\n");
    if (g_fill) fprintf(f, "    if (ma > va) uv.y = 0.5 + (l.y - 0.5) * (va / ma); else uv.x = 0.5 + (l.x - 0.5) * (ma / va);\n");
    else fprintf(f, "    if (ma > va) uv.x = 0.5 + (l.x - 0.5) * (ma / va); else uv.y = 0.5 + (l.y - 0.5) * (va / ma);\n"
                    "    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return vec4(0.0, 0.0, 0.0, 1.0);\n");
    fprintf(f, "    return HOOKED_tex(uv);\n}\n\nvec4 hook() {\n    vec2 px = HOOKED_pos * vec2(%d.0, %d.0);\n"
               "    float va = HOOKED_size.x / HOOKED_size.y;\n", W, H);
    for (int i = 0; i < g_nscr; i++) if (g_scr[i].grp == grp) {
        RECT m = g_scr[i].rc;
        int x = m.left - box->left, y = m.top - box->top, w = m.right - m.left, h = m.bottom - m.top;
        fprintf(f, "    if (px.x >= %d.0 && px.x < %d.0 && px.y >= %d.0 && px.y < %d.0) return lw_tile(px, vec4(%d.0, %d.0, %d.0, %d.0), va);\n",
                x, x + w, y, y + h, x, y, w, h);
        LOG("tile: monitor %d -> canvas rect (%d,%d) %dx%d of %dx%d", i, x, y, w, h, W, H);
    }
    fprintf(f, "    return vec4(0.0, 0.0, 0.0, 1.0);\n}\n");
    fclose(f);
    return path;
}

static void create_screens(void) {
    destroy_screens();
    if (g_noVideo) { LOG("--no-video: not creating wallpaper"); return; }
    int any = g_file[0] != 0; for (int i = 0; i < MAX_MON; i++) any |= g_monFile[i][0] != 0;
    if (!any) return;
    HWND parent = get_wallpaper_parent();
    { wchar_t c[64] = L""; GetClassNameW(parent, c, 64); RECT pr; GetWindowRect(parent, &pr);
      LOG("wallpaper host hwnd=%p class=%ls rect=(%ld,%ld)-(%ld,%ld)", (void *)parent, c, pr.left, pr.top, pr.right, pr.bottom); }
    EnumDisplayMonitors(NULL, NULL, enum_mon, 0);
    g_mode = g_spanSetting ? MODE_SPAN : MODE_TILE;
    if (wcsstr(GetCommandLineW(), L"--span")) g_mode = MODE_SPAN;
    else if (wcsstr(GetCommandLineW(), L"--per-monitor")) g_mode = MODE_PERMON;
    else if (wcsstr(GetCommandLineW(), L"--tile")) g_mode = MODE_TILE;
    LOG("mode=%s monitors=%d", g_mode == MODE_SPAN ? "span" : g_mode == MODE_TILE ? "tile (1 player, GPU shader per-monitor layout)" : "per-monitor", g_nscr);
    // 1) group monitors by file (per-monitor mode: every monitor is its own group)
    for (int i = 0; i < g_nscr; i++) {
        Screen *s = &g_scr[i]; const wchar_t *f = screen_file(i);
        s->wr = s->rc;
        LOG("monitor %d rect=(%ld,%ld)-(%ld,%ld) %ldx%ld file=%ls", i, s->rc.left, s->rc.top, s->rc.right, s->rc.bottom,
            s->rc.right - s->rc.left, s->rc.bottom - s->rc.top, f);
        if (!f[0]) continue;
        int g = -1;
        if (g_mode != MODE_PERMON) for (int k = 0; k < g_ngrp; k++) if (!_wcsicmp(g_grp[k].file, f)) { g = k; break; }
        if (g < 0 && g_ngrp < MAX_GRP) { g = g_ngrp++; wcscpy(g_grp[g].file, f); g_grp[g].paused = -1; }
        s->grp = g;
    }
    // 2) windows: one per monitor, or (span) one per group covering the union of its monitors
    HWND gwnd[MAX_GRP] = {0};
    g_maxW = g_maxH = 0;
    if (g_mode != MODE_SPAN)   // optimize target: big enough to cover the largest monitor (physical pixels)
        for (int i = 0; i < g_nscr; i++) if (g_scr[i].grp >= 0) {
            int w = g_scr[i].rc.right - g_scr[i].rc.left, h = g_scr[i].rc.bottom - g_scr[i].rc.top;
            if (w > g_maxW) g_maxW = w;
            if (h > g_maxH) g_maxH = h;
        }
    for (int i = 0; i < g_nscr; i++) {
        Screen *s = &g_scr[i];
        if (s->grp < 0) continue;
        if (g_mode != MODE_PERMON) {   // span + tile: one window per group covering all its monitors
            if (gwnd[s->grp]) continue;
            for (int j = 0; j < g_nscr; j++) if (g_scr[j].grp == s->grp) UnionRect(&s->wr, &s->wr, &g_scr[j].rc);
        }
        int w = s->wr.right - s->wr.left, h = s->wr.bottom - s->wr.top;
        if (g_mode == MODE_SPAN) { if (w > g_maxW) g_maxW = w; if (h > g_maxH) g_maxH = h; }
        POINT p = {s->wr.left, s->wr.top};
        MapWindowPoints(HWND_DESKTOP, parent, &p, 1);
        s->hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"LiteWallSurface", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                  p.x, p.y, w, h, parent, NULL, g_inst, NULL);
        if (!s->hwnd) { LOG("CreateWindow failed for monitor %d err=%lu", i, GetLastError()); continue; }
        SetWindowPos(s->hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);  // under the icons (24H2)
        LOG("window for monitor %d (group %d): screen (%ld,%ld) %dx%d -> parent-client (%ld,%ld)", i, s->grp, s->wr.left, s->wr.top, w, h, p.x, p.y);
        if (!gwnd[s->grp]) gwnd[s->grp] = s->hwnd;
    }
    // 3) players: one mpv per window. Optimized copy is made for the largest target, shared by all.
    for (int i = 0; i < g_nscr; i++) {
        Screen *s = &g_scr[i];
        if (!s->hwnd) continue;
        Group *G = &g_grp[s->grp];
        wchar_t shp[MAX_PATH]; const wchar_t *sh = NULL;
        if (g_mode == MODE_TILE) sh = write_tile_shader(s->grp, &s->wr, shp);
        G->mpv = mpv_start(s->hwnd, sh);
        if (G->mpv) mpv_load_group(G, g_maxW, g_maxH);
        LOG("player for monitor %d: %s", i, G->mpv ? "ok" : "FAILED");
    }
    LOG("optimize target = %dx%d (optimize %s)", g_maxW, g_maxH, g_optimize ? "on" : "off");
    opt_kick();
}

/* ---------- diagnostics: memory/CPU sampler ---------- */
static void log_stats(void) {
    PROCESS_MEMORY_COUNTERS_EX pm = {sizeof pm};
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
    FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    ULONGLONG cpu = ((((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 10000;
    static ULONGLONG last; static DWORD lastT;
    DWORD now = GetTickCount(); double pct = lastT ? 100.0 * (double)(cpu - last) / (double)(now - lastT) : 0;
    last = cpu; lastT = now;
    if (g_statTicks >= 10) { g_sumPriv += pm.PrivateUsage / 1048576.0; g_sumCpu += pct; g_nSum++; }
    LOG("STAT t=%ds private=%.1fMB workingset=%.1fMB peakWS=%.1fMB cpu_total=%.2fs cpu_1s=%.1f%%core paused=%d",
        g_statTicks, pm.PrivateUsage / 1048576.0, pm.WorkingSetSize / 1048576.0, pm.PeakWorkingSetSize / 1048576.0, cpu / 1000.0, pct, g_allPaused);
}

static void log_player_props(mpv_handle *m, int g) {
    static const char *props[] = {"hwdec-current", "hwdec-interop", "current-vo", "current-gpu-context", "video-codec",
        "video-params/pixelformat", "video-params/hw-pixelformat", "video-params/w", "video-params/h",
        "video-out-params/w", "video-out-params/h", "container-fps", "estimated-vf-fps", "hwdec", "gpu-api", "vo"};
    for (size_t i = 0; i < sizeof props / sizeof props[0]; i++) {
        char *v = mpv_get_property_string(m, props[i]);
        LOG("group %d prop %s = %s", g, props[i], v ? v : "(n/a)"); mpv_free(v);
    }
}

/* ---------- auto-pause logic ---------- */
static int is_shell_window(HWND w) {
    wchar_t cls[64]; GetClassNameW(w, cls, 64);
    return !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
           !wcscmp(cls, L"Shell_SecondaryTrayWnd") || !wcscmp(cls, L"LiteWallSurface") ||
           !wcscmp(cls, L"Windows.UI.Core.CoreWindow") || !wcscmp(cls, L"MultitaskingViewFrame") ||
           !wcscmp(cls, L"ForegroundStaging") || !wcscmp(cls, L"TaskSwitcherWnd") || !wcscmp(cls, L"XamlExplorerHostIslandWindow_WASDK") || !wcscmp(cls, L"XamlExplorerHostIslandWindow");
}

// Does a visible top-level window cover (fullscreen) or maximize onto this monitor?
typedef struct { RECT mon; int covered, full; wchar_t why[160]; } CoverCtx;
static BOOL CALLBACK cover_enum(HWND w, LPARAM lp) {
    CoverCtx *c = (CoverCtx *)lp;
    if (!IsWindowVisible(w) || IsIconic(w) || is_shell_window(w)) return TRUE;
    LONG ex = GetWindowLongW(w, GWL_EXSTYLE);
    if (ex & WS_EX_TRANSPARENT) return TRUE;                          // click-through overlays (GPU/Steam/Discord)
    if ((ex & WS_EX_NOACTIVATE) && (ex & WS_EX_TOOLWINDOW)) return TRUE;
    { DWORD pid; GetWindowThreadProcessId(w, &pid); if (pid == GetCurrentProcessId()) return TRUE; } // our own windows
    { BYTE a = 255; DWORD fl = 0; if ((ex & WS_EX_LAYERED) && GetLayeredWindowAttributes(w, NULL, &a, &fl) && (fl & LWA_ALPHA) && a < 32) return TRUE; }
    BOOL cloaked = FALSE;
    typedef HRESULT(WINAPI * DwmGetAttr)(HWND, DWORD, PVOID, DWORD);
    static DwmGetAttr dga; static int tried;
    if (!tried) { tried = 1; HMODULE d = LoadLibraryW(L"dwmapi.dll"); if (d) dga = (DwmGetAttr)GetProcAddress(d, "DwmGetWindowAttribute"); }
    if (dga) dga(w, 14 /*DWMWA_CLOAKED*/, &cloaked, sizeof cloaked);
    if (cloaked) return TRUE;
    RECT r; if (!GetWindowRect(w, &r)) return TRUE;
    int full = r.left <= c->mon.left && r.top <= c->mon.top && r.right >= c->mon.right && r.bottom >= c->mon.bottom;
    int maxed = g_pauseOnMaximized && IsZoomed(w) && MonitorFromWindow(w, MONITOR_DEFAULTTONULL) == MonitorFromRect(&c->mon, MONITOR_DEFAULTTONULL);
    if (full || maxed) {
        c->covered = 1; c->full |= full;
        if (g_logf) { wchar_t cls[64], t[64]; GetClassNameW(w, cls, 64); GetWindowTextW(w, t, 64); swprintf(c->why, 160, L"%s by [%s] \"%s\"", full ? L"fullscreen" : L"maximized", cls, t); }
        return FALSE;
    }
    return TRUE;
}

static void apply_pause(void) {
    const char *why = NULL; int game = 0;
    if (g_userPaused) why = "paused from tray";
    else if (g_locked) why = "session locked";
    if (!why && g_pauseOnBattery) {
        SYSTEM_POWER_STATUS ps;
        if (GetSystemPowerStatus(&ps) && ps.ACLineStatus == 0) why = "on battery";
    }
    QUERY_USER_NOTIFICATION_STATE st;
    if (SUCCEEDED(SHQueryUserNotificationState(&st)) && (st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE)) {
        game = 1; if (!why) why = st == QUNS_RUNNING_D3D_FULL_SCREEN ? "exclusive-fullscreen D3D app" : "presentation mode";
    }
    int global = why != NULL;
    int all = 1;
    static char lastWhy[8][200];
    for (int i = 0; i < g_nscr; i++) {
        Screen *s = &g_scr[i];
        CoverCtx c; memset(&c, 0, sizeof c); c.mon = s->rc;
        EnumWindows(cover_enum, (LPARAM)&c);
        game |= c.full;
        int want = global || c.covered;
        if (g_logf && i < 8) {
            char cur[200];
            if (global) snprintf(cur, sizeof cur, "%s", why); else if (c.covered) snprintf(cur, sizeof cur, "%ls", c.why); else cur[0] = 0;
            if (strcmp(cur, lastWhy[i])) { LOG("monitor %d %s%s", i, cur[0] ? "PAUSE: " : "resume", cur); strcpy(lastWhy[i], cur); }
        }
        s->paused = want;
        all &= want;
    }
    g_gameActive = game;   // the optimizer only yields to real fullscreen apps, not battery/lock/maximized
    // A shared player pauses only when every monitor it feeds is paused.
    for (int g = 0; g < g_ngrp; g++) {
        int gp = 1, any = 0; for (int i = 0; i < g_nscr; i++) if (g_scr[i].grp == g) { gp &= g_scr[i].paused == 1; any = 1; }
        if (!any) gp = 0;
        if (gp != g_grp[g].paused && g_grp[g].mpv) { int f = gp; mpv_set_property(g_grp[g].mpv, "pause", MPV_FORMAT_FLAG, &f); g_grp[g].paused = gp; }
    }
    if (!g_nscr) all = 0;
    if (all != g_allPaused) {
        g_allPaused = all;
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

#include "optimizer.c"
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
            players_set_string("panscan", g_fill ? "1.0" : "0.0"); if (g_mode == MODE_TILE) create_screens();
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
        for (int i = 0; i < g_ngrp; i++) if (g_grp[i].mpv == m) {
            for (;;) {
                mpv_event *e = mpv_wait_event(m, 0); if (e->event_id == MPV_EVENT_NONE) break;
                if (!g_logf) continue;
                if (e->event_id == MPV_EVENT_LOG_MESSAGE) {
                    mpv_event_log_message *lm = e->data;
                    LOG("mpv[%d] [%s] %s: %.*s", i, lm->level, lm->prefix, (int)(strlen(lm->text) && lm->text[strlen(lm->text) - 1] == '\n' ? strlen(lm->text) - 1 : strlen(lm->text)), lm->text);
                } else if (e->event_id == MPV_EVENT_VIDEO_RECONFIG || e->event_id == MPV_EVENT_FILE_LOADED) {
                    LOG("group %d event %s", i, mpv_event_name(e->event_id)); log_player_props(m, i);
                } else if (e->event_id == MPV_EVENT_END_FILE) LOG("group %d END_FILE", i);
            }
        }
        return 0;
    }
    case WM_TIMER: if (!surfaces_healthy()) reattach_screens(); apply_pause(); return 0;
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
    if (msg == WM_OPT_DONE) { LOG("optimized copy ready -> reloading players"); create_screens(); apply_pause(); return 0; }
    if (msg == WM_TIMER && wp == 5) { g_statTicks++; log_stats(); if (g_statTicks == 3 || g_statTicks == 15) for (int g = 0; g < g_ngrp; g++) if (g_grp[g].mpv) log_player_props(g_grp[g].mpv, g); if (g_statTicks >= g_statSecs) {
        KillTimer(h, 5);
        LOG("SUMMARY avg_private=%.1fMB avg_cpu_1s=%.1f%%core samples=%d (t>=10s) mode=%d canvas=%d%% fps_cap=%d opts=%s", g_nSum ? g_sumPriv / g_nSum : 0, g_nSum ? g_sumCpu / g_nSum : 0, g_nSum, g_mode, g_canvasPct, g_fpsCap, g_mpvOpts[0] ? g_mpvOpts : "(defaults)");
        LOG("stat sampling done");
        if (g_exitAfterStats) DestroyWindow(g_msg);
    } return 0; }
    if (msg == WM_TIMER && (wp == 3 || wp == 4)) { KillTimer(h, wp); reattach_screens(); apply_pause(); return 0; }
    if (msg == WM_TIMER && wp == 2) { KillTimer(h, 2); create_screens(); apply_pause(); return 0; }
    return msg_proc(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;
    g_inst = inst;
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"LiteWall.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { MessageBoxW(NULL, L"LiteWall is already running (see the tray icon).", APP_NAME, MB_OK | MB_ICONINFORMATION); return 0; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    load_settings();
    {
        const wchar_t *cl = GetCommandLineW();
        g_log = wcsstr(cl, L"--log") != NULL; g_noVideo = wcsstr(cl, L"--no-video") != NULL;
        g_exitAfterStats = wcsstr(cl, L"--exit-after-stats") != NULL;
        g_cliCanvas = g_cliFps = -1;
        int argc; LPWSTR *argv = CommandLineToArgvW(cl, &argc);
        for (int i = 1; argv && i < argc; i++) {
            if (!wcscmp(argv[i], L"--mpv-opts") && i + 1 < argc) WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, g_mpvOpts, sizeof g_mpvOpts, NULL, NULL);
            else if (!wcscmp(argv[i], L"--canvas-scale") && i + 1 < argc) g_cliCanvas = _wtoi(argv[++i]);
            else if (!wcscmp(argv[i], L"--fps-cap") && i + 1 < argc) g_cliFps = _wtoi(argv[++i]);
            else if (!wcscmp(argv[i], L"--stat-secs") && i + 1 < argc) { g_statSecs = _wtoi(argv[++i]); if (g_statSecs < 12) g_statSecs = 12; }
        }
        LocalFree(argv);
        if (g_cliCanvas >= 0) g_canvasPct = g_cliCanvas;   // command-line overrides (not saved... unless settings change)
        if (g_cliFps >= 0) g_fpsCap = g_cliFps;
        if (g_log) {
            wchar_t lp[MAX_PATH]; swprintf(lp, MAX_PATH, L"%s\\litewall.log", g_appdir);
            g_logf = _wfopen(lp, L"w");
            OSVERSIONINFOW ov = {sizeof ov};
            typedef LONG(WINAPI * RtlGV)(OSVERSIONINFOW *); RtlGV rg = (RtlGV)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"); if (rg) rg(&ov);
            LOG("LiteWall 0.3.3  cmdline=%ls", cl);
            LOG("Windows %lu.%lu build %lu; settings file=%ls fill=%d mute=%d speed=%d", ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber, g_file, g_fill, g_mute, g_speedPct);
            DISPLAY_DEVICEW dd = {sizeof dd}; for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); i++) { if (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) LOG("display %ls: %ls", dd.DeviceName, dd.DeviceString); dd.cb = sizeof dd; }
        }
    }

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
    SetTimer(g_msg, ID_TIMER, 1000, NULL);
    if (g_log) { log_stats(); SetTimer(g_msg, 5, 1000, NULL); }  // 1 Hz check - negligible cost

    // Watch the registry value Explorer updates on every virtual-desktop switch (zero idle cost).
    HKEY vdk = NULL; HANDLE vdev = CreateEventW(NULL, FALSE, FALSE, NULL);
    RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops", 0, NULL, 0, KEY_NOTIFY | KEY_READ, NULL, &vdk, NULL);
    if (vdk) RegNotifyChangeKeyValue(vdk, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME, vdev, TRUE);
    MSG m;
    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(vdk ? 1 : 0, &vdev, FALSE, INFINITE, QS_ALLINPUT);
        if (vdk && r == WAIT_OBJECT_0) {
            RegNotifyChangeKeyValue(vdk, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME, vdev, TRUE);
            reattach_screens();                         // immediately
            SetTimer(g_msg, 3, 400, NULL);              // after the switch animation
            SetTimer(g_msg, 4, 1500, NULL);             // and once more once Explorer settles
            continue;
        }
        int quit = 0;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { quit = 1; break; }
            TranslateMessage(&m); DispatchMessageW(&m);
        }
        if (quit) break;
    }
    if (vdk) RegCloseKey(vdk);
    CloseHandle(vdev);
    ReleaseMutex(mutex);
    return 0;
}

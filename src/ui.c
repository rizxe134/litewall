// LiteWall library window: dark native Win32 UI (ListView grid + details panel).
// Included from litewall.c. Everything here is created on open and fully freed on close.
#define COBJMACROS
#include <objbase.h>
#include <wincodec.h>
#include <uxtheme.h>
#include <dwmapi.h>

#define WM_THUMB_DONE (WM_APP + 10)
#define TW 192
#define TH 108
#define PW 300
#define PH 169
enum { IDC_LIST = 1000, IDC_SEARCH, IDC_ADD, IDC_PREVIEW, IDC_NAME, IDC_RES, IDC_MON, IDC_APPLY, IDC_REMOVE,
       IDC_FILL, IDC_PMAX, IDC_PBAT, IDC_START, IDC_MUTE, IDC_SPEED, IDC_HDR1, IDC_HDR2, IDC_SPEEDLBL, IDC_MONLBL, IDC_OPT, IDC_SPAN, IDC_FPS, IDC_FPSLBL, IDC_CANVAS, IDC_CANVASLBL,
       IDC_ROT, IDC_ROTLBL, IDC_EVERY, IDC_EVERYLBL, IDC_INCL, IDC_NEXT };

typedef struct { wchar_t path[MAX_PATH * 2]; wchar_t name[MAX_PATH]; int w, h, img, ready; } Item;

static HWND g_ui, g_list, g_preview;
static HIMAGELIST g_il;
static Item *g_items; static int g_nitems;
static HBITMAP g_prevBmp;
static HBRUSH g_brBg, g_brPanel, g_brEdit;
static HFONT g_font, g_fontBig;
static volatile LONG g_uiGen;       // bumped on every open/close so stale thumbnail jobs are ignored
static const COLORREF C_BG = RGB(24, 24, 28), C_PANEL = RGB(34, 34, 40), C_TEXT = RGB(235, 235, 240),
                      C_DIM = RGB(150, 150, 160), C_ACCENT = RGB(0, 120, 215);

static void lib_dir(wchar_t *out) { swprintf(out, MAX_PATH, L"%s\\library", g_appdir); CreateDirectoryW(out, NULL); }
static void thumb_dir(wchar_t *out) { swprintf(out, MAX_PATH, L"%s\\thumbs", g_appdir); CreateDirectoryW(out, NULL); }
static unsigned fnv(const wchar_t *s) { unsigned h = 2166136261u; for (; *s; s++) { h ^= (unsigned)towlower(*s); h *= 16777619u; } return h; }
static void thumb_paths(const wchar_t *file, wchar_t *png, wchar_t *meta) {
    wchar_t d[MAX_PATH]; thumb_dir(d); unsigned h = fnv(file);
    WIN32_FILE_ATTRIBUTE_DATA fa; unsigned sz = 0;
    if (GetFileAttributesExW(file, GetFileExInfoStandard, &fa)) sz = fa.nFileSizeLow ^ fa.ftLastWriteTime.dwLowDateTime;
    if (png) swprintf(png, MAX_PATH, L"%s\\%08x%08x.png", d, h, sz);
    if (meta) swprintf(meta, MAX_PATH, L"%s\\%08x%08x.ini", d, h, sz);
}

/* ---------- WIC: load PNG/JPG/... into a letterboxed 32-bit DIB of box size ---------- */
static HBITMAP wic_load(const wchar_t *file, int bw, int bh) {
    IWICImagingFactory *f = NULL; IWICBitmapDecoder *dec = NULL; IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *cv = NULL; IWICBitmapScaler *sc = NULL; HBITMAP bmp = NULL;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f))) return NULL;
    if (FAILED(IWICImagingFactory_CreateDecoderFromFilename(f, file, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) goto done;
    if (FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &fr))) goto done;
    UINT w, h; IWICBitmapFrameDecode_GetSize(fr, &w, &h);
    if (!w || !h) goto done;
    double k = (double)bw / w; if ((double)bh / h < k) k = (double)bh / h;
    UINT sw = (UINT)(w * k + 0.5), sh = (UINT)(h * k + 0.5); if (!sw) sw = 1; if (!sh) sh = 1;
    IWICImagingFactory_CreateFormatConverter(f, &cv);
    if (FAILED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))) goto done;
    IWICImagingFactory_CreateBitmapScaler(f, &sc);
    if (FAILED(IWICBitmapScaler_Initialize(sc, (IWICBitmapSource *)cv, sw, sh, WICBitmapInterpolationModeFant))) goto done;
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), bw, -bh, 1, 32, BI_RGB}};
    void *bits; bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) goto done;
    DWORD *px = bits; for (int i = 0; i < bw * bh; i++) px[i] = 0xFF101014;
    BYTE *tmp = malloc(sw * sh * 4);
    if (tmp && SUCCEEDED(IWICBitmapScaler_CopyPixels(sc, NULL, sw * 4, sw * sh * 4, tmp))) {
        int ox = (bw - (int)sw) / 2, oy = (bh - (int)sh) / 2;
        for (UINT y = 0; y < sh; y++) memcpy(px + (oy + y) * bw + ox, tmp + y * sw * 4, sw * 4);
        for (int i = 0; i < bw * bh; i++) px[i] |= 0xFF000000;
    }
    free(tmp);
done:
    if (sc) IWICBitmapScaler_Release(sc); if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr); if (dec) IWICBitmapDecoder_Release(dec);
    IWICImagingFactory_Release(f);
    return bmp;
}

/* ---------- thumbnail generation with libmpv (vo=image, first frame) ---------- */
static int make_thumb(const wchar_t *file, const wchar_t *png, const wchar_t *meta) {
    wchar_t tdir[MAX_PATH]; thumb_dir(tdir);
    wchar_t odir[MAX_PATH]; swprintf(odir, MAX_PATH, L"%s\\tmp%lu", tdir, GetCurrentThreadId());
    CreateDirectoryW(odir, NULL);
    mpv_handle *m = mpv_create(); if (!m) return 0;
    char u[MAX_PATH * 6], od[MAX_PATH * 6]; to_utf8(file, u, sizeof u); to_utf8(odir, od, sizeof od);
    static const char *o[][2] = {{"config", "no"}, {"terminal", "no"}, {"msg-level", "all=no"}, {"vo", "image"},
        {"vo-image-format", "png"}, {"vo-image-png-compression", "1"}, {"frames", "1"}, {"aid", "no"}, {"sid", "no"},
        {"hwdec", "no"}, {"vf", "scale=w=480:h=270:force_original_aspect_ratio=decrease"}, {"load-scripts", "no"},
        {"ytdl", "no"}, {"image-display-duration", "0"}, {"keep-open", "no"}, {"idle", "no"}};
    for (size_t i = 0; i < sizeof o / sizeof o[0]; i++) mpv_set_option_string(m, o[i][0], o[i][1]);
    mpv_set_option_string(m, "vo-image-outdir", od);
    int ok = 0; int64_t w = 0, h = 0;
    if (mpv_initialize(m) >= 0) {
        const char *cmd[] = {"loadfile", u, NULL}; mpv_command(m, cmd);
        DWORD t0 = GetTickCount();
        for (;;) {
            mpv_event *e = mpv_wait_event(m, 1.0);
            if (e->event_id == MPV_EVENT_VIDEO_RECONFIG || e->event_id == MPV_EVENT_FILE_LOADED) {
                mpv_get_property(m, "width", MPV_FORMAT_INT64, &w); mpv_get_property(m, "height", MPV_FORMAT_INT64, &h);
            }
            if (e->event_id == MPV_EVENT_END_FILE || e->event_id == MPV_EVENT_SHUTDOWN) break;
            if (GetTickCount() - t0 > 15000) break;
        }
    }
    mpv_terminate_destroy(m);
    wchar_t out[MAX_PATH]; swprintf(out, MAX_PATH, L"%s\\00000001.png", odir);
    if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) { DeleteFileW(png); ok = MoveFileW(out, png); }
    wchar_t b[32]; swprintf(b, 32, L"%lld", (long long)w); WritePrivateProfileStringW(L"m", L"w", b, meta);
    swprintf(b, 32, L"%lld", (long long)h); WritePrivateProfileStringW(L"m", L"h", b, meta);
    // clean any extra frames
    wchar_t pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%s\\*", odir); WIN32_FIND_DATAW fd; HANDLE fh = FindFirstFileW(pat, &fd);
    if (fh != INVALID_HANDLE_VALUE) { do { wchar_t x[MAX_PATH]; swprintf(x, MAX_PATH, L"%s\\%s", odir, fd.cFileName); DeleteFileW(x); } while (FindNextFileW(fh, &fd)); FindClose(fh); }
    RemoveDirectoryW(odir);
    return ok;
}

typedef struct { wchar_t path[MAX_PATH * 2]; int idx; LONG gen; } Job;
static DWORD WINAPI thumb_thread(LPVOID p) {
    Job *j = p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    wchar_t png[MAX_PATH], meta[MAX_PATH]; thumb_paths(j->path, png, meta);
    if (GetFileAttributesW(png) == INVALID_FILE_ATTRIBUTES) make_thumb(j->path, png, meta);
    if (j->gen == g_uiGen && g_ui) PostMessageW(g_ui, WM_THUMB_DONE, j->idx, j->gen);
    free(j); return 0;
}
// Run one worker that processes the whole queue sequentially (keeps CPU use modest).
typedef struct { LONG gen; int n; wchar_t (*paths)[MAX_PATH * 2]; } Batch;
static DWORD WINAPI batch_thread(LPVOID p) {
    Batch *b = p;
    for (int i = 0; i < b->n && b->gen == g_uiGen; i++) {
        Job *j = malloc(sizeof *j); wcscpy(j->path, b->paths[i]); j->idx = i; j->gen = b->gen; thumb_thread(j);
    }
    free(b->paths); free(b); return 0;
}

/* ---------- library ---------- */
static int is_media(const wchar_t *n) {
    const wchar_t *e = wcsrchr(n, L'.'); if (!e) return 0;
    static const wchar_t *x[] = {L".mp4", L".webm", L".mkv", L".mov", L".avi", L".m4v", L".wmv", L".gif", L".png", L".jpg", L".jpeg", L".bmp", L".webp"};
    for (size_t i = 0; i < sizeof x / sizeof x[0]; i++) if (!_wcsicmp(e, x[i])) return 1;
    return 0;
}
static void lib_scan(void) {
    free(g_items); g_items = NULL; g_nitems = 0;
    wchar_t d[MAX_PATH], pat[MAX_PATH]; lib_dir(d); swprintf(pat, MAX_PATH, L"%s\\*", d);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd); int cap = 0;
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || !is_media(fd.cFileName)) continue;
        if (g_nitems == cap) { cap = cap ? cap * 2 : 32; g_items = realloc(g_items, cap * sizeof(Item)); }
        Item *it = &g_items[g_nitems++]; memset(it, 0, sizeof *it); it->img = -1;
        swprintf(it->path, MAX_PATH * 2, L"%s\\%s", d, fd.cFileName);
        wcsncpy(it->name, fd.cFileName, MAX_PATH - 1); wchar_t *dot = wcsrchr(it->name, L'.'); if (dot) *dot = 0;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static int lib_import(const wchar_t *src) {
    if (!is_media(src)) return 0;
    wchar_t d[MAX_PATH], dst[MAX_PATH * 2]; lib_dir(d);
    const wchar_t *base = wcsrchr(src, L'\\'); base = base ? base + 1 : src;
    swprintf(dst, MAX_PATH * 2, L"%s\\%s", d, base);
    if (!_wcsicmp(src, dst)) return 1;
    for (int n = 2; GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES && n < 100; n++) {
        wchar_t stem[MAX_PATH]; wcsncpy(stem, base, MAX_PATH - 1); stem[MAX_PATH - 1] = 0;
        wchar_t *dot = wcsrchr(stem, L'.'); const wchar_t *ext = wcsrchr(base, L'.'); if (dot) *dot = 0;
        swprintf(dst, MAX_PATH * 2, L"%s\\%s (%d)%s", d, stem, n, ext ? ext : L"");
    }
    return CopyFileW(src, dst, TRUE);
}

/* ---------- UI helpers ---------- */
static int sel_index(void) {
    int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED); if (i < 0) return -1;
    LVITEMW it = {LVIF_PARAM, i}; ListView_GetItem(g_list, &it); return (int)it.lParam;
}
static void fill_list(void) {
    wchar_t q[128] = L""; GetDlgItemTextW(g_ui, IDC_SEARCH, q, 128); _wcslwr(q);
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0); ListView_DeleteAllItems(g_list);
    for (int i = 0, row = 0; i < g_nitems; i++) {
        wchar_t lo[MAX_PATH]; wcscpy(lo, g_items[i].name); _wcslwr(lo);
        if (q[0] && !wcsstr(lo, q)) continue;
        LVITEMW it = {LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE, row++}; it.pszText = g_items[i].name; it.lParam = i; it.iImage = g_items[i].img >= 0 ? g_items[i].img : 0;
        ListView_InsertItem(g_list, &it);
    }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0); InvalidateRect(g_list, NULL, TRUE);
}
static void update_panel(void) {
    int i = sel_index();
    if (g_prevBmp) { DeleteObject(g_prevBmp); g_prevBmp = NULL; }
    if (i < 0) { SetDlgItemTextW(g_ui, IDC_NAME, L"Select a wallpaper"); SetDlgItemTextW(g_ui, IDC_RES, L""); }
    else {
        Item *it = &g_items[i]; wchar_t png[MAX_PATH], meta[MAX_PATH]; thumb_paths(it->path, png, meta);
        g_prevBmp = wic_load(png, PW, PH);
        SetDlgItemTextW(g_ui, IDC_NAME, it->name);
        int w = GetPrivateProfileIntW(L"m", L"w", 0, meta), h = GetPrivateProfileIntW(L"m", L"h", 0, meta);
        const wchar_t *e = wcsrchr(it->path, L'.'); wchar_t b[96];
        if (w) swprintf(b, 96, L"%d \u00d7 %d   \u2022   %s", w, h, e ? e + 1 : L""); else swprintf(b, 96, L"%s", e ? e + 1 : L"");
        SetDlgItemTextW(g_ui, IDC_RES, b);
    }
    CheckDlgButton(g_ui, IDC_INCL, i >= 0 && rot_included(g_items[i].path)); EnableWindow(GetDlgItem(g_ui, IDC_INCL), i >= 0);
    EnableWindow(GetDlgItem(g_ui, IDC_APPLY), i >= 0); EnableWindow(GetDlgItem(g_ui, IDC_REMOVE), i >= 0);
    InvalidateRect(g_preview, NULL, FALSE);
}
static void start_thumbs(void) {
    Batch *b = malloc(sizeof *b); b->gen = g_uiGen; b->n = g_nitems;
    b->paths = malloc((g_nitems ? g_nitems : 1) * sizeof *b->paths);
    for (int i = 0; i < g_nitems; i++) wcscpy(b->paths[i], g_items[i].path);
    HANDLE t = CreateThread(NULL, 0, batch_thread, b, 0, NULL); if (t) CloseHandle(t); else { free(b->paths); free(b); }
}
static void reload_library(void) {
    g_uiGen++;
    ImageList_RemoveAll(g_il);
    HBITMAP ph = CreateBitmap(TW, TH, 1, 32, NULL); { HDC dc = CreateCompatibleDC(NULL); HGDIOBJ o = SelectObject(dc, ph); RECT r = {0, 0, TW, TH}; HBRUSH br = CreateSolidBrush(RGB(16, 16, 20)); FillRect(dc, &r, br); DeleteObject(br); SelectObject(dc, o); DeleteDC(dc); }
    ImageList_Add(g_il, ph, NULL); DeleteObject(ph);   // index 0 = placeholder
    lib_scan(); fill_list(); update_panel(); start_thumbs();
}
static void fill_monitors(void) {
    HWND c = GetDlgItem(g_ui, IDC_MON); SendMessageW(c, CB_RESETCONTENT, 0, 0);
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"All monitors");
    int n = g_nscr; if (!n) n = GetSystemMetrics(SM_CMONITORS);
    for (int i = 0; i < n && i < MAX_MON; i++) {
        wchar_t b[64];
        if (i < g_nscr) swprintf(b, 64, L"Monitor %d  (%ld\u00d7%ld)", i + 1, g_scr[i].rc.right - g_scr[i].rc.left, g_scr[i].rc.bottom - g_scr[i].rc.top);
        else swprintf(b, 64, L"Monitor %d", i + 1);
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)b);
    }
    SendMessageW(c, CB_SETCURSEL, 0, 0);
}
static void sync_settings_ui(void) {
    CheckDlgButton(g_ui, IDC_FILL, g_fill); CheckDlgButton(g_ui, IDC_PMAX, g_pauseOnMaximized);
    CheckDlgButton(g_ui, IDC_PBAT, g_pauseOnBattery); CheckDlgButton(g_ui, IDC_START, startup_enabled()); CheckDlgButton(g_ui, IDC_MUTE, g_mute);
    SendDlgItemMessageW(g_ui, IDC_FPS, CB_SETCURSEL, g_fpsCap == 30 ? 0 : g_fpsCap == 60 ? 1 : 2, 0);
    SendDlgItemMessageW(g_ui, IDC_CANVAS, CB_SETCURSEL, g_canvasPct >= 100 ? 0 : g_canvasPct >= 75 ? 1 : 2, 0);
    SendDlgItemMessageW(g_ui, IDC_ROT, CB_SETCURSEL, g_rotMode, 0); SendDlgItemMessageW(g_ui, IDC_EVERY, CB_SETCURSEL, g_rotEvery, 0);
    CheckDlgButton(g_ui, IDC_OPT, g_optimize); CheckDlgButton(g_ui, IDC_SPAN, g_spanSetting);
    static const int sp[] = {25, 50, 75, 100, 125, 150, 200}; HWND c = GetDlgItem(g_ui, IDC_SPEED);
    for (int i = 0; i < 7; i++) if (sp[i] == g_speedPct) SendMessageW(c, CB_SETCURSEL, i, 0);
}

static HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD st, int id) {
    HWND w = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | st, 0, 0, 10, 10, g_ui, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(w, WM_SETFONT, (WPARAM)g_font, FALSE); return w;
}
static void layout(void) {
    RECT r; GetClientRect(g_ui, &r); int W = r.right, H = r.bottom, P = 340, m = 16;
    HDWP d = BeginDeferWindowPos(42);
#define MV(id, x, y, w, h) d = DeferWindowPos(d, GetDlgItem(g_ui, id), NULL, x, y, w, h, SWP_NOZORDER)
    MV(IDC_SEARCH, m, m, 280, 28); MV(IDC_ADD, m + 292, m, 150, 28);
    MV(IDC_LIST, m, m + 44, W - P - 2 * m, H - m - 44 - m);
    int x = W - P + 4, y = m, pw = P - 24;
    MV(IDC_PREVIEW, x, y, PW, PH); y += PH + 12;
    MV(IDC_NAME, x, y, pw, 26); y += 28; MV(IDC_RES, x, y, pw, 20); y += 30;
    MV(IDC_MONLBL, x, y, pw, 18); y += 20; MV(IDC_MON, x, y, pw, 200); y += 34;
    MV(IDC_APPLY, x, y, 190, 32); MV(IDC_REMOVE, x + 200, y, pw - 200, 32); y += 40;
    MV(IDC_INCL, x, y, pw, 22); y += 34;
    MV(IDC_HDR2, x, y, pw, 20); y += 26;
    MV(IDC_FILL, x, y, pw, 22); y += 26; MV(IDC_PMAX, x, y, pw, 22); y += 26; MV(IDC_PBAT, x, y, pw, 22); y += 26;
    MV(IDC_START, x, y, pw, 22); y += 26; MV(IDC_MUTE, x, y, pw, 22); y += 26;
    MV(IDC_OPT, x, y, pw, 22); y += 26; MV(IDC_SPAN, x, y, pw, 22); y += 32;
    MV(IDC_SPEEDLBL, x, y + 4, 110, 20); MV(IDC_SPEED, x + 115, y, 100, 200); y += 32;
    MV(IDC_FPSLBL, x, y + 4, 110, 20); MV(IDC_FPS, x + 115, y, 100, 200); y += 32;
    MV(IDC_CANVASLBL, x, y + 4, 110, 20); MV(IDC_CANVAS, x + 115, y, 170, 200); y += 32;
    MV(IDC_ROTLBL, x, y + 4, 110, 20); MV(IDC_ROT, x + 115, y, 100, 200); MV(IDC_NEXT, x + 225, y, pw - 225, 26); y += 32;
    MV(IDC_EVERYLBL, x, y + 4, 110, 20); MV(IDC_EVERY, x + 115, y, 170, 300);
#undef MV
    EndDeferWindowPos(d);
}
static void apply_selected(void) {
    int i = sel_index(); if (i < 0) return;
    int mon = (int)SendDlgItemMessageW(g_ui, IDC_MON, CB_GETCURSEL, 0, 0);
    if (mon <= 0) { wcscpy(g_file, g_items[i].path); memset(g_monFile, 0, sizeof g_monFile); }
    else { wcscpy(g_monFile[mon - 1], g_items[i].path); if (!g_file[0]) { /* other monitors stay empty */ } }
    save_settings(); create_screens(); apply_pause();
}
static void add_files_dialog(void) {
    wchar_t *buf = calloc(32768, sizeof(wchar_t));
    OPENFILENAMEW o = {sizeof o}; o.hwndOwner = g_ui; o.lpstrFile = buf; o.nMaxFile = 32768;
    o.lpstrFilter = L"Wallpapers (video, GIF, image)\0*.mp4;*.webm;*.mkv;*.mov;*.avi;*.m4v;*.wmv;*.gif;*.png;*.jpg;*.jpeg;*.bmp;*.webp\0";
    o.lpstrTitle = L"Add wallpapers"; o.Flags = OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&o)) {
        wchar_t *dir = buf, *f = buf + wcslen(buf) + 1;
        if (!*f) lib_import(dir);
        for (; *f; f += wcslen(f) + 1) { wchar_t full[MAX_PATH * 2]; swprintf(full, MAX_PATH * 2, L"%s\\%s", dir, f); lib_import(full); }
        reload_library(); opt_kick();
    }
    free(buf);
}

static LRESULT CALLBACK ui_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_ui = h;
        BOOL dark = TRUE; DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
        g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_fontBig = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_brBg = CreateSolidBrush(C_BG); g_brPanel = CreateSolidBrush(C_PANEL); g_brEdit = CreateSolidBrush(RGB(45, 45, 52));
        HWND s = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_SEARCH); SendMessageW(s, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search wallpapers...");
        SetWindowTheme(s, L"DarkMode_CFD", NULL);
        mk(L"BUTTON", L"+  Add wallpaper", BS_OWNERDRAW | WS_TABSTOP, IDC_ADD);
        g_list = mk(WC_LISTVIEWW, L"", LVS_ICON | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_AUTOARRANGE | WS_TABSTOP, IDC_LIST);
        SetWindowTheme(g_list, L"DarkMode_Explorer", NULL);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(g_list, C_BG); ListView_SetTextBkColor(g_list, C_BG); ListView_SetTextColor(g_list, C_TEXT);
        g_il = ImageList_Create(TW, TH, ILC_COLOR32, 16, 16); ListView_SetImageList(g_list, g_il, LVSIL_NORMAL);
        ListView_SetIconSpacing(g_list, TW + 20, TH + 44);
        g_preview = mk(L"STATIC", L"", SS_OWNERDRAW, IDC_PREVIEW);
        SendMessageW(mk(L"STATIC", L"", SS_ENDELLIPSIS, IDC_NAME), WM_SETFONT, (WPARAM)g_fontBig, FALSE);
        mk(L"STATIC", L"", 0, IDC_RES);
        mk(L"STATIC", L"Apply to", 0, IDC_MONLBL);
        SetWindowTheme(mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_MON), L"DarkMode_CFD", NULL);
        mk(L"BUTTON", L"Apply", BS_OWNERDRAW | WS_TABSTOP, IDC_APPLY);
        mk(L"BUTTON", L"Remove", BS_OWNERDRAW | WS_TABSTOP, IDC_REMOVE);
        SendMessageW(mk(L"STATIC", L"SETTINGS", 0, IDC_HDR2), WM_SETFONT, (WPARAM)g_font, FALSE);
        const struct { int id; const wchar_t *t; } cb[] = {{IDC_FILL, L"Fill screen (crop to fit)"}, {IDC_PMAX, L"Pause when a window is maximized"},
            {IDC_PBAT, L"Pause on battery"}, {IDC_START, L"Start with Windows"}, {IDC_MUTE, L"Mute audio"},
            {IDC_OPT, L"Optimize videos for my monitors (less RAM)"}, {IDC_SPAN, L"Span one wallpaper across monitors"}};
        for (int i = 0; i < 7; i++) SetWindowTheme(mk(L"BUTTON", cb[i].t, BS_AUTOCHECKBOX | WS_TABSTOP, cb[i].id), L"", L""); // unthemed so dark colors apply
        mk(L"STATIC", L"Frame rate", 0, IDC_FPSLBL);
        { HWND c = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_FPS); SetWindowTheme(c, L"DarkMode_CFD", NULL);
          SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"30 fps"); SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"60 fps"); SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Source"); }
        mk(L"STATIC", L"Multi-monitor quality", 0, IDC_CANVASLBL);
        { HWND c = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_CANVAS); SetWindowTheme(c, L"DarkMode_CFD", NULL);
          SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Full (100%)"); SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Balanced (75%)"); SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Light (50%)"); }
        SetWindowTheme(mk(L"BUTTON", L"Include in rotation", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_INCL), L"", L"");
        mk(L"STATIC", L"Rotation", 0, IDC_ROTLBL);
        { HWND c = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_ROT); SetWindowTheme(c, L"DarkMode_CFD", NULL);
          const wchar_t *t[] = {L"Off", L"In order", L"Shuffle", L"Random"}; for (int i = 0; i < 4; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)t[i]); }
        mk(L"BUTTON", L"Next \u25B6", BS_OWNERDRAW | WS_TABSTOP, IDC_NEXT);
        mk(L"STATIC", L"Change every", 0, IDC_EVERYLBL);
        { HWND c = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_EVERY); SetWindowTheme(c, L"DarkMode_CFD", NULL);
          const wchar_t *t[] = {L"After each loop", L"1 minute", L"5 minutes", L"15 minutes", L"30 minutes", L"1 hour", L"1 day", L"On startup only"};
          for (int i = 0; i < 8; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)t[i]); }
        mk(L"STATIC", L"Playback speed", 0, IDC_SPEEDLBL);
        HWND sp = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_SPEED); SetWindowTheme(sp, L"DarkMode_CFD", NULL);
        const wchar_t *spt[] = {L"0.25\u00d7", L"0.5\u00d7", L"0.75\u00d7", L"1\u00d7", L"1.25\u00d7", L"1.5\u00d7", L"2\u00d7"};
        for (int i = 0; i < 7; i++) SendMessageW(sp, CB_ADDSTRING, 0, (LPARAM)spt[i]);
        DragAcceptFiles(h, TRUE);
        layout(); fill_monitors(); sync_settings_ui(); reload_library();
        SetTimer(h, 7, 700, NULL);
        return 0;
    }
    case WM_TIMER: if (wp == 7) {   // optimization progress under the thumbnail
        static int lastRow = -1; int row = -1; wchar_t t[MAX_PATH + 32];
        for (int r = 0, n = ListView_GetItemCount(g_list); r < n; r++) {
            LVITEMW li = {LVIF_PARAM, r}; ListView_GetItem(g_list, &li);
            Item *it = &g_items[li.lParam];
            if (g_optPath[0] && !_wcsicmp(it->path, g_optPath)) {
                row = r; swprintf(t, MAX_PATH + 32, L"%s\n\u2699 Optimizing %ld%%", it->name, g_optPct < 0 ? 0L : (long)g_optPct);
                ListView_SetItemText(g_list, r, 0, t);
            }
        }
        if (lastRow >= 0 && lastRow != row && lastRow < ListView_GetItemCount(g_list)) {
            LVITEMW li = {LVIF_PARAM, lastRow}; ListView_GetItem(g_list, &li); ListView_SetItemText(g_list, lastRow, 0, g_items[li.lParam].name);
        }
        lastRow = row;
    } return 0;
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO *)lp)->ptMinTrackSize.x = 860; ((MINMAXINFO *)lp)->ptMinTrackSize.y = 860; return 0;
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(h, &r); FillRect((HDC)wp, &r, g_brBg);
        RECT p = r; p.left = r.right - 340 - 8; FillRect((HDC)wp, &p, g_brPanel); return 1;
    }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: {
        HWND c = (HWND)lp; int id = GetDlgCtrlID(c);
        SetTextColor((HDC)wp, (id == IDC_RES || id == IDC_HDR2 || id == IDC_MONLBL || id == IDC_SPEEDLBL || id == IDC_ROTLBL || id == IDC_EVERYLBL) ? C_DIM : C_TEXT);
        SetBkColor((HDC)wp, C_PANEL); return (LRESULT)g_brPanel;
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, C_TEXT); SetBkColor((HDC)wp, RGB(45, 45, 52)); return (LRESULT)g_brEdit;
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        if (di->CtlID == IDC_PREVIEW) {
            HBRUSH b = CreateSolidBrush(RGB(16, 16, 20)); FillRect(di->hDC, &di->rcItem, b); DeleteObject(b);
            if (g_prevBmp) { HDC m = CreateCompatibleDC(di->hDC); HGDIOBJ o = SelectObject(m, g_prevBmp); BitBlt(di->hDC, 0, 0, PW, PH, m, 0, 0, SRCCOPY); SelectObject(m, o); DeleteDC(m); }
            return TRUE;
        }
        int dis = di->itemState & ODS_DISABLED, prim = di->CtlID == IDC_APPLY || di->CtlID == IDC_ADD;
        COLORREF bg = dis ? RGB(55, 55, 60) : prim ? ((di->itemState & ODS_SELECTED) ? RGB(0, 95, 180) : C_ACCENT)
                                                   : ((di->itemState & ODS_SELECTED) ? RGB(120, 40, 40) : RGB(60, 60, 68));
        HBRUSH b = CreateSolidBrush(bg); HPEN pen = CreatePen(PS_SOLID, 1, bg);
        HGDIOBJ ob = SelectObject(di->hDC, b), op = SelectObject(di->hDC, pen);
        FillRect(di->hDC, &di->rcItem, (di->CtlID == IDC_ADD) ? g_brBg : g_brPanel);
        RoundRect(di->hDC, di->rcItem.left, di->rcItem.top, di->rcItem.right, di->rcItem.bottom, 8, 8);
        SelectObject(di->hDC, ob); SelectObject(di->hDC, op); DeleteObject(b); DeleteObject(pen);
        wchar_t t[64]; GetWindowTextW(di->hwndItem, t, 64); SetBkMode(di->hDC, TRANSPARENT);
        SetTextColor(di->hDC, dis ? C_DIM : RGB(255, 255, 255)); SelectObject(di->hDC, g_font);
        DrawTextW(di->hDC, t, -1, &di->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return TRUE;
    }
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->idFrom == IDC_LIST) {
            if (n->code == LVN_ITEMCHANGED && (((NMLISTVIEW *)lp)->uChanged & LVIF_STATE)) update_panel();
            else if (n->code == NM_DBLCLK) apply_selected();
            else if (n->code == NM_CUSTOMDRAW) {
                NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lp;
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) { cd->clrText = C_TEXT; cd->clrTextBk = C_BG; return CDRF_DODEFAULT; }
            } else if (n->code == LVN_KEYDOWN && ((NMLVKEYDOWN *)lp)->wVKey == VK_DELETE) PostMessageW(h, WM_COMMAND, IDC_REMOVE, 0);
        }
        return 0;
    }
    case WM_THUMB_DONE: {
        if ((LONG)lp != g_uiGen || (int)wp >= g_nitems) return 0;
        Item *it = &g_items[wp]; wchar_t png[MAX_PATH]; thumb_paths(it->path, png, NULL);
        HBITMAP b = wic_load(png, TW, TH);
        if (b) { it->img = ImageList_Add(g_il, b, NULL); DeleteObject(b); }
        for (int r = 0, n = ListView_GetItemCount(g_list); r < n; r++) {
            LVITEMW li = {LVIF_PARAM, r}; ListView_GetItem(g_list, &li);
            if ((int)li.lParam == (int)wp && it->img >= 0) { li.mask = LVIF_IMAGE; li.iImage = it->img; ListView_SetItem(g_list, &li); }
        }
        if (sel_index() == (int)wp) update_panel();
        return 0;
    }
    case WM_DROPFILES: {
        HDROP d = (HDROP)wp; UINT n = DragQueryFileW(d, 0xFFFFFFFF, NULL, 0); int added = 0;
        for (UINT i = 0; i < n; i++) { wchar_t f[MAX_PATH * 2]; DragQueryFileW(d, i, f, MAX_PATH * 2); added += lib_import(f) ? 1 : 0; }
        DragFinish(d); if (added) { reload_library(); opt_kick(); }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDC_SEARCH && code == EN_CHANGE) { fill_list(); update_panel(); }
        else if (id == IDC_ADD) add_files_dialog();
        else if (id == IDC_APPLY) apply_selected();
        else if (id == IDC_REMOVE) {
            int i = sel_index(); if (i < 0) return 0;
            wchar_t q[MAX_PATH + 64]; swprintf(q, MAX_PATH + 64, L"Remove \"%s\" from your library?", g_items[i].name);
            if (MessageBoxW(h, q, APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            int inuse = !_wcsicmp(g_file, g_items[i].path);
            for (int k = 0; k < MAX_MON; k++) if (!_wcsicmp(g_monFile[k], g_items[i].path)) { g_monFile[k][0] = 0; inuse = 1; }
            if (!_wcsicmp(g_file, g_items[i].path)) g_file[0] = 0;
            if (inuse) { save_settings(); create_screens(); apply_pause(); }
            wchar_t png[MAX_PATH], meta[MAX_PATH]; thumb_paths(g_items[i].path, png, meta);
            DeleteFileW(png); DeleteFileW(meta); opt_delete_for(g_items[i].path); DeleteFileW(g_items[i].path);
            reload_library();
        }
        else if (code == BN_CLICKED && (id == IDC_OPT || id == IDC_SPAN)) {
            int on = IsDlgButtonChecked(h, id) == BST_CHECKED;
            if (id == IDC_OPT) g_optimize = on; else g_spanSetting = on;
            save_settings(); create_screens(); apply_pause(); fill_monitors();
        }
        else if (code == BN_CLICKED && id >= IDC_FILL && id <= IDC_MUTE) {
            int on = IsDlgButtonChecked(h, id) == BST_CHECKED;
            if (id == IDC_FILL) { g_fill = on; players_set_string("panscan", on ? "1.0" : "0.0"); if (g_mode == MODE_TILE) create_screens(); }
            if (id == IDC_PMAX) g_pauseOnMaximized = on;
            if (id == IDC_PBAT) g_pauseOnBattery = on;
            if (id == IDC_START) set_startup(on);
            if (id == IDC_MUTE) { g_mute = on; players_set_string("aid", on ? "no" : "auto"); }
            save_settings(); apply_pause();
        }
        else if ((id == IDC_FPS || id == IDC_CANVAS) && code == CBN_SELCHANGE) {
            int k = (int)SendDlgItemMessageW(h, id, CB_GETCURSEL, 0, 0);
            if (id == IDC_FPS) g_fpsCap = k == 0 ? 30 : k == 1 ? 60 : 0;
            else g_canvasPct = k == 0 ? 100 : k == 1 ? 75 : 50;
            save_settings(); create_screens(); apply_pause();
        }
        else if ((id == IDC_ROT || id == IDC_EVERY) && code == CBN_SELCHANGE) {
            int k = (int)SendDlgItemMessageW(h, id, CB_GETCURSEL, 0, 0); if (k < 0) return 0;
            if (id == IDC_ROT) g_rotMode = k; else g_rotEvery = k;
            save_settings(); rot_settings_changed();
        }
        else if (id == IDC_NEXT) rot_next("manual (library window)");
        else if (id == IDC_INCL && code == BN_CLICKED) {
            int i = sel_index(); if (i >= 0) rot_set_included(g_items[i].path, IsDlgButtonChecked(h, IDC_INCL) == BST_CHECKED);
        }
        else if (id == IDC_SPEED && code == CBN_SELCHANGE) {
            static const int sp[] = {25, 50, 75, 100, 125, 150, 200};
            int k = (int)SendDlgItemMessageW(h, IDC_SPEED, CB_GETCURSEL, 0, 0); if (k >= 0 && k < 7) g_speedPct = sp[k];
            double v = g_speedPct / 100.0; players_set_double("speed", v);
            save_settings();
        }
        return 0;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;   // hide to tray = destroy UI entirely
    case WM_DESTROY:
        g_uiGen++;
        if (g_il) { ImageList_Destroy(g_il); g_il = NULL; }
        if (g_prevBmp) { DeleteObject(g_prevBmp); g_prevBmp = NULL; }
        free(g_items); g_items = NULL; g_nitems = 0;
        DeleteObject(g_font); DeleteObject(g_fontBig); DeleteObject(g_brBg); DeleteObject(g_brPanel); DeleteObject(g_brEdit);
        g_ui = g_list = g_preview = NULL;
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);  // give UI pages back
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ui_open(void) {
    if (g_ui) { ShowWindow(g_ui, SW_RESTORE); SetForegroundWindow(g_ui); return; }
    static int reg;
    if (!reg) {
        WNDCLASSEXW wc = {sizeof wc}; wc.lpfnWndProc = ui_proc; wc.hInstance = g_inst; wc.lpszClassName = L"LiteWallLibrary";
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1)); RegisterClassExW(&wc); reg = 1;
    }
    HWND w = CreateWindowExW(WS_EX_ACCEPTFILES, L"LiteWallLibrary", L"LiteWall \u2014 Library", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1180, 880, NULL, NULL, g_inst, NULL);
    ShowWindow(w, SW_SHOW); SetForegroundWindow(w);
}

// LiteWall optimizer: transcode oversized videos once, in the background, to an H.264 copy at the
// largest monitor's resolution. A 4K decode keeps ~12 MB per decoder surface plus a 4K pipeline in
// every player; a 1080p copy cuts that by ~4x. Uses libmpv's built-in encoding mode (--o=).
// Included from litewall.c.
#define WM_OPT_DONE (WM_APP + 4)

static HANDLE g_optEvent, g_optThread;
static wchar_t g_optPath[MAX_PATH * 2];   // file currently being optimized ("" = idle); read by the UI
static volatile LONG g_optPct = -1;

static int is_video_ext(const wchar_t *p) {
    const wchar_t *e = wcsrchr(p, L'.'); if (!e) return 0;
    static const wchar_t *x[] = {L".mp4", L".webm", L".mkv", L".mov", L".avi", L".m4v", L".wmv"};
    for (size_t i = 0; i < sizeof x / sizeof x[0]; i++) if (!_wcsicmp(e, x[i])) return 1;
    return 0;
}
static void opt_dir(wchar_t *out) { swprintf(out, MAX_PATH, L"%s\\library\\.optimized", g_appdir); CreateDirectoryW(out, NULL); }
static const wchar_t *base_name(const wchar_t *p) { const wchar_t *b = wcsrchr(p, L'\\'); return b ? b + 1 : p; }
static void opt_path(const wchar_t *orig, int tw, int th, wchar_t *out, const wchar_t *suffix) {
    wchar_t d[MAX_PATH]; opt_dir(d);
    swprintf(out, MAX_PATH * 2, L"%s\\%s.%dx%d.mp4%s", d, base_name(orig), tw, th, suffix);
}
static int file_ok(const wchar_t *p) {
    WIN32_FILE_ATTRIBUTE_DATA fa; return GetFileAttributesExW(p, GetFileExInfoStandard, &fa) && (fa.nFileSizeLow || fa.nFileSizeHigh);
}
static const wchar_t *optimized_for(const wchar_t *orig, int tw, int th, wchar_t *out) {
    if (!is_video_ext(orig) || tw <= 0 || th <= 0) return NULL;
    opt_path(orig, tw, th, out, L"");
    return file_ok(out) ? out : NULL;
}
static void opt_delete_for(const wchar_t *orig) {   // remove all optimized copies of a library file
    wchar_t d[MAX_PATH], pat[MAX_PATH * 2]; opt_dir(d); swprintf(pat, MAX_PATH * 2, L"%s\\%s.*", d, base_name(orig));
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { wchar_t x[MAX_PATH * 2]; swprintf(x, MAX_PATH * 2, L"%s\\%s", d, fd.cFileName); DeleteFileW(x); } while (FindNextFileW(h, &fd));
    FindClose(h);
}

typedef struct { int64_t w, h; double dur; } Probe;
static int opt_probe(const wchar_t *file, Probe *p) {
    memset(p, 0, sizeof *p);
    mpv_handle *m = mpv_create(); if (!m) return 0;
    static const char *o[][2] = {{"config", "no"}, {"terminal", "no"}, {"msg-level", "all=no"}, {"vo", "null"}, {"ao", "null"},
                                 {"aid", "no"}, {"sid", "no"}, {"frames", "1"}, {"hwdec", "no"}, {"load-scripts", "no"}, {"ytdl", "no"}, {"idle", "no"}};
    for (size_t i = 0; i < sizeof o / sizeof o[0]; i++) mpv_set_option_string(m, o[i][0], o[i][1]);
    int ok = 0;
    if (mpv_initialize(m) >= 0) {
        char u[MAX_PATH * 6]; to_utf8(file, u, sizeof u);
        const char *cmd[] = {"loadfile", u, NULL}; mpv_command(m, cmd);
        DWORD t0 = GetTickCount();
        for (;;) {
            mpv_event *e = mpv_wait_event(m, 1.0);
            if (e->event_id == MPV_EVENT_FILE_LOADED) mpv_get_property(m, "duration", MPV_FORMAT_DOUBLE, &p->dur);
            if (e->event_id == MPV_EVENT_VIDEO_RECONFIG) {
                mpv_get_property(m, "width", MPV_FORMAT_INT64, &p->w); mpv_get_property(m, "height", MPV_FORMAT_INT64, &p->h);
                if (p->w > 0) { ok = 1; break; }
            }
            if (e->event_id == MPV_EVENT_END_FILE || e->event_id == MPV_EVENT_SHUTDOWN || GetTickCount() - t0 > 15000) break;
        }
    }
    mpv_terminate_destroy(m);
    return ok;
}

// Encode with one encoder; returns 1 on success.
static int opt_encode(const wchar_t *src, const wchar_t *dst, int ow, int oh, const char *enc, const char *encopts) {
    mpv_handle *m = mpv_create(); if (!m) return 0;
    char u[MAX_PATH * 6], o8[MAX_PATH * 6], vf[96];
    to_utf8(src, u, sizeof u); to_utf8(dst, o8, sizeof o8);
    snprintf(vf, sizeof vf, "scale=w=%d:h=%d", ow, oh);
    static const char *o[][2] = {{"config", "no"}, {"terminal", "no"}, {"msg-level", "all=no"}, {"load-scripts", "no"}, {"ytdl", "no"},
        {"idle", "no"}, {"keep-open", "no"}, {"loop-file", "no"}, {"sid", "no"}, {"hwdec", "auto-copy"},
        {"of", "mp4"}, {"ofopts", "movflags=+faststart"}, {"oac", "aac"}, {"oacopts", "b=128k"}};
    for (size_t i = 0; i < sizeof o / sizeof o[0]; i++) mpv_set_option_string(m, o[i][0], o[i][1]);
    mpv_set_option_string(m, "o", o8); mpv_set_option_string(m, "ovc", enc); mpv_set_option_string(m, "ovcopts", encopts);
    mpv_set_option_string(m, "vf", vf);
    if (g_log) mpv_request_log_messages(m, "info");
    int ok = 0;
    if (mpv_initialize(m) >= 0) {
        const char *cmd[] = {"loadfile", u, NULL}; mpv_command(m, cmd);
        int paused = 0;
        for (;;) {
            mpv_event *e = mpv_wait_event(m, 0.5);
            if (e->event_id == MPV_EVENT_LOG_MESSAGE && g_log) {
                mpv_event_log_message *lm = e->data; size_t n = strlen(lm->text);
                LOG("enc[%s] [%s] %s: %.*s", enc, lm->level, lm->prefix, (int)(n && lm->text[n - 1] == '\n' ? n - 1 : n), lm->text);
            }
            if (e->event_id == MPV_EVENT_END_FILE) {
                mpv_event_end_file *ef = e->data;
                ok = ef->reason == MPV_END_FILE_REASON_EOF;
                LOG("encode %s finished reason=%d error=%s", enc, ef->reason, mpv_error_string(ef->error));
                break;
            }
            if (e->event_id == MPV_EVENT_SHUTDOWN) break;
            double pct = 0; if (mpv_get_property(m, "percent-pos", MPV_FORMAT_DOUBLE, &pct) >= 0) InterlockedExchange(&g_optPct, (LONG)pct);
            int want = g_allPaused == 1;   // a game / fullscreen app is up: stop encoding until it's gone
            if (want != paused) { mpv_set_property(m, "pause", MPV_FORMAT_FLAG, &want); paused = want; LOG("encode %s", want ? "paused (fullscreen app)" : "resumed"); }
        }
    }
    mpv_terminate_destroy(m);
    return ok && file_ok(dst);
}

static int opt_needed(const wchar_t *f, int tw, int th, Probe *p, int *ow, int *oh) {
    if (!opt_probe(f, p)) { LOG("optimize: probe failed for %ls", f); return 0; }
    WIN32_FILE_ATTRIBUTE_DATA fa; GetFileAttributesExW(f, GetFileExInfoStandard, &fa);
    double bytes = (double)fa.nFileSizeHigh * 4294967296.0 + fa.nFileSizeLow;
    double mbps = p->dur > 0 ? bytes * 8 / p->dur / 1e6 : 0;
    int big = p->w > tw || p->h > th, heavy = mbps > 25;
    // cover the target box keeping aspect (fill crops at playback); never upscale; even dimensions
    double k = (double)tw / p->w; if ((double)th / p->h > k) k = (double)th / p->h; if (k > 1) k = 1;
    *ow = ((int)(p->w * k + 0.5)) & ~1; *oh = ((int)(p->h * k + 0.5)) & ~1;
    LOG("optimize check %ls: %lldx%lld %.1fs %.1f Mbps target %dx%d -> %s (%dx%d)", base_name(f), (long long)p->w, (long long)p->h, p->dur, mbps, tw, th,
        (big || heavy) ? "OPTIMIZE" : "keep original", *ow, *oh);
    return big || heavy;
}

static DWORD WINAPI opt_thread(LPVOID arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
    for (;;) {
        WaitForSingleObject(g_optEvent, INFINITE);
        Sleep(3000);   // let startup / import settle first
        int again = 1;
        while (again && g_optimize) {
            again = 0;
            int tw = g_maxW, th = g_maxH;
            if (tw <= 0 || th <= 0) break;
            wchar_t d[MAX_PATH], pat[MAX_PATH]; swprintf(d, MAX_PATH, L"%s\\library", g_appdir); swprintf(pat, MAX_PATH, L"%s\\*", d);
            WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
            if (h == INVALID_HANDLE_VALUE) break;
            wchar_t todo[MAX_PATH * 2] = L"";
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                wchar_t f[MAX_PATH * 2]; swprintf(f, MAX_PATH * 2, L"%s\\%s", d, fd.cFileName);
                if (!is_video_ext(f)) continue;
                wchar_t out[MAX_PATH * 2], mark[MAX_PATH * 2];
                opt_path(f, tw, th, out, L""); opt_path(f, tw, th, mark, L".skip");
                if (file_ok(out) || GetFileAttributesW(mark) != INVALID_FILE_ATTRIBUTES) continue;
                wcscpy(todo, f); break;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
            if (!todo[0]) break;
            again = 1;
            wchar_t out[MAX_PATH * 2], part[MAX_PATH * 2], mark[MAX_PATH * 2];
            opt_path(todo, tw, th, out, L""); opt_path(todo, tw, th, part, L".part.mp4"); opt_path(todo, tw, th, mark, L".skip");
            Probe p; int ow, oh;
            if (!opt_needed(todo, tw, th, &p, &ow, &oh)) {   // remember "no need" so we don't re-probe every start
                HANDLE m = CreateFileW(mark, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL); if (m != INVALID_HANDLE_VALUE) CloseHandle(m);
                continue;
            }
            wcscpy(g_optPath, todo); InterlockedExchange(&g_optPct, 0);
            static const char *encs[][2] = {{"h264_amf", "b=8M,quality=balanced"}, {"h264_nvenc", "b=8M,preset=p4"}, {"h264_qsv", "b=8M"},
                                           {"h264_mf", "b=8M,hw_encoding=1"}, {"libx264", "crf=20,preset=veryfast"}};
            int ok = 0;
            for (size_t i = 0; i < sizeof encs / sizeof encs[0] && !ok && g_optimize; i++) {
                DeleteFileW(part);
                LOG("optimize %ls -> %dx%d with %s", base_name(todo), ow, oh, encs[i][0]);
                ok = opt_encode(todo, part, ow, oh, encs[i][0], encs[i][1]);
            }
            if (ok) {
                // drop copies made for other (old) monitor sizes, then publish the new one
                wchar_t keepname[MAX_PATH * 2]; wcscpy(keepname, part);
                wchar_t od[MAX_PATH], pat2[MAX_PATH * 2]; opt_dir(od); swprintf(pat2, MAX_PATH * 2, L"%s\\%s.*.mp4", od, base_name(todo));
                WIN32_FIND_DATAW f2; HANDLE h2 = FindFirstFileW(pat2, &f2);
                if (h2 != INVALID_HANDLE_VALUE) { do { wchar_t x[MAX_PATH * 2]; swprintf(x, MAX_PATH * 2, L"%s\\%s", od, f2.cFileName); if (_wcsicmp(x, keepname)) DeleteFileW(x); } while (FindNextFileW(h2, &f2)); FindClose(h2); }
                MoveFileExW(part, out, MOVEFILE_REPLACE_EXISTING);
                LOG("optimize done: %ls", out);
            } else {
                DeleteFileW(part);
                HANDLE m = CreateFileW(mark, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL); if (m != INVALID_HANDLE_VALUE) CloseHandle(m);
                LOG("optimize FAILED for %ls with all encoders; playing original", todo);
            }
            g_optPath[0] = 0; InterlockedExchange(&g_optPct, -1);
            if (ok) PostMessageW(g_msg, WM_OPT_DONE, 0, 0);
        }
        g_optPath[0] = 0;
    }
    return 0;
}

static void opt_kick(void) {
    if (!g_optimize || g_noVideo) return;
    if (!g_optEvent) g_optEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_optThread) g_optThread = CreateThread(NULL, 64 * 1024, opt_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    SetEvent(g_optEvent);
}

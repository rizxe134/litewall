// LiteWall optimizer: transcode oversized videos once, in the background, to an H.264 copy at the
// largest monitor's resolution. A 4K decode keeps ~12 MB per decoder surface plus a 4K pipeline in
// every player; a 1080p copy cuts that by ~4x. Uses libmpv's built-in encoding mode (--o=).
// Included from litewall.c.
#define WM_OPT_DONE (WM_APP + 4)

static HANDLE g_optEvent, g_optThread;
static HANDLE g_optJob;                       // helper processes live in a kill-on-close job
static volatile LONG *g_encPctShm;            // helper side: progress shared with the main process
static volatile LONG g_optPct = -1;
static wchar_t g_optPath[MAX_PATH * 2];   // file currently being optimized ("" = idle); read by the UI

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
    swprintf(out, MAX_PATH * 2, L"%s\\%s.%dx%d@%d.mp4%s", d, base_name(orig), tw, th, g_fpsCap, suffix);
}
static int file_ok(const wchar_t *p) {
    WIN32_FILE_ATTRIBUTE_DATA fa; return GetFileAttributesExW(p, GetFileExInfoStandard, &fa) && (fa.nFileSizeLow || fa.nFileSizeHigh);
}
static const wchar_t *optimized_for(const wchar_t *orig, int tw, int th, wchar_t *out) {
    if (!is_video_ext(orig) || tw <= 0 || th <= 0) return NULL;
    opt_path(orig, tw, th, out, L"");
    return file_ok(out) ? out : NULL;
}
// Any existing copy for this size at another fps cap (newest), used while the exact one is encoding.
static const wchar_t *any_copy_for(const wchar_t *orig, int tw, int th, wchar_t *out) {
    if (!is_video_ext(orig) || tw <= 0 || th <= 0) return NULL;
    wchar_t d[MAX_PATH], pat[MAX_PATH * 2]; opt_dir(d); swprintf(pat, MAX_PATH * 2, L"%s\\%s.%dx%d@*.mp4", d, base_name(orig), tw, th);
    WIN32_FIND_DATAW f; HANDLE h = FindFirstFileW(pat, &f); if (h == INVALID_HANDLE_VALUE) return NULL;
    FILETIME bt = {0}; int found = 0;
    do { if (wcsstr(f.cFileName, L".part.") || !(f.nFileSizeLow || f.nFileSizeHigh)) continue;
         if (CompareFileTime(&f.ftLastWriteTime, &bt) > 0) { bt = f.ftLastWriteTime; swprintf(out, MAX_PATH * 2, L"%s\\%s", d, f.cFileName); found = 1; } } while (FindNextFileW(h, &f));
    FindClose(h);
    return found ? out : NULL;
}
static void opt_delete_for(const wchar_t *orig) {   // remove all optimized copies of a library file
    wchar_t d[MAX_PATH], pat[MAX_PATH * 2]; opt_dir(d); swprintf(pat, MAX_PATH * 2, L"%s\\%s.*", d, base_name(orig));
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { wchar_t x[MAX_PATH * 2]; swprintf(x, MAX_PATH * 2, L"%s\\%s", d, fd.cFileName); DeleteFileW(x); } while (FindNextFileW(h, &fd));
    FindClose(h);
}

typedef struct { int64_t w, h; double dur, fps; } Probe;
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
            if (e->event_id == MPV_EVENT_FILE_LOADED) { mpv_get_property(m, "duration", MPV_FORMAT_DOUBLE, &p->dur); mpv_get_property(m, "container-fps", MPV_FORMAT_DOUBLE, &p->fps); }
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
static int opt_encode(const wchar_t *src, const wchar_t *dst, int ow, int oh, int fps, const char *enc, const char *encopts) {
    mpv_handle *m = mpv_create(); if (!m) return 0;
    char u[MAX_PATH * 6], o8[MAX_PATH * 6], vf[96];
    to_utf8(src, u, sizeof u); to_utf8(dst, o8, sizeof o8);
    if (fps > 0) snprintf(vf, sizeof vf, "scale=w=%d:h=%d,fps=%d", ow, oh, fps); else snprintf(vf, sizeof vf, "scale=w=%d:h=%d", ow, oh);
    static const char *o[][2] = {{"config", "no"}, {"terminal", "no"}, {"msg-level", "all=no"}, {"load-scripts", "no"}, {"ytdl", "no"},
        {"idle", "no"}, {"keep-open", "no"}, {"loop-file", "no"}, {"sid", "no"}, {"hwdec", "auto-copy"},
        {"vd-lavc-threads", "2"}, {"demuxer-max-bytes", "4MiB"}, {"cache", "no"}, {"of", "mp4"}, {"ofopts", "movflags=+faststart"}, {"oac", "aac"}, {"oacopts", "b=128k"}};
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
            double pct = 0; if (mpv_get_property(m, "percent-pos", MPV_FORMAT_DOUBLE, &pct) >= 0) { if (g_encPctShm) InterlockedExchange(g_encPctShm, (LONG)pct); }
            int want = g_gameActive;   // a game / fullscreen app is up: stop encoding until it's gone
            if (want != paused) { mpv_set_property(m, "pause", MPV_FORMAT_FLAG, &want); paused = want; LOG("encode %s", want ? "paused (fullscreen app/game detected; see monitor PAUSE lines)" : "resumed"); }
        }
    }
    mpv_terminate_destroy(m);
    return ok && file_ok(dst);
}

static int helper_probe(const wchar_t *f, Probe *p);
static int opt_needed(const wchar_t *f, int tw, int th, Probe *p, int *ow, int *oh) {
    if (!helper_probe(f, p)) { LOG("optimize: probe failed for %ls", f); return 0; }
    WIN32_FILE_ATTRIBUTE_DATA fa; GetFileAttributesExW(f, GetFileExInfoStandard, &fa);
    double bytes = (double)fa.nFileSizeHigh * 4294967296.0 + fa.nFileSizeLow;
    double mbps = p->dur > 0 ? bytes * 8 / p->dur / 1e6 : 0;
    int big = p->w > tw || p->h > th, heavy = mbps > 25 || (g_fpsCap > 0 && p->fps > g_fpsCap + 1);
    // cover the target box keeping aspect (fill crops at playback); never upscale; even dimensions
    double k = (double)tw / p->w; if ((double)th / p->h > k) k = (double)th / p->h; if (k > 1) k = 1;
    *ow = ((int)(p->w * k + 0.5)) & ~1; *oh = ((int)(p->h * k + 0.5)) & ~1;
    LOG("optimize check %ls: %lldx%lld %.1fs %.1f Mbps target %dx%d -> %s (%dx%d)", base_name(f), (long long)p->w, (long long)p->h, p->dur, mbps, tw, th,
        (big || heavy) ? "OPTIMIZE" : "keep original", *ow, *oh);
    return big || heavy;
}

/* ---------- helper process (v0.3.5) ----------
   Probing and encoding run in a short-lived child "LiteWall.exe --encode ..." / "--probe ..." at idle
   priority with EcoQoS, so the encoder's heap and buffers never land in the main process. */
static HANDLE g_pctMap; static volatile LONG *g_pctView;
static int helper_run(const wchar_t *args, const wchar_t *what, int killOnGame) {
    // returns child exit code, -1 = could not start, -2 = killed for a game / optimize turned off
    if (!g_optJob) {
        g_optJob = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li; memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (g_optJob) SetInformationJobObject(g_optJob, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    if (g_pctView) InterlockedExchange(g_pctView, 0);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    size_t n = wcslen(exe) + wcslen(args) + 32; wchar_t *cl = malloc(n * sizeof(wchar_t));
    swprintf(cl, n, L"\"%s\" %s%s", exe, args, g_log ? L" --log" : L"");
    STARTUPINFOW si = {sizeof si}; PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(exe, cl, NULL, NULL, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW | IDLE_PRIORITY_CLASS, NULL, NULL, &si, &pi);
    free(cl);
    if (!ok) { LOG("helper %ls: CreateProcess failed err=%lu", what, GetLastError()); return -1; }
    if (g_optJob) AssignProcessToJobObject(g_optJob, pi.hProcess);
    ResumeThread(pi.hThread); CloseHandle(pi.hThread);
    DWORD t0 = GetTickCount(); LOG("helper %ls started pid=%lu", what, pi.dwProcessId);
    int code = -2;
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 500) == WAIT_OBJECT_0) { DWORD c = 1; GetExitCodeProcess(pi.hProcess, &c); code = (int)c; break; }
        if (g_pctView) InterlockedExchange(&g_optPct, *g_pctView);
        if ((killOnGame && g_gameActive) || !g_optimize) {
            TerminateProcess(pi.hProcess, 3); WaitForSingleObject(pi.hProcess, 5000);
            LOG("helper %ls pid=%lu killed (%s) after %.1fs", what, pi.dwProcessId, g_optimize ? "fullscreen app/game started" : "optimize turned off", (GetTickCount() - t0) / 1000.0);
            code = -2; break;
        }
    }
    if (code != -2) LOG("helper %ls pid=%lu exit=%d after %.1fs", what, pi.dwProcessId, code, (GetTickCount() - t0) / 1000.0);
    CloseHandle(pi.hProcess);
    HeapCompact(GetProcessHeap(), 0); SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    return code;
}
static int helper_probe(const wchar_t *f, Probe *p) {
    memset(p, 0, sizeof *p);
    wchar_t ini[MAX_PATH], args[MAX_PATH * 5]; swprintf(ini, MAX_PATH, L"%s\\probe.tmp", g_appdir); DeleteFileW(ini);
    swprintf(args, MAX_PATH * 5, L"--probe \"%s\" \"%s\"", f, ini);
    int c = helper_run(args, L"probe", 0);
    if (c == 0) {
        wchar_t b[64];
        p->w = GetPrivateProfileIntW(L"p", L"w", 0, ini); p->h = GetPrivateProfileIntW(L"p", L"h", 0, ini);
        GetPrivateProfileStringW(L"p", L"dur", L"0", b, 64, ini); p->dur = _wtof(b);
        GetPrivateProfileStringW(L"p", L"fps", L"0", b, 64, ini); p->fps = _wtof(b);
    }
    DeleteFileW(ini);
    return c == 0 && p->w > 0;
}
// Child-side entry points. Never touch the tray, the mutex or any window.
static void helper_lowpower(void) {
    SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
    PROCESS_POWER_THROTTLING_STATE t = {PROCESS_POWER_THROTTLING_CURRENT_VERSION, PROCESS_POWER_THROTTLING_EXECUTION_SPEED, PROCESS_POWER_THROTTLING_EXECUTION_SPEED};
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &t, sizeof t);
}
static int helper_main(int argc, wchar_t **argv) {
    helper_lowpower();
    wchar_t dir[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, dir))) { swprintf(g_appdir, MAX_PATH, L"%s\\LiteWall", dir); }
    for (int i = 1; i < argc; i++) if (!wcscmp(argv[i], L"--log")) g_log = 1;
    if (g_log) { wchar_t lp[MAX_PATH]; swprintf(lp, MAX_PATH, L"%s\\encode.log", g_appdir); g_logf = _wfopen(lp, L"a"); }
    if (!wcscmp(argv[1], L"--probe") && argc >= 4) {
        Probe p; if (!opt_probe(argv[2], &p)) return 2;
        wchar_t b[64];
        swprintf(b, 64, L"%lld", (long long)p.w); WritePrivateProfileStringW(L"p", L"w", b, argv[3]);
        swprintf(b, 64, L"%lld", (long long)p.h); WritePrivateProfileStringW(L"p", L"h", b, argv[3]);
        swprintf(b, 64, L"%.3f", p.dur); WritePrivateProfileStringW(L"p", L"dur", b, argv[3]);
        swprintf(b, 64, L"%.3f", p.fps); WritePrivateProfileStringW(L"p", L"fps", b, argv[3]);
        return 0;
    }
    if (!wcscmp(argv[1], L"--encode") && argc >= 9) {
        char enc[64], eo[256]; WideCharToMultiByte(CP_UTF8, 0, argv[7], -1, enc, sizeof enc, NULL, NULL); WideCharToMultiByte(CP_UTF8, 0, argv[8], -1, eo, sizeof eo, NULL, NULL);
        HANDLE mp = OpenFileMappingW(FILE_MAP_WRITE, FALSE, L"Local\\LiteWall.EncodeProgress");
        if (mp) g_encPctShm = MapViewOfFile(mp, FILE_MAP_WRITE, 0, 0, sizeof(LONG));
        LOG("encode helper pid=%lu: %ls -> %ls %lsx%ls fps=%ls %s", GetCurrentProcessId(), base_name(argv[2]), base_name(argv[3]), argv[4], argv[5], argv[6], enc);
        int ok = opt_encode(argv[2], argv[3], _wtoi(argv[4]), _wtoi(argv[5]), _wtoi(argv[6]), enc, eo);
        if (g_logf) fclose(g_logf);
        return ok ? 0 : 1;
    }
    return 64;
}

// True when a copy for this exact target exists and is newer than the source.
static int opt_copy_current(const wchar_t *src, const wchar_t *out) {
    WIN32_FILE_ATTRIBUTE_DATA a, b;
    if (!GetFileAttributesExW(out, GetFileExInfoStandard, &b) || !(b.nFileSizeLow || b.nFileSizeHigh)) return 0;
    if (!GetFileAttributesExW(src, GetFileExInfoStandard, &a)) return 1;
    return CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) <= 0;
}
// Keep the current copy plus the most recent other variant (so flipping fps cap / span / monitor
// layout back and forth doesn't re-encode the whole library); delete anything older.
static void opt_prune(const wchar_t *src, const wchar_t *keep) {
    wchar_t od[MAX_PATH], pat[MAX_PATH * 2], best[MAX_PATH * 2] = L""; FILETIME bt = {0};
    opt_dir(od); swprintf(pat, MAX_PATH * 2, L"%s\\%s.*@*.mp4", od, base_name(src));
    WIN32_FIND_DATAW f; HANDLE h = FindFirstFileW(pat, &f); if (h == INVALID_HANDLE_VALUE) return;
    do { wchar_t x[MAX_PATH * 2]; swprintf(x, MAX_PATH * 2, L"%s\\%s", od, f.cFileName);
         if (_wcsicmp(x, keep) && CompareFileTime(&f.ftLastWriteTime, &bt) > 0) { bt = f.ftLastWriteTime; wcscpy(best, x); } } while (FindNextFileW(h, &f));
    FindClose(h);
    h = FindFirstFileW(pat, &f); if (h == INVALID_HANDLE_VALUE) return;
    do { wchar_t x[MAX_PATH * 2]; swprintf(x, MAX_PATH * 2, L"%s\\%s", od, f.cFileName);
         if (_wcsicmp(x, keep) && _wcsicmp(x, best) && !wcsstr(f.cFileName, L".part.")) { DeleteFileW(x); LOG("optimize: pruned old copy %ls", f.cFileName); } } while (FindNextFileW(h, &f));
    FindClose(h);
}

static DWORD WINAPI opt_thread(LPVOID arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
    if (!g_pctMap) { g_pctMap = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(LONG), L"Local\\LiteWall.EncodeProgress");
                     if (g_pctMap) g_pctView = MapViewOfFile(g_pctMap, FILE_MAP_WRITE, 0, 0, sizeof(LONG)); }
    for (;;) {
        WaitForSingleObject(g_optEvent, INFINITE);
        Sleep(3000);   // let startup / import / settings changes settle first
        int again = 1;
        while (again && g_optimize) {
            again = 0;
            while (g_gameActive && g_optimize) Sleep(2000);   // never start while a game is up
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
                if (opt_copy_current(f, out) || GetFileAttributesW(mark) != INVALID_FILE_ATTRIBUTES) continue;
                LOG("optimize: %ls needs a copy for %dx%d@%d (%s)", fd.cFileName, tw, th, g_fpsCap, file_ok(out) ? "source is newer" : "no copy for this target");
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
            static const wchar_t *encs[][2] = {{L"h264_amf", L"b=8M,quality=balanced"}, {L"h264_nvenc", L"b=8M,preset=p4"}, {L"h264_qsv", L"b=8M"},
                                              {L"h264_mf", L"b=8M,hw_encoding=1"}, {L"libx264", L"crf=20,preset=veryfast,threads=2"}};
            int ok = 0, fps = (g_fpsCap > 0 && p.fps > g_fpsCap + 1) ? g_fpsCap : 0;
            for (size_t i = 0; i < sizeof encs / sizeof encs[0] && !ok && g_optimize; i++) {
                DeleteFileW(part);
                LOG("optimize %ls -> %dx%d with %ls", base_name(todo), ow, oh, encs[i][0]);
                wchar_t args[MAX_PATH * 6];
                swprintf(args, MAX_PATH * 6, L"--encode \"%s\" \"%s\" %d %d %d %s %s", todo, part, ow, oh, fps, encs[i][0], encs[i][1]);
                int c = helper_run(args, L"encode", 1);
                if (c == -2) {   // killed for a game: wait it out and retry the same encoder
                    DeleteFileW(part);
                    if (!g_optimize) break;
                    while (g_gameActive && g_optimize) Sleep(2000);
                    i--; continue;
                }
                ok = c == 0 && file_ok(part);
            }
            if (ok) {
                MoveFileExW(part, out, MOVEFILE_REPLACE_EXISTING);
                opt_prune(todo, out);
                LOG("optimize done: %ls", out);
            } else if (g_optimize) {
                DeleteFileW(part);
                HANDLE m = CreateFileW(mark, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL); if (m != INVALID_HANDLE_VALUE) CloseHandle(m);
                LOG("optimize FAILED for %ls with all encoders; playing original", todo);
            } else DeleteFileW(part);
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

#include "events.h"

#define MAX_XML_BYTES (256u * 1024u * sizeof(WCHAR))
#define Q_RECENT_30M L"*[System[(Level=1 or Level=2 or Level=3) and TimeCreated[timediff(@SystemTime) <= 1800000]]]"
#define Q_RECENT_60M L"*[System[(Level=1 or Level=2 or Level=3) and TimeCreated[timediff(@SystemTime) <= 3600000]]]"
#define Q_SLEEP L"*[System[Provider[@Name='Microsoft-Windows-Power-Troubleshooter' or @Name='Microsoft-Windows-Kernel-Power'] and (EventID=1 or EventID=42 or EventID=107 or EventID=506 or EventID=507) and TimeCreated[timediff(@SystemTime) <= 1800000]]]"
#define Q_WER   L"*[System[Provider[@Name='Windows Error Reporting'] and EventID=1001 and TimeCreated[timediff(@SystemTime) <= 1800000]]]"

typedef struct Pub { WCHAR name[96]; EVT_HANDLE h; } Pub;

typedef struct EvCtx {
    FILE *digest;
    EVT_HANDLE render;
    BYTE *vbuf; DWORD vcap;
    Pub pub[24]; int npub;
    ULONGLONG trig_ft;
    EventSummary *sum;
    bool redact;
    int rows, fmt_budget;
} EvCtx;

static EVT_HANDLE publisher(EvCtx *c, const WCHAR *provider) {
    for (int i = 0; i < c->npub; ++i) if (wcscmp(c->pub[i].name, provider) == 0) return c->pub[i].h;
    if (c->npub >= 24) return NULL;
    Pub *p = &c->pub[c->npub++];
    wft_wsnprintf(p->name, 96, L"%ls", provider);
    p->h = EvtOpenPublisherMetadata(NULL, provider, NULL, 0, 0);     /* NULL is cached too: do not retry */
    return p->h;
}

static WCHAR *render_xml(EVT_HANDLE ev) {
    DWORD need = 0, props = 0;
    EvtRender(NULL, ev, EvtRenderEventXml, 0, NULL, &need, &props);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || need == 0 || need > MAX_XML_BYTES) return NULL;
    WCHAR *xml = (WCHAR *)malloc(need);
    if (!xml) return NULL;
    DWORD used = 0;
    if (!EvtRender(NULL, ev, EvtRenderEventXml, need, xml, &used, &props)) { free(xml); return NULL; }
    return xml;
}

static bool render_system(EvCtx *c, EVT_HANDLE ev) {
    DWORD used = 0, props = 0;
    if (EvtRender(c->render, ev, EvtRenderEventValues, c->vcap, c->vbuf, &used, &props)) return true;
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || used == 0 || used > 65536) return false;
    BYTE *nb = (BYTE *)realloc(c->vbuf, used);
    if (!nb) return false;
    c->vbuf = nb; c->vcap = used;
    return EvtRender(c->render, ev, EvtRenderEventValues, c->vcap, c->vbuf, &used, &props) != 0;
}

static void one_line(WCHAR *s, size_t max) {
    size_t j = 0;
    for (size_t i = 0; s[i] && j + 1 < max; ++i) s[j++] = (s[i] < 0x20) ? L' ' : s[i];
    s[j] = 0;
}

static void handle_event(EvCtx *c, EVT_HANDLE ev, FILE *raw, int *raw_count) {
    if (!render_system(c, ev)) return;
    const EVT_VARIANT *v = (const EVT_VARIANT *)c->vbuf;
    const WCHAR *prov = v[EvtSystemProviderName].Type == EvtVarTypeString ? v[EvtSystemProviderName].StringVal : L"";
    const WCHAR *chan = v[EvtSystemChannel].Type == EvtVarTypeString ? v[EvtSystemChannel].StringVal : L"";
    uint32_t id = v[EvtSystemEventID].Type == EvtVarTypeUInt16 ? v[EvtSystemEventID].UInt16Val : 0;
    uint32_t level = v[EvtSystemLevel].Type == EvtVarTypeByte ? v[EvtSystemLevel].ByteVal : 0;
    ULONGLONG ft = v[EvtSystemTimeCreated].Type == EvtVarTypeFileTime ? v[EvtSystemTimeCreated].FileTimeVal : 0;
    WCHAR provider[96]; wft_wsnprintf(provider, 96, L"%ls", prov);       /* v[] is invalidated by the next render */
    WCHAR channel[64];  wft_wsnprintf(channel, 64, L"%ls", chan);

    bool want_raw = raw && !c->redact && *raw_count < 160;
    bool is_wer = _wcsicmp(provider, L"Windows Error Reporting") == 0;
    WCHAR *xml = (want_raw || is_wer) ? render_xml(ev) : NULL;
    bool live = xml && wcsstr(xml, L"LiveKernelEvent") != NULL;
    if (want_raw && xml) { wft_write_utf8(raw, xml); fputs("\r\n", raw); ++*raw_count; }
    free(xml);

    EventClass cls = event_classify(provider, id, level, live);
    double age_s = ft ? (double)((LONGLONG)(c->trig_ft - ft)) / 1e7 : 0.0;
    char ex[112], pa[80]; wft_ascii_from_wide(pa, sizeof pa, provider);
    wft_snprintf(ex, sizeof ex, "%s %u", pa, (unsigned)id);
    event_summary_add(c->sum, cls, age_s, WFT_NEAR_EVENT_S, ex);

    WCHAR msg[1024]; msg[0] = 0;
    if (c->redact) wft_wsnprintf(msg, 1024, L"<redacted>");
    else if (c->fmt_budget > 0 && (cls != EVC_NONE || level <= 2)) {
        EVT_HANDLE pub = publisher(c, provider);
        DWORD used = 0;
        --c->fmt_budget;
        if (pub && EvtFormatMessage(pub, ev, 0, 0, NULL, EvtFormatMessageEvent, 1023, msg, &used)) one_line(msg, 260);
        else msg[0] = 0;
    }
    SYSTEMTIME utc, loc; FILETIME f; f.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFFu); f.dwHighDateTime = (DWORD)(ft >> 32);
    if (!ft || !FileTimeToSystemTime(&f, &utc) || !SystemTimeToTzSpecificLocalTime(NULL, &utc, &loc)) memset(&loc, 0, sizeof loc);
    fprintf(c->digest, "%04u-%02u-%02u %02u:%02u:%02u,%.1f,", loc.wYear, loc.wMonth, loc.wDay, loc.wHour, loc.wMinute, loc.wSecond, age_s);
    wft_csv_text(c->digest, channel); fprintf(c->digest, ",%u,", (unsigned)level);
    wft_csv_text(c->digest, provider); fprintf(c->digest, ",%u,%s,", (unsigned)id, event_class_name(cls));
    wft_csv_text(c->digest, msg); fputs("\r\n", c->digest);
    ++c->rows;
}

static bool run_query(EvCtx *c, const WCHAR *channel, const WCHAR *xpath, int max_events, FILE *raw) {
    EVT_HANDLE q = EvtQuery(NULL, channel, xpath, EvtQueryChannelPath | EvtQueryReverseDirection);   /* newest first */
    if (!q) return false;
    EVT_HANDLE ev[16]; DWORD got = 0; int total = 0, raw_count = 0;
    while (total < max_events && EvtNext(q, 16, ev, 2000, 0, &got)) {
        for (DWORD i = 0; i < got; ++i) {
            if (total < max_events) { handle_event(c, ev[i], raw, &raw_count); ++total; }
            EvtClose(ev[i]);
        }
    }
    EvtClose(q);
    return true;
}

void events_capture(const WCHAR *dir, const Config *cfg, ULONGLONG trigger_ft, EventsResult *res) {
    memset(res, 0, sizeof *res);
    event_summary_init(&res->summary);
    EvCtx c; memset(&c, 0, sizeof c);
    c.sum = &res->summary; c.redact = cfg->redact; c.fmt_budget = 120;
    c.trig_ft = trigger_ft;
    c.vcap = 8192; c.vbuf = (BYTE *)malloc(c.vcap);
    c.render = EvtCreateRenderContext(0, NULL, EvtRenderContextSystem);
    WCHAR path[WFT_PATH_CCH]; wft_wsnprintf(path, WFT_PATH_CCH, L"%ls\\events_digest.csv", dir);
    c.digest = plat_fopen_w(path);
    if (!c.vbuf || !c.render || !c.digest) goto done;
    fputs("time_local,seconds_before_trigger,channel,level,provider,event_id,class,message\r\n", c.digest);

    static const struct { const WCHAR *channel, *file, *xpath; int max; } q[] = {
        { L"System",      L"events_system.xml",      Q_RECENT_30M, 160 },
        { L"Application", L"events_application.xml", Q_RECENT_30M, 160 },
        { L"Microsoft-Windows-Diagnostics-Performance/Operational", L"events_performance.xml", Q_RECENT_60M, 160 },
        { L"System",      NULL, Q_SLEEP, 40 },      /* informational sleep/resume markers: digest only */
        { L"Application", NULL, Q_WER,   40 },      /* WER 1001 carries LiveKernelEvent (GPU/driver watchdog) */
    };
    for (size_t i = 0; i < sizeof q / sizeof q[0]; ++i) {
        FILE *raw = NULL;
        if (q[i].file && !cfg->redact) {
            wft_wsnprintf(path, WFT_PATH_CCH, L"%ls\\%ls", dir, q[i].file);
            raw = plat_fopen_w(path);
            if (raw) fputs("<!-- Recent Warning/Error/Critical events, newest first -->\r\n<Events>\r\n", raw);
        }
        if (run_query(&c, q[i].channel, q[i].xpath, q[i].max, raw)) ++res->channels_ok;
        if (raw) { fputs("</Events>\r\n", raw); fclose(raw); }
    }
done:
    res->digest_rows = c.rows;
    if (c.digest) fclose(c.digest);
    for (int i = 0; i < c.npub; ++i) if (c.pub[i].h) EvtClose(c.pub[i].h);
    if (c.render) EvtClose(c.render);
    free(c.vbuf);
}

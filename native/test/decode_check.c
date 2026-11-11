/*
 * mst2-cluster-nav - host check of the decoder module (src/sal/decode.c) and of the lab
 * player's decode path (src/sal/lab.c), 32-bit as the unit.
 * Android Auto branch. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * Stand-ins for mm-renderer (mmr_*), libstrm, QNX Screen, name_open, MsgSendvs_r and
 * TimerTimeout record every call in a trace and every mdev message byte for byte, so the
 * module's set-up order and URLs can be compared with the stock sequence of the notes, and the
 * messages with the formats found there. The worker runs with no thread of its own: the check
 * calls its passes (mcn_decode_step). Modes: the recording's path runs everything but two
 * cases that need a fresh process: "missing" has one Screen symbol gone (the module may then
 * never set anything up), "window k" fails the k-th call of the window set-up.
 *
 *   decode-check missing | window <k> | <recording.h264>   (a phone's 800x480 cluster stream, Annex-B)
 */
#include "mcn_qnx.h"
#include "mcn_aa.h"
#include "mcn_video.h"
#include "../src/sal/sal.h"

int puts(const char *);
int vsnprintf(char *, size_t, const char *, __builtin_va_list);
int atoi(const char *);
int strncmp(const char *, const char *, size_t);

static void dump_log(void);
#define CHECK(x)                                                           \
    do {                                                                   \
        if (!(x)) {                                                        \
            write(2, "decode check failed: " #x "\n", sizeof(#x) + 21);    \
            dump_log();                                                    \
            abort();                                                       \
        }                                                                  \
    } while (0)

/* ---- the unit around the module ------------------------------------------------------------ */

static int active = 1;
static unsigned now_ms = 100000, link_video, link_other;
static struct mcn_video video_mem;

int mcn_active(void)
{
    return active;
}

int mcn_stream_live(void)
{
    return 0;
}

int mcn_aa_stream_live(void)
{
    return 0;
}

struct mcn_video *mcn_video_map(void)
{
    return &video_mem;
}

void mcn_video_wake(struct mcn_video *v)
{
    (void)v;
}

void mcn_link_send(const char *text)
{
    if (!strcmp(text, "video=1")) {
        link_video++;
    } else {
        link_other++;
    }
}

/* The log: the last lines, and counts of what the checks look for. */
static char log_lines[64][200];
static unsigned log_count;

void mcn_log(const char *msg)
{
    snprintf(log_lines[log_count % 64], sizeof(log_lines[0]), "%s", msg);
    log_count++;
    if (getenv("DECODE_CHECK_LOG")) {
        puts(msg);
    }
}

void mcn_log2(const char *a, const char *b)
{
    char line[256];

    snprintf(line, sizeof(line), "%s%s", a, b);
    mcn_log(line);
}

static void dump_log(void)
{
    unsigned i, from = log_count > 64 ? log_count - 64 : 0;

    write(2, "--- last log lines ---\n", 23);
    for (i = from; i < log_count; i++) {
        write(2, log_lines[i % 64], strlen(log_lines[i % 64]));
        write(2, "\n", 1);
    }
}

/* Log lines since `from` that contain `text`. */
static unsigned logged(unsigned from, const char *text)
{
    unsigned i, n = 0, start = from;

    if (log_count - start > 64) {
        start = log_count - 64;
    }
    for (i = start; i < log_count; i++) {
        n += strstr(log_lines[i % 64], text) != NULL;
    }
    return n;
}

int mcn_test_clock_gettime(int clock, struct timespec *ts)
{
    (void)clock;
    ts->tv_sec = (long)(now_ms / 1000);
    ts->tv_nsec = (long)(now_ms % 1000) * 1000000L;
    return 0;
}

/* ---- the trace of calls ------------------------------------------------------------------- */

static char trace[1 << 16];
static unsigned trace_len;

static void tr(const char *fmt, ...)
{
    __builtin_va_list ap;
    int n;

    if (trace_len + 400 > sizeof(trace)) {
        trace_len = 0; /* only the latest part is ever compared */
    }
    __builtin_va_start(ap, fmt);
    n = vsnprintf(trace + trace_len, sizeof(trace) - trace_len, fmt, ap);
    __builtin_va_end(ap);
    if (n > 0) {
        trace_len += (unsigned)n;
    }
    trace[trace_len++] = '\n';
    trace[trace_len] = 0;
}

static void trace_reset(void)
{
    trace_len = 0;
    trace[0] = 0;
}

/* Failure injection: the fail_at-th call that may fail since the last reset fails. */
static unsigned calls, fail_at, injected;

static int failing(void)
{
    calls++;
    if (calls == fail_at) {
        injected++;
        return 1;
    }
    return 0;
}

/* ---- files ------------------------------------------------------------------------------- */

static int sw_file, sw_card = -1, hide_map, stock_state, stock_playing_now = 1, dpy_present = 1;
static unsigned dpy_counter_value = 1000;
static int dpy_capturing = 1;
static const uint8_t *replay;
static unsigned replay_size;
static unsigned read_pos[32];

#define FD_FLAG 10
#define FD_STOCK 13
#define FD_DPY 14
#define FD_LAB 15

int mcn_test_open(const char *path, int flags, ...)
{
    (void)flags;
    /* Decode mode is the default; the files turn it off (sw_file = 1: decode on = no file). */
    if (!strcmp(path, MCN_DIR "/aa-passthrough")) {
        return sw_file ? -1 : FD_FLAG;
    }
    if (!strncmp(path, "/media/mp00", 11) && strstr(path, "/mst2-cluster-nav-passthrough")) {
        return path[11] - '0' == sw_card ? FD_FLAG : -1;
    }
    if (!strcmp(path, MCN_DIR "/aa-decode-plain") ||
        (!strncmp(path, "/media/mp00", 11) && strstr(path, "/mst2-cluster-nav-decode-plain"))) {
        return -1; /* the plain fallback output is not exercised here */
    }
    if (!strcmp(path, MCN_DIR "/hide-map")) {
        return hide_map ? FD_FLAG : -1;
    }
    if (strstr(path, "/pps/services/multimedia/renderer/context/")) {
        CHECK(!strcmp(path, "/pps/services/multimedia/renderer/context/"
                            "SalTechnology::GoogleAutomotiveLink/state"));
        read_pos[FD_STOCK] = 0;
        return stock_state ? FD_STOCK : -1;
    }
    if (!strcmp(path, "/dev/screen/0/dpy-6")) {
        read_pos[FD_DPY] = 0;
        return dpy_present ? FD_DPY : -1;
    }
    if (!strcmp(path, MCN_LAB_VIDEO)) {
        return replay ? FD_LAB : -1;
    }
    if (strstr(path, "layout")) {
        return -1; /* no layout file: the defaults */
    }
    CHECK(!"unexpected file opened");
    return -1;
}

ssize_t mcn_test_read(int fd, void *buf, size_t n)
{
    char text[256];
    unsigned len, pos = read_pos[fd & 31], k;

    if (fd == FD_STOCK) {
        snprintf(text, sizeof(text), "@state\nspeed:n:1000\nstate::%s\n",
                 stock_playing_now ? "playing" : "idle");
    } else if (fd == FD_DPY) {
        snprintf(text, sizeof(text),
                 "id = 6\nsize = 1024x384\ncounter = %u\ncapture buffer = %s\nmode = 1024x384\n",
                 dpy_counter_value, dpy_capturing ? "0x1234 (1024x384)" : "(none)");
    } else {
        return 0;
    }
    len = strlen(text);
    if (pos >= len) {
        return 0;
    }
    k = len - pos < n ? len - pos : n;
    memcpy(buf, text + pos, k);
    read_pos[fd & 31] = pos + k;
    return (ssize_t)k;
}

int mcn_test_close(int fd)
{
    (void)fd;
    return 0;
}

off_t mcn_test_lseek(int fd, off_t off, int whence)
{
    CHECK(fd == FD_LAB && off == 0 && whence == SEEK_END);
    return (off_t)replay_size;
}

void *mcn_test_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    (void)addr;
    (void)prot;
    (void)flags;
    CHECK(fd == FD_LAB && len == replay_size && off == 0);
    return (void *)replay;
}

/* ---- threads and sleeps: the worker's passes run from the check ---------------------------- */

static void *(*thread_fn[4])(void *);
static unsigned threads, sleeps_100ms, in_step, lab_driving;
static void lab_tick(void);

int mcn_test_pthread_create(pthread_t *thread, const void *attr, void *(*fn)(void *), void *arg)
{
    (void)attr;
    (void)arg;
    CHECK(threads < 4);
    *thread = (pthread_t)(threads + 1);
    thread_fn[threads++] = fn;
    return 0;
}

int mcn_test_pthread_detach(pthread_t thread)
{
    (void)thread;
    return 0;
}

int mcn_test_usleep(unsigned us)
{
    now_ms += (us + 999) / 1000;
    if (us == 100000) {
        sleeps_100ms++;
    }
    if (lab_driving && !in_step) {
        lab_tick();
    }
    return 0;
}

static int step(void)
{
    int r;

    in_step++;
    r = mcn_decode_step();
    in_step--;
    return r;
}

/* Worker passes until it has nothing to do (bounded). */
static void drain(void)
{
    int i;

    for (i = 0; i < 100000 && step(); i++) {
    }
    CHECK(i < 100000);
}

/* ---- QNX Screen -------------------------------------------------------------------------- */

struct win {
    int alive;
    int props[100][2];
    char id[72];
    void *display;
    char group[64];
    int buffers, posts;
};
static struct win wins[8];
static unsigned nwins;
static int ctx_token, display_token[2] = { 1, 6 }, display_size[2][2] = { { 800, 480 },
                                                                        { 1024, 384 } };
static int buffer_token, event_token;
static struct {
    int type;
    unsigned win;
} events[16];
static unsigned ev_head, ev_tail, cur_event_type, cur_event_win;
static int renderer_resets_source;

static int win_index(void *w)
{
    CHECK((struct win *)w >= wins && (struct win *)w < wins + 8);
    return (int)((struct win *)w - wins);
}

static void push_event(int type, unsigned win)
{
    events[ev_tail++ % 16].type = type;
    events[(ev_tail - 1) % 16].win = win;
}

static int fake_screen_create_context(void **ctx, int flags)
{
    tr("screen_create_context(%d)", flags);
    if (failing()) {
        return -1;
    }
    *ctx = &ctx_token;
    return 0;
}

static int fake_screen_get_context_property_iv(void *ctx, int prop, int *v)
{
    CHECK(ctx == &ctx_token && prop == 59);
    tr("screen_get_context_property_iv(59)");
    if (failing()) {
        return -1;
    }
    *v = 2;
    return 0;
}

static int fake_screen_get_context_property_pv(void *ctx, int prop, void **v)
{
    CHECK(ctx == &ctx_token && prop == 60);
    tr("screen_get_context_property_pv(60)");
    if (failing()) {
        return -1;
    }
    v[0] = &display_token[0];
    v[1] = &display_token[1];
    return 0;
}

static int fake_screen_get_display_property_iv(void *d, int prop, int *v)
{
    int i = d == &display_token[0] ? 0 : 1;

    CHECK(d == &display_token[0] || d == &display_token[1]);
    tr("screen_get_display_property_iv(d%d,%d)", i, prop);
    if (failing()) {
        return -1;
    }
    if (prop == 87) {
        *v = display_token[i];
    } else if (prop == 40) {
        v[0] = display_size[i][0];
        v[1] = display_size[i][1];
    } else {
        CHECK(!"unknown display property");
    }
    return 0;
}

/* A window handle: Screen gives a destroyed window's slot to the next one. */
static struct win *new_window(const char *id)
{
    struct win *w = NULL;
    unsigned i;

    for (i = 1; i < nwins && !w; i++) {
        if (!wins[i].alive) {
            w = &wins[i];
        }
    }
    if (!w) {
        CHECK(nwins < 8);
        w = &wins[nwins++];
    }
    memset(w, 0, sizeof(*w));
    w->alive = 1;
    w->props[51][0] = 1; /* Screen's default: visible */
    snprintf(w->id, sizeof(w->id), "%s", id);
    return w;
}

static int fake_screen_create_window(void **pw, void *ctx)
{
    CHECK(ctx == &ctx_token);
    tr("screen_create_window");
    if (failing()) {
        return -1;
    }
    *pw = new_window("");
    return 0;
}

static int fake_screen_set_window_property_iv(void *pw, int prop, const int *v)
{
    struct win *w = pw;

    CHECK(w->alive && prop >= 0 && prop < 100);
    tr("set_iv(w%d,%d,%d,%d)", win_index(pw), prop, v[0], v[1]);
    /* Injected only while the window is being made; later sets are cared for, not set-up. */
    if (!wins[0].posts && failing()) {
        return -1;
    }
    w->props[prop][0] = v[0];
    w->props[prop][1] = v[1];
    return 0;
}

static int fake_screen_set_window_property_cv(void *pw, int prop, int len, const char *s)
{
    struct win *w = pw;

    CHECK(w->alive && prop == 20 && len > 0 && len < (int)sizeof(w->id));
    tr("set_cv(w%d,%d,%.*s)", win_index(pw), prop, len, s);
    if (failing()) {
        return -1;
    }
    memcpy(w->id, s, (unsigned)len);
    w->id[len] = 0;
    return 0;
}

static int fake_screen_set_window_property_pv(void *pw, int prop, void **v)
{
    struct win *w = pw;

    CHECK(prop == 11);
    if (!w->alive) {
        return -1; /* a CREATE event read after the renderer destroyed its window */
    }
    tr("set_pv(w%d,%d,d%d)", win_index(pw), prop, *v == &display_token[0] ? 0 : 1);
    if (failing()) {
        return -1;
    }
    w->display = *v;
    return 0;
}

static int fake_screen_get_window_property_iv(void *pw, int prop, int *v)
{
    struct win *w = pw;

    CHECK(prop >= 0 && prop < 100);
    if (!w->alive) {
        return -1;
    }
    v[0] = w->props[prop][0];
    v[1] = w->props[prop][1];
    return 0;
}

static int fake_screen_get_window_property_cv(void *pw, int prop, int len, char *s)
{
    struct win *w = pw;

    CHECK(prop == 20 && len > 0);
    if (!w->alive) {
        return -1;
    }
    snprintf(s, (unsigned)len, "%s", w->id);
    return 0;
}

static int fake_screen_get_window_property_pv(void *pw, int prop, void **v)
{
    struct win *w = pw;

    CHECK(w->alive && prop == 37);
    tr("get_pv(w%d,%d)", win_index(pw), prop);
    if (failing()) {
        return -1;
    }
    *v = &buffer_token;
    return 0;
}

static int fake_screen_create_window_group(void *pw, const char *name)
{
    struct win *w = pw;

    CHECK(w->alive);
    tr("screen_create_window_group(w%d,%s)", win_index(pw), name);
    if (failing()) {
        return -1;
    }
    snprintf(w->group, sizeof(w->group), "%s", name);
    return 0;
}

static int fake_screen_create_window_buffers(void *pw, int n)
{
    struct win *w = pw;

    CHECK(w->alive);
    tr("screen_create_window_buffers(w%d,%d)", win_index(pw), n);
    if (failing()) {
        return -1;
    }
    w->buffers = n;
    return 0;
}

static int fake_screen_post_window(void *pw, void *buf, int count, const int *rects, int flags)
{
    struct win *w = pw;

    CHECK(w->alive && buf == &buffer_token && count == 1 && rects && flags == 0);
    tr("screen_post_window(w%d,%d,%d,%d,%d)", win_index(pw), rects[0], rects[1], rects[2],
       rects[3]);
    if (failing()) {
        return -1;
    }
    w->posts++;
    return 0;
}

static int fake_screen_flush_context(void *ctx, int flags)
{
    CHECK(ctx == &ctx_token && flags == 0);
    if (failing()) {
        return -1;
    }
    return 0;
}

static int fake_screen_create_event(void **ev)
{
    tr("screen_create_event");
    if (failing()) {
        return -1;
    }
    *ev = &event_token;
    return 0;
}

static int fake_screen_get_event(void *ctx, void *ev, uint64_t timeout)
{
    CHECK(ctx == &ctx_token && ev == &event_token && timeout == 0);
    if (ev_head == ev_tail) {
        cur_event_type = 0;
        return 0;
    }
    cur_event_type = (unsigned)events[ev_head % 16].type;
    cur_event_win = events[ev_head % 16].win;
    ev_head++;
    return 0;
}

static int fake_screen_get_event_property_iv(void *ev, int prop, int *v)
{
    CHECK(ev == &event_token && prop == 47);
    *v = (int)cur_event_type;
    return 0;
}

static int fake_screen_get_event_property_pv(void *ev, int prop, void **v)
{
    CHECK(ev == &event_token && prop == 52);
    *v = &wins[cur_event_win];
    return 0;
}

/* ---- mm-renderer and libstrm ----------------------------------------------------------------- */

static int conn_token, ctx_token_mmr, stale_token, dict_token;
static int m_connected, m_ctx, m_stale, m_out, m_in, m_playing, m_ctx_params, m_in_params;
static unsigned m_connects, m_disconnects, m_creates, m_destroys, m_detaches, m_stops, m_opens;
static char m_out_url[512], m_in_url[512], dict_key[64], dict_value[64];
static unsigned in_w, in_h, video_win = 99, other_win = 99;
static struct {
    uint32_t code;
    char type[20];
    uint64_t value;
    char text[256];
} error_info;

static void *fake_mmr_connect(const char *name)
{
    CHECK(name == NULL);
    tr("mmr_connect(NULL)");
    if (failing()) {
        return NULL;
    }
    m_connected = 1;
    m_connects++;
    return &conn_token;
}

static void fake_mmr_disconnect(void *conn)
{
    CHECK(conn == &conn_token && m_connected);
    tr("mmr_disconnect");
    m_connected = 0;
    m_disconnects++;
}

static void *fake_mmr_context_create(void *conn, const char *name, unsigned flags, unsigned mode)
{
    CHECK(conn == &conn_token && m_connected && !strcmp(name, "mcn-aa-cluster") && flags == 0 &&
          mode == 0500);
    tr("mmr_context_create(%s)", name);
    if (failing() || m_stale) {
        return NULL;
    }
    CHECK(!m_ctx);
    m_ctx = 1;
    m_creates++;
    return &ctx_token_mmr;
}

static void *fake_mmr_context_open(void *conn, const char *name)
{
    CHECK(conn == &conn_token && !strcmp(name, "mcn-aa-cluster"));
    tr("mmr_context_open(%s)", name);
    m_opens++;
    return m_stale ? &stale_token : NULL;
}

static void close_video_windows(void)
{
    if (video_win < 8 && wins[video_win].alive) {
        wins[video_win].alive = 0;
        push_event(3, video_win);
    }
    if (other_win < 8 && wins[other_win].alive) {
        wins[other_win].alive = 0;
        push_event(3, other_win);
    }
}

static int fake_mmr_context_destroy(void *ctx)
{
    if (ctx == &stale_token) {
        tr("mmr_context_destroy(stale)");
        m_stale = 0;
        return 0;
    }
    CHECK(ctx == &ctx_token_mmr && m_ctx);
    tr("mmr_context_destroy");
    m_ctx = m_out = m_in = m_playing = 0;
    m_destroys++;
    close_video_windows();
    return 0;
}

static int fake_mmr_context_parameters(void *ctx, void *dict)
{
    CHECK(ctx == &ctx_token_mmr && dict == &dict_token && !strcmp(dict_key, "prefetch_mode") &&
          !strcmp(dict_value, "-1"));
    tr("mmr_context_parameters(%s=%s)", dict_key, dict_value);
    if (failing()) {
        return -1;
    }
    m_ctx_params = 1;
    return 0;
}

static const char *url_value(const char *url, const char *key, char *out, unsigned cap)
{
    const char *p = strstr(url, key);
    unsigned i = 0;

    if (!p) {
        out[0] = 0;
        return out;
    }
    p += strlen(key);
    while (*p && *p != '&' && i + 1 < cap) {
        out[i++] = *p++;
    }
    out[i] = 0;
    return out;
}

static int fake_mmr_output_attach(void *ctx, const char *url, const char *type)
{
    char group[64], id[64];
    struct win *w;

    CHECK(ctx == &ctx_token_mmr && m_ctx && !strcmp(type, "video"));
    tr("mmr_output_attach(%s,%s)", url, type);
    if (failing()) {
        return -1;
    }
    CHECK(!m_out);
    snprintf(m_out_url, sizeof(m_out_url), "%s", url);
    m_out = 1;
    /* screen_writer: a child window with the given id joins the given group; a second,
     * unnamed one comes with it. The group's owner gets both CREATE events. */
    url_value(url, "wingrp=", group, sizeof(group));
    url_value(url, "winid=", id, sizeof(id));
    CHECK(nwins >= 1 && !strcmp(wins[0].group, group));
    w = new_window("");
    other_win = (unsigned)(w - wins);
    push_event(1, other_win);
    w = new_window(id);
    video_win = (unsigned)(w - wins);
    w->props[40][0] = atoi(url_value(url, "dstw=", group, sizeof(group)));
    w->props[40][1] = atoi(url_value(url, "dsth=", group, sizeof(group)));
    w->props[54][0] = atoi(url_value(url, "zorder=", group, sizeof(group)));
    push_event(1, video_win);
    return 7;
}

static int fake_mmr_output_detach(void *ctx, unsigned id)
{
    CHECK(ctx == &ctx_token_mmr && m_ctx && id == 7 && m_out && !m_playing);
    tr("mmr_output_detach(%u)", id);
    m_out = 0;
    m_detaches++;
    close_video_windows();
    return 0;
}

static int fake_mmr_input_parameters(void *ctx, void *dict)
{
    CHECK(ctx == &ctx_token_mmr && dict == &dict_token &&
          !strcmp(dict_key, "VideoDecoderLowLatencyMode") && !strcmp(dict_value, "1"));
    tr("mmr_input_parameters(%s=%s)", dict_key, dict_value);
    if (failing()) {
        return -1;
    }
    m_in_params = 1;
    return 0;
}

static int fake_mmr_input_attach(void *ctx, const char *url, const char *type)
{
    char v[32];

    CHECK(ctx == &ctx_token_mmr && m_ctx && m_out && !strcmp(type, "track"));
    tr("mmr_input_attach(%s,%s)", url, type);
    if (failing()) {
        return -1;
    }
    CHECK(!m_in && m_in_params); /* low latency before the attach, as stock */
    snprintf(m_in_url, sizeof(m_in_url), "%s", url);
    m_in = 1;
    in_w = (unsigned)atoi(url_value(url, "video.width=", v, sizeof(v)));
    in_h = (unsigned)atoi(url_value(url, "video.height=", v, sizeof(v)));
    if (video_win < 8) {
        wins[video_win].props[5][0] = (int)in_w; /* the decoder's buffers */
        wins[video_win].props[5][1] = (int)in_h;
        if (renderer_resets_source) {
            wins[video_win].props[41][0] = wins[video_win].props[41][1] = 0;
            wins[video_win].props[42][0] = (int)in_w;
            wins[video_win].props[42][1] = (int)in_h;
        }
    }
    return 0;
}

static int fake_mmr_play(void *ctx)
{
    CHECK(ctx == &ctx_token_mmr && m_in && m_out);
    tr("mmr_play");
    if (failing()) {
        return -1;
    }
    m_playing = 1;
    return 0;
}

static int fake_mmr_stop(void *ctx)
{
    CHECK(ctx == &ctx_token_mmr && m_playing);
    tr("mmr_stop");
    m_playing = 0;
    m_stops++;
    return 0;
}

static const void *fake_mmr_error_info(void *ctx)
{
    CHECK(ctx == &ctx_token_mmr);
    tr("mmr_error_info");
    error_info.code = 5;
    snprintf(error_info.text, sizeof(error_info.text), "stand-in error text");
    return &error_info;
}

static void *fake_strm_dict_new(void)
{
    tr("strm_dict_new");
    if (failing()) {
        return NULL;
    }
    dict_key[0] = dict_value[0] = 0;
    return &dict_token;
}

static void *fake_strm_dict_set(void *d, const char *key, const char *value)
{
    CHECK(d == &dict_token);
    tr("strm_dict_set(%s,%s)", key, value);
    if (failing()) {
        return NULL;
    }
    snprintf(dict_key, sizeof(dict_key), "%s", key);
    snprintf(dict_value, sizeof(dict_value), "%s", value);
    return d;
}

/* ---- the kernel side: name_open, TimerTimeout, MsgSendvs_r ----------------------------------- */

#define COID 42
static int name_opened, name_open_fail_all, name_open_fail_first;
static unsigned name_opens, name_closes, timeouts_armed, timeout_armed_now;
static int send_rc_next, send_rc_count; /* the next send_rc_count sends answer send_rc_next */

/* Messages: counts per command, the last ones in full, and the end-to-end comparison. */
struct msg {
    unsigned cmd, len;
    uint64_t pts;
    uint8_t payload[4096];
};
static struct msg msgs[16];
static unsigned msg_count, cmd_count[8];
static uint64_t last_pts;
static int e2e;
static unsigned e2e_off, e2e_bad, e2e_aus;
static const uint8_t *e2e_config;
static unsigned e2e_config_len;

int name_open(const char *name, int flags)
{
    char v[64];

    CHECK(!strcmp(name, "/dev/name/local/mcnaamdev") && flags == 0);
    name_opens++;
    tr("name_open(%s)", name);
    if (failing() || name_open_fail_all || (name_open_fail_first && name_opens <= 3)) {
        return -1;
    }
    /* The name is served only while the renderer plays our input of that name. */
    url_value(m_in_url, "mdevpush:", v, sizeof(v));
    v[strlen("/dev/name/local/mcnaamdev")] = 0;
    CHECK(m_playing && m_in && !strcmp(v, name));
    CHECK(!name_opened);
    name_opened = 1;
    return COID;
}

int name_close(int coid)
{
    CHECK(coid == COID && name_opened);
    tr("name_close");
    name_opened = 0;
    name_closes++;
    return 0;
}

int TimerTimeout(int clock_id, int flags, const void *notify, const uint64_t *ntime,
                 uint64_t *otime)
{
    CHECK(clock_id == CLOCK_MONOTONIC && flags == 0x50 && notify == NULL && ntime &&
          *ntime == 2500000000ull && otime == NULL);
    tr("TimerTimeout");
    if (failing()) {
        return -1;
    }
    timeouts_armed++;
    timeout_armed_now = 1;
    return 0;
}

int MsgSendvs_r(int coid, const iov_t *iov, int parts, void *rmsg, int rbytes)
{
    const uint8_t *h = iov[0].iov_base;
    struct msg *m = &msgs[msg_count % 16];
    uint32_t cmd, len, total = 0;
    uint16_t type;
    unsigned i, used = 0;
    int rc;

    CHECK(coid == COID && name_opened && rmsg == NULL && rbytes == 0 && parts >= 2 && parts <= 3);
    CHECK(timeout_armed_now); /* every send is under a kernel timeout */
    timeout_armed_now = 0;
    CHECK(iov[0].iov_len == 24);
    memcpy(&type, h, 2);
    memcpy(&cmd, h + 16, 4);
    memcpy(&len, h + 20, 4);
    CHECK(type == 0);
    for (i = 2; i < 16; i++) {
        CHECK(h[i] == 0); /* the whole header zeroed */
    }
    for (i = 1; i < (unsigned)parts; i++) {
        total += iov[i].iov_len;
    }
    CHECK(len == total && len > 0 && len <= 0x400000);
    tr("MsgSendvs_r(cmd %u, len %u)", cmd, len);
    if (failing()) {
        return -89;
    }
    if (send_rc_count > 0) {
        send_rc_count--;
        rc = send_rc_next;
        if (rc) {
            return rc;
        }
    }
    m->cmd = cmd;
    m->len = len;
    m->pts = 0;
    for (i = 1; i < (unsigned)parts; i++) {
        unsigned k = iov[i].iov_len;

        if (used < sizeof(m->payload)) {
            memcpy(m->payload + used, iov[i].iov_base,
                   k < sizeof(m->payload) - used ? k : sizeof(m->payload) - used);
        }
        used += k;
    }
    if (cmd == 3) {
        CHECK(parts == 3 && iov[1].iov_len == 8);
        memcpy(&m->pts, iov[1].iov_base, 8);
        CHECK(m->pts >= last_pts);
        last_pts = m->pts;
        if (e2e) {
            const uint8_t *au = iov[2].iov_base;
            unsigned n = iov[2].iov_len;

            CHECK((au[0] == 0 && au[1] == 0 && au[2] == 1) ||
                  (au[0] == 0 && au[1] == 0 && au[2] == 0 && au[3] == 1));
            if (e2e_off + n > replay_size || memcmp(replay + e2e_off, au, n)) {
                e2e_bad++;
            }
            e2e_off += n;
            e2e_aus++;
            if (e2e_off >= replay_size) {
                e2e_off -= replay_size; /* the lab plays the file in a loop */
            }
        }
    } else if (cmd == 4 && e2e) {
        CHECK(len == e2e_config_len && !memcmp(iov[1].iov_base, e2e_config, len));
    }
    msg_count++;
    cmd_count[cmd & 7]++;
    return 0;
}

static const struct msg *last_msg(unsigned back)
{
    CHECK(msg_count > back);
    return &msgs[(msg_count - 1 - back) % 16];
}

/* ---- dlopen / dlsym ------------------------------------------------------------------------ */

static int lib_mmr, lib_strm, lib_screen;
static const char *missing;

void *mcn_test_dlopen(const char *name, int flags)
{
    CHECK(flags == (RTLD_LAZY | RTLD_GLOBAL));
    if (!strcmp(name, "libmmrndclient.so.1")) {
        return &lib_mmr;
    }
    if (!strcmp(name, "libstrm.so.1")) {
        return &lib_strm;
    }
    if (!strcmp(name, "libscreen.so.1")) {
        return &lib_screen;
    }
    CHECK(!"unexpected library");
    return NULL;
}

void *mcn_test_dlsym(void *lib, const char *name)
{
    static const struct {
        const char *name;
        void *at;
        int *lib;
    } exports[] = {
        { "mmr_connect", (void *)fake_mmr_connect, &lib_mmr },
        { "mmr_disconnect", (void *)fake_mmr_disconnect, &lib_mmr },
        { "mmr_context_create", (void *)fake_mmr_context_create, &lib_mmr },
        { "mmr_context_open", (void *)fake_mmr_context_open, &lib_mmr },
        { "mmr_context_destroy", (void *)fake_mmr_context_destroy, &lib_mmr },
        { "mmr_context_parameters", (void *)fake_mmr_context_parameters, &lib_mmr },
        { "mmr_output_attach", (void *)fake_mmr_output_attach, &lib_mmr },
        { "mmr_output_detach", (void *)fake_mmr_output_detach, &lib_mmr },
        { "mmr_input_parameters", (void *)fake_mmr_input_parameters, &lib_mmr },
        { "mmr_input_attach", (void *)fake_mmr_input_attach, &lib_mmr },
        { "mmr_play", (void *)fake_mmr_play, &lib_mmr },
        { "mmr_stop", (void *)fake_mmr_stop, &lib_mmr },
        { "mmr_error_info", (void *)fake_mmr_error_info, &lib_mmr },
        { "strm_dict_new", (void *)fake_strm_dict_new, &lib_strm },
        { "strm_dict_set", (void *)fake_strm_dict_set, &lib_strm },
        { "screen_create_context", (void *)fake_screen_create_context, &lib_screen },
        { "screen_get_context_property_iv", (void *)fake_screen_get_context_property_iv,
          &lib_screen },
        { "screen_get_context_property_pv", (void *)fake_screen_get_context_property_pv,
          &lib_screen },
        { "screen_get_display_property_iv", (void *)fake_screen_get_display_property_iv,
          &lib_screen },
        { "screen_create_window", (void *)fake_screen_create_window, &lib_screen },
        { "screen_set_window_property_iv", (void *)fake_screen_set_window_property_iv,
          &lib_screen },
        { "screen_set_window_property_cv", (void *)fake_screen_set_window_property_cv,
          &lib_screen },
        { "screen_set_window_property_pv", (void *)fake_screen_set_window_property_pv,
          &lib_screen },
        { "screen_get_window_property_iv", (void *)fake_screen_get_window_property_iv,
          &lib_screen },
        { "screen_get_window_property_cv", (void *)fake_screen_get_window_property_cv,
          &lib_screen },
        { "screen_get_window_property_pv", (void *)fake_screen_get_window_property_pv,
          &lib_screen },
        { "screen_create_window_group", (void *)fake_screen_create_window_group, &lib_screen },
        { "screen_create_window_buffers", (void *)fake_screen_create_window_buffers,
          &lib_screen },
        { "screen_post_window", (void *)fake_screen_post_window, &lib_screen },
        { "screen_flush_context", (void *)fake_screen_flush_context, &lib_screen },
        { "screen_create_event", (void *)fake_screen_create_event, &lib_screen },
        { "screen_get_event", (void *)fake_screen_get_event, &lib_screen },
        { "screen_get_event_property_iv", (void *)fake_screen_get_event_property_iv,
          &lib_screen },
        { "screen_get_event_property_pv", (void *)fake_screen_get_event_property_pv,
          &lib_screen },
    };
    unsigned i;

    for (i = 0; i < sizeof(exports) / sizeof(exports[0]); i++) {
        if (!strcmp(name, exports[i].name)) {
            CHECK(lib == exports[i].lib);
            return missing && !strcmp(name, missing) ? NULL : exports[i].at;
        }
    }
    CHECK(!"unexpected symbol asked for");
    return NULL;
}

/* ---- test streams ---------------------------------------------------------------------------- */

/* An 800x480 Baseline SPS and PPS (the stock player's own black-frame blob, notes 1.1). */
static const uint8_t sps_pps[] = { 0, 0, 0, 1, 0x67, 0x42, 0x80, 0x1f, 0xda, 0x03, 0x20, 0xf6,
                                   0x80, 0x6d, 0x0a, 0x13, 0x50, 0, 0, 0, 1, 0x68, 0xce, 0x06,
                                   0xe2 };
static uint8_t idr_unit[64], p_unit[48];
static unsigned idr_len, p_len;

static void make_units(void)
{
    static const uint8_t idr_slice[] = { 0, 0, 0, 1, 0x65, 0xb8, 0x41, 0x7f, 0x11, 0x22 };
    static const uint8_t p_slice[] = { 0, 0, 0, 1, 0x41, 0xe2, 0x07, 0x7f, 0x33, 0x44 };

    memcpy(idr_unit, sps_pps, sizeof(sps_pps));
    memcpy(idr_unit + sizeof(sps_pps), idr_slice, sizeof(idr_slice));
    idr_len = sizeof(sps_pps) + sizeof(idr_slice);
    memcpy(p_unit, p_slice, sizeof(p_slice));
    p_len = sizeof(p_slice);
}

static void put_idr(unsigned token)
{
    mcn_decode_put(token, idr_unit, idr_len, 1, sps_pps, sizeof(sps_pps));
}

static void put_p(unsigned token)
{
    mcn_decode_put(token, p_unit, p_len, 0, NULL, 0);
}

static void reset_counts(void)
{
    calls = 0;
    fail_at = 0;
    injected = 0;
    msg_count = 0;
    memset(cmd_count, 0, sizeof(cmd_count));
    trace_reset();
}

/* The resources of a stopped session are all given back; the window stays, hidden. */
static void check_released(void)
{
    CHECK(!m_connected && !m_ctx && !m_out && !m_in && !m_playing && !name_opened);
    CHECK(m_connects == m_disconnects && m_creates == m_destroys);
    CHECK(nwins >= 1 ? wins[0].alive : 1);
    if (nwins >= 1 && wins[0].posts) { /* an unfinished window has nothing to show */
        CHECK(wins[0].props[51][0] == 0);
    }
}

/* Everything the first set-up must do, in order (notes, sections 4.4, 6 and 7a). */
static const char expected_first_setup[] =
    "screen_create_context(0)\n"
    "screen_get_context_property_iv(59)\n"
    "screen_get_context_property_pv(60)\n"
    "screen_get_display_property_iv(d0,87)\n"
    "screen_get_display_property_iv(d0,40)\n"
    "screen_get_display_property_iv(d1,87)\n"
    "screen_get_display_property_iv(d1,40)\n"
    "screen_create_window\n"
    "set_cv(w0,20,mcnAaClusterClone1)\n"
    "set_iv(w0,51,0,0)\n"
    "set_pv(w0,11,d1)\n"
    "screen_create_window_group(w0,mcnAaClusterGroup)\n"
    "set_iv(w0,46,3,0)\n"
    "set_iv(w0,93,0,0)\n"
    "set_iv(w0,5,1,1)\n"
    "set_iv(w0,35,0,0)\n"
    "set_iv(w0,40,1023,384)\n"
    "set_iv(w0,42,1023,384)\n"
    "set_iv(w0,41,-1023,-384)\n"
    "set_iv(w0,54,500,0)\n"
    "screen_create_window_buffers(w0,1)\n"
    "get_pv(w0,37)\n"
    "screen_post_window(w0,0,0,1,1)\n"
    "screen_create_event\n"
    "mmr_connect(NULL)\n"
    "mmr_context_create(mcn-aa-cluster)\n"
    "strm_dict_new\n"
    "strm_dict_set(prefetch_mode,-1)\n"
    "mmr_context_parameters(prefetch_mode=-1)\n"
    "mmr_output_attach(screen:/?index=1&wingrp=mcnAaClusterGroup&winid=mcnAaVideoClone1&dstx=0&dsty=0"
    "&dstw=1023&dsth=384&overlay=0&zorder=1,video)\n"
    "strm_dict_new\n"
    "strm_dict_set(VideoDecoderLowLatencyMode,1)\n"
    "mmr_input_parameters(VideoDecoderLowLatencyMode=1)\n"
    "mmr_input_attach(mdevpush:/dev/name/local/mcnaamdev?fourcc=H264&video.frame_rate=30"
    "&video.depth=16&video.width=800&video.height=480&video.duration=0,track)\n"
    "mmr_play\n"
    "name_open(/dev/name/local/mcnaamdev)\n"
    "TimerTimeout\n"
    "MsgSendvs_r(cmd 5, len 4)\n"
    "TimerTimeout\n"
    "MsgSendvs_r(cmd 2, len 32)\n"
    /* the renderer's windows joined our group: both go onto the cluster display, the named
     * one is cropped and placed */
    "set_pv(w1,11,d1)\n"
    "set_pv(w2,11,d1)\n"
    "set_iv(w2,41,0,90)\n"
    "set_iv(w2,42,800,300)\n"
    "set_iv(w2,35,0,0)\n"
    "set_iv(w2,40,1023,384)\n"
    "set_iv(w2,54,1,0)\n"
    "set_iv(w2,51,0,0)\n"
    "TimerTimeout\n"
    "MsgSendvs_r(cmd 4, len 25)\n"
    "TimerTimeout\n"
    "MsgSendvs_r(cmd 3, len 43)\n"
    /* the first key frame is out: the window shows */
    "set_iv(w0,51,1,0)\n"
    "set_iv(w0,54,500,0)\n"
    "set_iv(w2,51,1,0)\n";

/* The end of a session, in the stock player's order, after hiding both windows. */
static const char *expected_teardown(void)
{
    static char text[256];

    snprintf(text, sizeof(text),
             "set_iv(w0,51,0,0)\nset_iv(w%u,51,0,0)\nname_close\nmmr_stop\nmmr_output_detach(7)\n"
             "mmr_context_destroy\nmmr_disconnect\n",
             video_win);
    return text;
}

/* The trace since `from` starts with `expected`. */
static void trace_is(unsigned from, const char *expected)
{
    if (strncmp(trace + from, expected, strlen(expected))) {
        write(2, "--- trace ---\n", 14);
        write(2, trace + from, strlen(trace + from));
        write(2, "--- expected ---\n", 17);
        write(2, expected, strlen(expected));
        CHECK(!"trace differs");
    }
}

/* ---- the lab player on the recording --------------------------------------------------------- */

static void *lab_jump[5];
static unsigned lab_starts_wanted = 2, lab_units_wanted;

static void lab_tick(void)
{
    drain();
    if (cmd_count[3] >= lab_units_wanted && m_creates >= lab_starts_wanted) {
        lab_driving = 0;
        __builtin_longjmp(lab_jump, 1);
    }
}

static unsigned load_replay(const char *path)
{
    int fd = open(path, O_RDONLY), n;
    off_t size;
    uint8_t *p;

    CHECK(fd >= 0);
    size = lseek(fd, 0, SEEK_END);
    CHECK(size > 0 && lseek(fd, 0, SEEK_SET) == 0);
    p = malloc((size_t)size);
    CHECK(p);
    replay = p;
    replay_size = (unsigned)size;
    while ((unsigned)(p - replay) < replay_size &&
           (n = read(fd, p, replay_size - (unsigned)(p - replay))) > 0) {
        p += n;
    }
    CHECK((unsigned)(p - replay) == replay_size);
    close(fd);
    return replay_size;
}

/* Access units of the recording, counted on our own: slices with first_mb_in_slice 0. */
static unsigned count_units(void)
{
    unsigned i, n = 0;

    for (i = 0; i + 4 < replay_size; i++) {
        if (replay[i] == 0 && replay[i + 1] == 0 && replay[i + 2] == 1 &&
            ((replay[i + 3] & 0x1f) == 1 || (replay[i + 3] & 0x1f) == 5) &&
            (replay[i + 4] & 0x80)) {
            n++;
        }
    }
    return n;
}

/* ---- the checks ---------------------------------------------------------------------------- */

static void check_missing_symbol(void)
{
    unsigned token, from = log_count;

    missing = "screen_create_window_group";
    sw_file = 1;
    token = mcn_decode_begin(800, 480);
    CHECK(token && threads == 1);
    put_idr(token);
    stock_state = 1;
    drain();
    CHECK(!mcn_decode_live(token));
    CHECK(logged(from, "missing symbol screen_create_window_group") == 1);
    CHECK(logged(from, "functions missing") >= 1);
    CHECK(!m_connects && !nwins); /* nothing was set up without every function */
    put_p(token);
    drain();
    CHECK(!msg_count);
    mcn_decode_end(token);
    drain();
    check_released();
    puts("decode (missing symbol): stays off, nothing touched: ok");
}

static void check_switch(void)
{
    /* On by default; off with MCN_DIR/aa-passthrough, or the card flag on any of mp000..mp009. */
    CHECK(!mcn_decode_on()); /* sw_file 0: the pass-through file exists */
    sw_file = 1;
    CHECK(mcn_decode_on());
    sw_card = 0;
    CHECK(!mcn_decode_on());
    sw_card = 9;
    CHECK(!mcn_decode_on());
    sw_card = -1;
    CHECK(mcn_decode_on());
    sw_file = 0;
    CHECK(!mcn_decode_on());
}

static void check_first_stream(void)
{
    unsigned token, from = log_count, t;

    stock_state = 1;
    stock_playing_now = 1;
    reset_counts();
    CHECK(!mcn_decode_begin(0, 0) && !mcn_decode_begin(4000, 480));
    token = mcn_decode_begin(800, 480);
    CHECK(token && threads == 1); /* the worker thread exists once */
    /* Nothing is set up for a stream that has not sent a key frame. */
    put_p(token);
    drain();
    CHECK(!m_connects && !nwins && mcn_decode_want_key(token));
    CHECK(logged(from, "stock video context plays") == 0);
    put_idr(token);
    CHECK(!mcn_decode_want_key(token));
    drain();
    CHECK(mcn_decode_live(token));
    trace_is(0, expected_first_setup);
    CHECK(logged(from, "stock video context plays") == 1);
    CHECK(logged(from, "display index 1: id 6, 1024x384") == 1);
    CHECK(logged(from, "window ready on the cluster display") == 1);
    CHECK(logged(from, "video window shows 800x300 at (0,90) of 800x480 in 1023x384") == 1);
    CHECK(logged(from, "window joined our group: id 'mcnAaVideoClone1'") == 1);
    CHECK(logged(from, "window joined our group: id ''") == 1);
    CHECK(logged(from, "first key frame pushed") == 1);
    CHECK(logged(from, "window shown") == 1);
    /* The messages: command 5 = 1, command 2 = the format, command 4 = SPS and PPS as they
     * came, command 3 = PTS then the unit. */
    CHECK(msg_count == 4 && cmd_count[5] == 1 && cmd_count[2] == 1 && cmd_count[4] == 1 &&
          cmd_count[3] == 1);
    CHECK(last_msg(3)->cmd == 5 && last_msg(3)->len == 4 &&
          !memcmp(last_msg(3)->payload, "\1\0\0\0", 4));
    {
        static const uint32_t format[8] = { 2, 0x48323634u, 800, 480, 16, 30, 1, 0 };

        CHECK(last_msg(2)->cmd == 2 && last_msg(2)->len == 32 &&
              !memcmp(last_msg(2)->payload, format, 32));
    }
    CHECK(last_msg(1)->cmd == 4 && last_msg(1)->len == sizeof(sps_pps) &&
          !memcmp(last_msg(1)->payload, sps_pps, sizeof(sps_pps)));
    CHECK(last_msg(0)->cmd == 3 && last_msg(0)->len == 8 + idr_len &&
          !memcmp(last_msg(0)->payload + 8, idr_unit, idr_len));
    CHECK(last_msg(0)->pts == (uint64_t)now_ms * 1000 + 50000);
    /* The window: on the cluster display, our group, shown at 500; the video window cropped. */
    CHECK(nwins == 3 && wins[0].display == &display_token[1] &&
          !strcmp(wins[0].id, "mcnAaClusterClone1") && !strcmp(wins[0].group, "mcnAaClusterGroup"));
    CHECK(wins[0].props[51][0] == 1 && wins[0].props[54][0] == 500 && wins[0].posts == 1 &&
          wins[0].buffers == 1 && wins[0].props[40][0] == 1023 && wins[0].props[40][1] == 384);
    CHECK(wins[2].props[41][0] == 0 && wins[2].props[41][1] == 90 && wins[2].props[42][0] == 800 &&
          wins[2].props[42][1] == 300 && wins[2].props[40][0] == 1023 && wins[2].props[51][0] == 1);
    CHECK(wins[1].props[41][0] == 0 && wins[1].props[42][0] == 0); /* the unnamed one: untouched */

    /* Delta frames follow; a key frame with new parameter sets sends command 4 again, one
     * without new ones does not. */
    t = trace_len;
    put_p(token);
    put_p(token);
    drain();
    CHECK(cmd_count[3] == 3 && cmd_count[4] == 1);
    trace_is(t, "TimerTimeout\nMsgSendvs_r(cmd 3, len 18)\nTimerTimeout\nMsgSendvs_r(cmd 3, len 18)\n");
    mcn_decode_put(token, idr_unit, idr_len, 1, NULL, 0);
    drain();
    CHECK(cmd_count[3] == 4 && cmd_count[4] == 1);
    put_idr(token);
    drain();
    CHECK(cmd_count[3] == 5 && cmd_count[4] == 2);

    /* DisplayManager's one-shot: the window hidden and at z-order 0 from outside; the next
     * tick puts it back. The renderer laying its window out again is put back too. */
    wins[0].props[51][0] = 0;
    wins[0].props[54][0] = 0;
    wins[2].props[42][0] = 800;
    wins[2].props[42][1] = 480;
    wins[2].props[41][1] = 0;
    now_ms += 600;
    from = log_count;
    drain();
    CHECK(wins[0].props[51][0] == 1 && wins[0].props[54][0] == 500);
    CHECK(wins[2].props[42][1] == 300 && wins[2].props[41][1] == 90);
    CHECK(logged(from, "window was changed from outside (visible 0, z-order 0)") == 1);
    CHECK(logged(from, "video window source was 800x480 at (0,0): set again") == 1);

    /* The mod switched off: the window hides, decoding goes on; on again: shown. */
    active = 0;
    now_ms += 600;
    drain();
    CHECK(wins[0].props[51][0] == 0 && wins[2].props[51][0] == 0 && mcn_decode_live(token));
    put_p(token);
    drain();
    CHECK(cmd_count[3] == 6);
    active = 1;
    now_ms += 600;
    drain();
    CHECK(wins[0].props[51][0] == 1 && wins[2].props[51][0] == 1);

    /* Every 10 s a line with the numbers, the dpy-6 counter and the capture state. */
    from = log_count;
    dpy_counter_value += 133;
    now_ms += 10000;
    drain();
    now_ms += 10000;
    dpy_counter_value += 100;
    drain();
    CHECK(logged(from, "decode: 10 s: pushed") == 2);
    CHECK(logged(from, "dpy-6 composed +100, capture on") == 1);
    dpy_capturing = 0;
    now_ms += 10000;
    drain();
    CHECK(logged(from, "capture off") == 1);
    dpy_capturing = 1;

    /* The queue: filled beyond its bound without the worker running, units are dropped until
     * a key frame, said once; the key frame clears the request. */
    from = log_count;
    for (t = 0; t < 200; t++) {
        put_p(token);
    }
    CHECK(mcn_decode_want_key(token));
    CHECK(logged(from, "queue full") == 1);
    put_p(token);
    CHECK(logged(from, "queue full") == 1);
    drain();
    CHECK(cmd_count[3] == 6 + 128);
    CHECK(mcn_decode_want_key(token));
    put_p(token);
    drain();
    CHECK(cmd_count[3] == 6 + 128); /* still waiting for the key frame */
    put_idr(token);
    CHECK(!mcn_decode_want_key(token));
    put_p(token);
    drain();
    CHECK(cmd_count[3] == 6 + 128 + 2 && cmd_count[4] == 3);
    now_ms += 10000;
    drain();
    /* One unit lost at the full queue, the rest waited for the key frame. */
    CHECK(logged(from, "dropped 1 at the queue, 73 waiting for a key frame") == 1);

    /* A unit the renderer dropped (-16): the stream goes on, a key frame is asked for, and
     * the units until it come are not sent. */
    from = log_count;
    send_rc_next = -16;
    send_rc_count = 1;
    put_p(token);
    drain();
    CHECK(cmd_count[3] == 136 && mcn_decode_live(token));
    CHECK(logged(from, "the renderer dropped an access unit (rc -16") == 1);
    put_p(token);
    CHECK(mcn_decode_want_key(token));
    put_p(token);
    drain();
    CHECK(cmd_count[3] == 136);
    put_idr(token);
    put_p(token);
    drain();
    CHECK(cmd_count[3] == 138 && !mcn_decode_want_key(token));
    /* Five dropped in a row give the session up (key frames, so that each one is sent). */
    from = log_count;
    send_rc_next = -16;
    send_rc_count = 5;
    for (t = 0; t < 5; t++) {
        mcn_decode_put(token, idr_unit, idr_len, 1, NULL, 0);
        drain();
    }
    CHECK(!mcn_decode_live(token));
    check_released();
    CHECK(logged(from, "sending an access unit failed, rc -16") == 1);
    CHECK(logged(from, "stopping (the renderer's input failed") == 1);
    put_idr(token);
    drain();
    CHECK(!m_connects || !m_connected); /* a failed stream is not started again */
    mcn_decode_end(token);
    drain();

    /* The next stream (reconnect): no second window, the same sequence otherwise, and the
     * teardown in the stock order when it ends. The caller's own loss asks for a key frame. */
    reset_counts();
    token = mcn_decode_begin(800, 480);
    put_idr(token);
    drain();
    CHECK(mcn_decode_live(token) && nwins == 3 && wins[0].alive && video_win == 2);
    trace_is(0, "mmr_connect(NULL)\nmmr_context_create(mcn-aa-cluster)\n");
    CHECK(!strstr(trace, "screen_create_window\n"));
    mcn_decode_lost(token);
    CHECK(mcn_decode_want_key(token));
    put_p(token);
    CHECK(mcn_decode_want_key(token));
    put_idr(token);
    CHECK(!mcn_decode_want_key(token));
    drain();
    CHECK(cmd_count[3] == 2);
    /* An oversized unit is dropped and asks for a key frame. */
    {
        static uint8_t big[800 * 480 * 2];

        memcpy(big, p_unit, p_len);
        mcn_decode_put(token, big, sizeof(big) - 32, 0, NULL, 0);
        CHECK(mcn_decode_want_key(token));
        drain();
        CHECK(cmd_count[3] == 2);
        put_idr(token);
        drain();
        CHECK(cmd_count[3] == 3 && !mcn_decode_want_key(token));
    }
    /* Stale tokens are ignored; the end tears down in order. */
    mcn_decode_put(token + 1, idr_unit, idr_len, 1, sps_pps, sizeof(sps_pps));
    mcn_decode_end(token + 1);
    drain();
    CHECK(mcn_decode_live(token) && cmd_count[3] == 3);
    t = trace_len;
    mcn_decode_end(token);
    CHECK(!mcn_decode_want_key(token));
    drain();
    trace_is(t, expected_teardown());
    check_released();
    CHECK(!mcn_decode_live(token) && wins[0].alive && !wins[1].alive && !wins[2].alive);
    puts("decode (stream): set-up order, URLs, message layout, window, crop, re-assert, "
         "queue overflow, renderer drops, teardown order: ok");
}

/* A fatal send error (-89) stops the session cleanly. */
static void check_fatal_send(void)
{
    unsigned token, from = log_count;

    reset_counts();
    token = mcn_decode_begin(800, 480);
    put_idr(token);
    drain();
    CHECK(mcn_decode_live(token));
    send_rc_next = -89;
    send_rc_count = 1;
    put_p(token);
    drain();
    CHECK(!mcn_decode_live(token));
    check_released();
    CHECK(logged(from, "sending an access unit failed, rc -89") == 1);
    mcn_decode_end(token);
    drain();
}

/* The stock context's state: ours waits for it to play, at most SETTLE_MS. */
static void check_settle(void)
{
    unsigned token, from = log_count, start;

    reset_counts();
    stock_state = 1;
    stock_playing_now = 0;
    token = mcn_decode_begin(800, 480);
    put_idr(token);
    start = now_ms;
    while (!mcn_decode_live(token) && now_ms - start < 10000) {
        drain();
        now_ms += 100;
    }
    CHECK(mcn_decode_live(token) && now_ms - start >= 3000 && now_ms - start < 3500);
    CHECK(logged(from, "stock video context not playing after 3") == 1);
    mcn_decode_end(token);
    drain();
    reset_counts();
    stock_state = 0;
    from = log_count;
    token = mcn_decode_begin(800, 480);
    put_idr(token);
    start = now_ms;
    while (!mcn_decode_live(token) && now_ms - start < 10000) {
        drain();
        now_ms += 100;
    }
    CHECK(mcn_decode_live(token) && now_ms - start >= 3000);
    CHECK(logged(from, "stock video context absent after 3") == 1);
    mcn_decode_end(token);
    drain();
    reset_counts();
    stock_state = 1;
    stock_playing_now = 1;
    from = log_count;
    token = mcn_decode_begin(800, 480);
    put_idr(token);
    start = now_ms;
    drain();
    CHECK(mcn_decode_live(token) && now_ms == start);
    CHECK(logged(from, "stock video context plays after 0 ms") == 1);
    mcn_decode_end(token);
    drain();
}

/* A context left behind by a crashed SAL is opened, destroyed and made anew. */
static void check_stale_context(void)
{
    unsigned token;

    reset_counts();
    m_stale = 1;
    token = mcn_decode_begin(800, 480);
    put_idr(token);
    drain();
    CHECK(mcn_decode_live(token) && m_opens == 1 && !m_stale);
    CHECK(strstr(trace, "mmr_context_create(mcn-aa-cluster)\nmmr_context_open(mcn-aa-cluster)\n"
                        "mmr_context_destroy(stale)\nmmr_context_create(mcn-aa-cluster)\n"));
    mcn_decode_end(token);
    drain();
    check_released();
}

/* The name that never opens, and one that opens at the fourth try. */
static void check_name_open(void)
{
    unsigned token, from = log_count, sleeps;

    reset_counts();
    name_open_fail_all = 1;
    sleeps = sleeps_100ms;
    token = mcn_decode_begin(800, 480);
    put_idr(token);
    drain();
    CHECK(!mcn_decode_live(token) && sleeps_100ms - sleeps == 29);
    CHECK(logged(from, "name_open /dev/name/local/mcnaamdev -> -1 FAILED") == 1);
    CHECK(logged(from, "the renderer's input name does not open") == 1);
    check_released();
    name_open_fail_all = 0;
    mcn_decode_end(token);
    drain();
    reset_counts();
    name_open_fail_first = 1;
    name_opens = 0;
    sleeps = sleeps_100ms;
    token = mcn_decode_begin(800, 480);
    put_idr(token);
    drain();
    CHECK(mcn_decode_live(token) && sleeps_100ms - sleeps == 3);
    name_open_fail_first = 0;
    mcn_decode_end(token);
    drain();
    check_released();
}

/*
 * Every call of a set-up that fails leads to a clean stop, and the next stream recovers. The
 * fail_at-th call fails; a failure the module may live with (a display property of another
 * display, one name_open try, a flush after the set-up) leaves the stream running. Returns the
 * number of failures that stopped a stream, and counts those that stopped it for want of the
 * window (the window steps are only passed once per process).
 */
static unsigned every_failure(unsigned *window_failures, unsigned *tolerated)
{
    unsigned token, from, failures = 0;

    *window_failures = 0;
    *tolerated = 0;
    for (fail_at = 1;; fail_at++) {
        calls = 0;
        injected = 0;
        msg_count = 0;
        memset(cmd_count, 0, sizeof(cmd_count));
        from = log_count;
        token = mcn_decode_begin(800, 480);
        put_idr(token);
        put_p(token);
        drain();
        if (!injected) {
            CHECK(mcn_decode_live(token) && cmd_count[3] == 2);
            mcn_decode_end(token);
            drain();
            break;
        }
        if (mcn_decode_live(token)) {
            (*tolerated)++;
            mcn_decode_end(token);
            drain();
            check_released();
            continue;
        }
        failures++;
        check_released();
        CHECK(logged(from, "FAILED") + logged(from, "failed") >= 1);
        if (logged(from, "no window on the cluster display")) {
            (*window_failures)++;
            CHECK(!m_connects);
        }
        CHECK(logged(from, "the cluster shows the built-in map") +
                  logged(from, "this stream is not decoded any more") ==
              1);
        /* The failed stream stays off even if more units come. */
        put_idr(token);
        drain();
        CHECK(!mcn_decode_live(token) && !m_connected);
        mcn_decode_end(token);
        drain();
    }
    fail_at = 0;
    check_released();
    return failures;
}

static void check_every_failure(void)
{
    unsigned window_failures, tolerated, failures;

    failures = every_failure(&window_failures, &tolerated);
    {
        char line[128];

        snprintf(line, sizeof(line), "failures %u, window %u, tolerated %u", failures,
                 window_failures, tolerated);
        if (failures != 21 || window_failures || tolerated != 5) {
            write(2, line, strlen(line));
            write(2, "\n", 1);
        }
    }
    /* The window exists: connect, create, two dictionaries, parameters, attach, parameters,
     * attach, play, five timeouts and five sends (commands 5, 2, 4, 3, 3); one name_open try,
     * two flushes after the set-up and the two moves of the renderer's windows to the cluster
     * display are lived with. */
    CHECK(failures == 21 && window_failures == 0 && tolerated == 5);
    puts("decode (failures): every failing renderer, dictionary, name and send call stops "
         "cleanly and the next stream recovers: ok");
}

/*
 * Own process per k: the k-th call of the first set-up fails. The window is made once per
 * process and a tolerated failure completes it, so one process can only try one of its calls.
 * Calls 1..26 make the window (context, count, list, two properties of each of two displays,
 * window, id, visible 0, display, flush, group, eight properties, buffers, render buffer, post,
 * event, flush); 4 and 5 are the other display's properties and are lived with; 27 is
 * mmr_connect.
 */
static void check_window_failure(unsigned k)
{
    unsigned token, from = log_count;
    char line[128];

    stock_state = 1;
    fail_at = k;
    token = mcn_decode_begin(800, 480);
    put_idr(token);
    put_p(token);
    drain();
    CHECK(injected == 1);
    if (k == 4 || k == 5) {
        CHECK(mcn_decode_live(token) && cmd_count[3] == 2);
        CHECK(logged(from, "display index 0: id") == 1 && logged(from, "FAILED") == 0);
    } else if (k <= 26) {
        CHECK(!mcn_decode_live(token) && !m_connects && (nwins == 0) == (k <= 8));
        CHECK(logged(from, "FAILED") == 1 || logged(from, "no display with id 6") == 1);
        CHECK(logged(from, "no window on the cluster display") == 1);
        check_released();
    } else {
        CHECK(!mcn_decode_live(token) && !m_connects && wins[0].posts == 1);
        CHECK(logged(from, "mmr_connect -> 0 FAILED") == 1);
        check_released();
    }
    mcn_decode_end(token);
    drain();
    /* The next stream completes the window where it stopped and runs. */
    fail_at = 0;
    from = log_count;
    token = mcn_decode_begin(800, 480);
    put_idr(token);
    drain();
    CHECK(mcn_decode_live(token) && nwins == 3 && wins[0].alive && wins[0].posts == 1 &&
          !strcmp(wins[0].id, "mcnAaClusterClone1") && wins[0].props[51][0] == 1);
    CHECK(logged(from, "screen_create_context") == (k == 1));
    mcn_decode_end(token);
    drain();
    check_released();
    snprintf(line, sizeof(line), "decode (window, call %u fails): %s: ok", k,
             k == 4 || k == 5 ? "lived with" : k <= 26 ? "clean stop, finished next time"
                                                       : "no Screen call left to fail");
    puts(line);
}

static void check_lab(void)
{
    unsigned units = count_units(), from = log_count;
    uint8_t config[64];
    unsigned config_len;

    /* The recording's first SPS and PPS, with 4-byte start codes, as the lab hands them over. */
    config_len = aa_annexb_parameter_sets(replay, 64, config, sizeof(config), 0);
    CHECK(config_len == 29 && !memcmp(config, replay, 29));
    e2e_config = config;
    e2e_config_len = config_len;
    reset_counts();
    m_creates = m_destroys = 0;
    mcn_lab_start();
    CHECK(threads == 2 && thread_fn[1]);
    CHECK(logged(from, "lab video: parts 2") == 1 || logged(from, "lab video: parts ") == 1);
    e2e = 1;
    e2e_off = 0;
    link_video = 0;
    sw_file = 1;
    lab_units_wanted = units + 1; /* the whole file, then the first unit of the next loop */
    lab_starts_wanted = 2;
    if (__builtin_setjmp(lab_jump) == 0) {
        lab_driving = 1;
        thread_fn[1](NULL);
        CHECK(!"the lab player returned");
    }
    CHECK(cmd_count[3] == units + 1 && e2e_aus == units + 1 && !e2e_bad && e2e_off > 0);
    /* Three key frames with parameter sets in the file: command 4 before each, and again
     * after the restart; 5 and 2 once per decoder. */
    CHECK(cmd_count[4] == 4 && cmd_count[5] == 2 && cmd_count[2] == 2);
    CHECK(m_creates == 2 && m_destroys == 1);
    CHECK(link_video >= 60 && mcn_decode_live(0) == 0);
    CHECK(logged(from, "lab video: decode mode") == 1);
    CHECK(strstr(trace, "name_close\nmmr_stop\nmmr_output_detach(7)\nmmr_context_destroy\n"
                        "mmr_disconnect\n"));
    CHECK(logged(from, "stopping (another stream begins)") == 1);
    /* No unit was lost on the way: nothing dropped, nothing waiting. */
    CHECK(logged(from, "queue full") == 0 && logged(from, "waiting for a key frame") >= 1);
    CHECK(logged(from, "dropped 0 at the queue, 0 waiting for a key frame") ==
          logged(from, "decode: 10 s: pushed"));
    e2e = 0;
    puts("decode (lab): the recording end to end, SPS and PPS first, complete access units, "
         "count matches, restart at the loop: ok");
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[argc - 1] : NULL;

    make_units();
    if (argc > 1 && !strcmp(argv[1], "missing")) {
        check_missing_symbol();
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "window")) {
        check_window_failure((unsigned)atoi(argv[2]));
        return 0;
    }
    CHECK(path);
    load_replay(path);
    check_switch();
    check_first_stream();
    check_fatal_send();
    check_settle();
    check_stale_context();
    check_name_open();
    check_every_failure();
    check_lab();
    puts("decode: ok");
    return 0;
}

/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Android Auto branch. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * "Decode mode": the phone's cluster stream is decoded on the head unit instead of being
 * handed to the cluster as it is. The phone can only send 800x480 or 1280x720, and the
 * cluster's decoder freezes when the coded size changes from the stock 1024x384 (car,
 * 2026-11-09). So the stream goes to a second mm-renderer context of our own, exactly the way
 * the stock SAL plays the phone's main video, and its picture is put into a Screen window on
 * the cluster source display (dpy-6, 1024x384). DisplayManager captures that display and
 * encodes it for the cluster in the stock geometry; nothing after the encoder changes.
 *
 * Where the facts come from (notes taken from the P0478T binaries, 2026-11-09):
 *   - sal-video-player.md: the mm-renderer call sequence of H264VideoPlayer, the "mdevpush:"
 *     input, the mdev message formats (commands 5, 2, 4, 3), PTS, replies and teardown;
 *   - displaymanager-cluster.md: how a foreign window gets into the encoded cluster picture
 *     and the traps of DisplayManager's CREATE handler (ID string ending in "Clone1", never
 *     destroy, re-assert visibility and z-order).
 * Proven in a car on the sister firmware P0480T (same DisplayManager binary) by the project
 * mst2-altscreen-carplay (GPL-3.0-or-later; docs/PROTOCOL-MDEVPUSH.md, docs/ARCHITECTURE.md,
 * docs/FINDINGS.md): a second mm-renderer context next to the stock one decodes and is
 * captured; "overlay=0" (an overlay plane is not captured); a window one pixel narrower than
 * the display (DisplayManager captures when the display is composed, and that is driven by
 * the stock map underneath: fully covered, it fell to 0.4-4 fps); destroying an mm-renderer
 * window that DisplayManager still has on its list of unnamed windows wedges its capture.
 * Only those findings are used; no code of that project is in this file.
 *
 * The parts:
 *   window   our own application context, one top-level window on the display with id 6 and
 *            size 1024x384, ID string "...Clone1" without "DispId[" (DisplayManager then puts
 *            it on display index 1, hides it once and forgets it), its own window group, a
 *            transparent 1x1 buffer like DisplayManager's own group owners. Created once,
 *            never destroyed: hidden while nothing is decoded.
 *   renderer context MMR_CONTEXT, output "screen:" into our group (the renderer makes the
 *            video window a child of ours, so we get its CREATE event and may crop it), input
 *            "mdevpush:" on our own name, as the stock player does it.
 *   feeding  the Android Auto receiver thread (or the lab player) only copies access units
 *            into a bounded queue; one worker thread of ours does everything that may block:
 *            set-up, sends (each with a kernel timeout), window care, teardown.
 * Any failure logs one line, releases what was taken and leaves the session off: the cluster
 * shows the built-in map, as stock.
 */
#include "sal.h"
#include "mcn_aa.h"

/* ---- what is easy to change ---------------------------------------------------------------- */

/* The switch: decode mode is the default since the car run of 2026-11-10 (the decoded
 * picture reached the cluster). It is off only while this file exists (GEM "passed through",
 * expert menu), or an empty file of this name sits in the root of a mounted card. */
#define PASSTHROUGH_FILE MCN_DIR "/aa-passthrough"
#define PASSTHROUGH_CARD_FLAG "/media/mp000/mst2-cluster-nav-passthrough"
/* Fallback picked at run time, no rebuild: the renderer's own top-level window on the display
 * index, as mst2-altscreen-carplay does it in a car (no crop: the picture is squeezed to 8:3). */
#define PLAIN_FILE MCN_DIR "/aa-decode-plain"
#define PLAIN_CARD_FLAG "/media/mp000/mst2-cluster-nav-decode-plain"
#define OUTPUT_URL_PLAIN "screen:/?index=%d&overlay=0&dstx=%u&dsty=%u&dstw=%u&dsth=%u&zorder=%d"
#define HIDE_MAP_FILE MCN_DIR "/hide-map"

/* The cluster source display (DisplayManager: Screen display id 6 is DISPLAYID_CLUSTER). */
#define CLUSTER_DISPLAY_ID 6
#define CLUSTER_DISPLAY_W 1024
#define CLUSTER_DISPLAY_H 384
/* Our window: one pixel column of the built-in map stays visible at the right edge. */
#define WIN_X 0
#define WIN_Y 0
#define WIN_W 1023
#define WIN_H 384
#define WIN_ZORDER 500 /* the built-in map's clone is at 3; 500 is the car-proven value */
#define VIDEO_ZORDER 1 /* the renderer's window inside our group: in front of its parent */
/* DisplayManager reads the display index after the last "Clone"; no "DispId[": unmanaged. */
#define WIN_ID "mcnAaClusterClone1"
#define WIN_GROUP "mcnAaClusterGroup"
#define VIDEO_ID "mcnAaVideoClone1"
/*
 * The part of the phone's picture that is shown: the centred part with the window's aspect
 * (8:3), over the whole width. For 800x480 that is 800x300 at (0, 90); for 1280x720 1280x480
 * at (0, 120). VIEW_DX / VIEW_DY move it (pixels of the phone's picture, + is right / down).
 */
#define VIEW_ASPECT_W 8
#define VIEW_ASPECT_H 3
#define VIEW_DX 0
#define LAYOUT_FILE MCN_DIR "/layout"
#define LAYOUT_CARD_FILE "/media/mp000/mst2-cluster-nav-layout.txt"
static int view_dy = AA_DEC_VIEW_DY; /* set from the layout when a session starts */

#define MMR_CONTEXT "mcn-aa-cluster"
/*
 * The renderer's output: its video window joins our group as a child (so we get its CREATE
 * event and can crop it), the stock keys, then the car-proven "overlay=0" (an overlay plane is
 * not captured). Fallback if the child is not captured: the reference's top-level window,
 * "screen:/?index=%s&overlay=0&dstx=%u&dsty=%u&dstw=%u&dsth=%u&zorder=500" with the display
 * index for the group (and no crop; the whole picture is squeezed into 8:3).
 */
/* "index": measured on the car, 2026-11-10: the child joined our group but Screen composed it on the
 * centre display (index 0); the display is the window's own, not the parent's, on this Screen.
 * So the renderer is told the display index, and the display is set again when it joins. */
#define OUTPUT_URL \
    "screen:/?index=%d&wingrp=%s&winid=%s&dstx=%u&dsty=%u&dstw=%u&dsth=%u&overlay=0&zorder=%d"
#define MDEV_NAME "/dev/name/local/mcnaamdev" /* not "mdev": that is the stock player's */
#define STREAM_FPS 30                         /* as the stock player declares, whatever comes */
#define PTS_AHEAD_US 50000                    /* stock: present 50 ms from now */
#define FOURCC_H264 0x48323634u

/* The renderer waits up to 2 s for room in its queue of 4 before it answers "dropped" (-16);
 * our kernel timeout lies above that, so a timeout means its receive thread is gone. */
#define SEND_TIMEOUT_MS 2500
#define SEND_ERRORS_MAX 5   /* dropped units in a row before the session is given up */
#define NAME_OPEN_TRIES 30  /* the name is served from mmr_play on; 100 ms apart */
#define NAME_OPEN_GAP_MS 100
/* The decoder plug-in opens with static descriptors (notes 3.5): our input is not attached
 * while the stock player may be attaching its own. Wait for the stock context to play. */
#define STOCK_STATE "/pps/services/multimedia/renderer/context/" \
                    "SalTechnology::GoogleAutomotiveLink/state"
#define SETTLE_MS 3000
#define SETTLE_CHECK_MS 100

#define QUEUE_UNITS 128u /* about 4 s of the phone's 27 fps: rides out a set-up */
#define QUEUE_BYTES (3u * 1024u * 1024u)
#define AU_MAX 1000000u
#define CONFIG_MAX 1024u

#define TICK_MS 500         /* window care: visibility, z-order, source rectangle */
#define STAT_MS 10000
#define VIDEO_WAIT_MS 5000  /* the renderer's window is looked for at every pass this long */
#define VERBOSE_STARTS 3    /* every call of the first set-ups is logged, later only failures */
#define DPY_FILE "/dev/screen/0/dpy-6"

/* ---- QNX pieces, written from the public API ------------------------------------------------- */

#define NTO_TIMEOUT_SEND 0x10  /* 1 << STATE_SEND */
#define NTO_TIMEOUT_REPLY 0x40 /* 1 << STATE_REPLY */
#define QNX_EBUSY 16

/* screen.h numbers; DisplayManager's own calls on this unit use the same (notes, section 4.4). */
enum {
    P_BUFFER_SIZE = 5,
    P_DISPLAY = 11,
    P_ID_STRING = 20,
    P_POSITION = 35,
    P_RENDER_BUFFERS = 37,
    P_SIZE = 40,
    P_SOURCE_POSITION = 41,
    P_SOURCE_SIZE = 42,
    P_TRANSPARENCY = 46,
    P_TYPE = 47,
    P_VISIBLE = 51,
    P_WINDOW = 52,
    P_ZORDER = 54,
    P_DISPLAY_COUNT = 59,
    P_DISPLAYS = 60,
    P_ID = 87,
    P_COLOR = 93
};
#define EVENT_NONE 0
#define EVENT_CREATE 1
#define EVENT_CLOSE 3
#define TRANSPARENCY_SOURCE_OVER 3

/* The 24-byte header of every mdev message; the server reads type, cmd and len only. */
struct mdev_hdr {
    uint16_t type;    /* 0: outside 0x100..0x1ff, so not a QNX I/O message */
    uint16_t subtype;
    uint8_t pad[12];
    uint32_t cmd;
    uint32_t len;     /* payload bytes after the header */
};
typedef char mdev_hdr_is_24_bytes[sizeof(struct mdev_hdr) == 24 ? 1 : -1];

/* Payload of command 2. */
struct mdev_format {
    uint32_t kind; /* 2: video */
    uint32_t fourcc;
    uint32_t width, height;
    uint32_t depth; /* 16 */
    uint32_t frame_rate;
    uint32_t flag; /* 1 in the stock Android Auto path */
    uint32_t zero;
};
typedef char mdev_format_is_32_bytes[sizeof(struct mdev_format) == 32 ? 1 : -1];

/* libmmrndclient.so.1, libstrm.so.1, libscreen.so.1: loaded by SAL, resolved at run time. */
static void *(*p_mmr_connect)(const char *);
static void (*p_mmr_disconnect)(void *);
static void *(*p_mmr_context_create)(void *, const char *, unsigned, unsigned);
static void *(*p_mmr_context_open)(void *, const char *); /* optional */
static int (*p_mmr_context_destroy)(void *);
static int (*p_mmr_context_parameters)(void *, void *);
static int (*p_mmr_output_attach)(void *, const char *, const char *);
static int (*p_mmr_output_detach)(void *, unsigned);
static int (*p_mmr_input_parameters)(void *, void *);
static int (*p_mmr_input_attach)(void *, const char *, const char *);
static int (*p_mmr_play)(void *);
static int (*p_mmr_stop)(void *);
static const void *(*p_mmr_error_info)(void *);
static void *(*p_strm_dict_new)(void);
static void *(*p_strm_dict_set)(void *, const char *, const char *);
static int (*p_screen_create_context)(void **, int);
static int (*p_screen_get_context_property_iv)(void *, int, int *);
static int (*p_screen_get_context_property_pv)(void *, int, void **);
static int (*p_screen_get_display_property_iv)(void *, int, int *);
static int (*p_screen_create_window)(void **, void *);
static int (*p_screen_set_window_property_iv)(void *, int, const int *);
static int (*p_screen_set_window_property_cv)(void *, int, int, const char *);
static int (*p_screen_set_window_property_pv)(void *, int, void **);
static int (*p_screen_get_window_property_iv)(void *, int, int *);
static int (*p_screen_get_window_property_cv)(void *, int, int, char *);
static int (*p_screen_get_window_property_pv)(void *, int, void **);
static int (*p_screen_create_window_group)(void *, const char *);
static int (*p_screen_create_window_buffers)(void *, int);
static int (*p_screen_post_window)(void *, void *, int, const int *, int);
static int (*p_screen_flush_context)(void *, int);
static int (*p_screen_create_event)(void **);
static int (*p_screen_get_event)(void *, void *, uint64_t);
static int (*p_screen_get_event_property_iv)(void *, int, int *);
static int (*p_screen_get_event_property_pv)(void *, int, void **);
static int resolved; /* 1 all there, -1 something missing (stays off), 0 not tried */

/* ---- state ------------------------------------------------------------------------------- */

struct unit {
    uint8_t *data;       /* config_len bytes of SPS and PPS, then the access unit */
    unsigned n;          /* bytes of the access unit */
    unsigned config_len; /* key frames carry the stream's parameter sets */
    unsigned token;
    unsigned width, height;
    int idr;
    int config_new;      /* the phone sent these parameter sets with (or just before) this unit */
};

/* The queue: one producer at a time (p_lock), one consumer (the worker). */
static struct unit ring[QUEUE_UNITS];
static volatile unsigned q_head, q_tail, q_bytes_in, q_bytes_out;

/* Producers (the Android Auto receiver thread, the lab player). */
static volatile int p_lock;
static volatile unsigned want_token; /* the session that may be decoded; 0: none */
static unsigned p_tokens, p_width, p_height, p_config_len;
static uint8_t p_config[CONFIG_MAX];
static int p_need_idr, p_config_new, p_overflow_said, p_config_said, p_big_said, worker_started;
static volatile int want_key; /* a unit was lost in front of the queue: a key frame is needed */
static volatile unsigned p_queued, p_dropped, p_waiting;

/* The worker. */
enum { ST_IDLE, ST_RUN };
static volatile int state, primed;
static volatile unsigned run_token, failed_token;
static volatile int w_lost; /* a unit was lost behind the queue: tells the producer */
static void *mconn, *mctx;
static int mout = -1, coid = -1, playing;
static int config_sent, w_skip, errors_row, timeout_said;
static unsigned run_ms, tick_ms, stat_ms, settle_token, settle_ms, settle_check_ms, starts;
static unsigned run_w, run_h;
static unsigned pushed, pushed_bytes, send_errors, skipped, q_max, last_counter;
static unsigned st_pushed, st_bytes, st_errors, st_skipped, st_dropped, st_waiting;

/* The window. */
static void *sctx, *sdisplay, *swin, *sbuf, *sevent, *svideo;
static int sdisplay_index = -1, plain; /* plain: the fallback output of PLAIN_FILE */
static int win_step, shown, verbose = 1;
static int video_said, reassert_said, source_said, event_said;

/* ---- logging ------------------------------------------------------------------------------ */

static void say(const char *text)
{
    mcn_log(text);
}

static void sayn(const char *what, long v)
{
    char line[160];

    snprintf(line, sizeof(line), "decode: %s%ld", what, v);
    mcn_log(line);
}

/* One set-up call: its result goes to the log (always when it failed). Returns ok. */
static int called(const char *what, long rc, int ok)
{
    char line[200];

    if (verbose || !ok) {
        snprintf(line, sizeof(line), "decode: %s -> %ld%s", what, rc, ok ? "" : " FAILED");
        mcn_log(line);
    }
    return ok;
}

static void say_error_info(void)
{
    const uint8_t *e = mctx && p_mmr_error_info ? p_mmr_error_info(mctx) : NULL;
    char line[200], text[81];
    uint32_t code;
    unsigned i;

    if (!e) {
        return;
    }
    /* mmr_error_info_t: error code at +0, extra_type +4 (20 bytes), extra_text +0x20. */
    memcpy(&code, e, 4);
    for (i = 0; i < sizeof(text) - 1 && e[0x20 + i] >= 0x20 && e[0x20 + i] < 0x7f; i++) {
        text[i] = (char)e[0x20 + i];
    }
    text[i] = 0;
    snprintf(line, sizeof(line), "decode: mmr_error_info: error %u, text '%s'", (unsigned)code,
             text);
    mcn_log(line);
}

static int exists(const char *path)
{
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        return 0;
    }
    close(fd);
    return 1;
}

/* ---- the switch ----------------------------------------------------------------------------- */

int mcn_decode_on(void)
{
    char flag[] = PASSTHROUGH_CARD_FLAG;
    int i;

    if (exists(PASSTHROUGH_FILE)) {
        return 0;
    }
    for (i = 0; i < 10; i++) {
        flag[11] = (char)('0' + i); /* /media/mp000 .. mp009 */
        if (exists(flag)) {
            return 0;
        }
    }
    return 1;
}

static int layout_read(const char *path, int v[5])
{
    char text[96];
    int fd = open(path, O_RDONLY), k = 0, i = 0, neg, n;

    if (fd < 0) {
        return 0;
    }
    memset(text, 0, sizeof(text)); /* not "= { 0 }": that needs a run-time helper libc lacks */
    read(fd, text, sizeof(text) - 1);
    close(fd);
    while (k < 5 && text[i]) {
        while (text[i] && text[i] != '-' && (text[i] < '0' || text[i] > '9')) {
            i++;
        }
        if (!text[i]) {
            break;
        }
        neg = text[i] == '-';
        i += neg;
        for (n = 0; text[i] >= '0' && text[i] <= '9'; i++) {
            n = n * 10 + (text[i] - '0');
        }
        v[k++] = neg ? -n : n;
    }
    return k;
}

void mcn_decode_layout(int v[5])
{
    char card[] = LAYOUT_CARD_FILE, line[160];
    int i, k = 0, ok = 1;

    v[0] = AA_DEC_INSET_TOP;
    v[1] = AA_DEC_INSET_BOTTOM;
    v[2] = AA_DEC_INSET_LEFT;
    v[3] = AA_DEC_INSET_RIGHT;
    v[4] = AA_DEC_VIEW_DY;
    for (i = 0; i < 10 && !k; i++) {
        card[11] = (char)('0' + i); /* /media/mp000 .. mp009 */
        k = layout_read(card, v);
    }
    if (!k) {
        k = layout_read(LAYOUT_FILE, v);
    }
    for (i = 0; i < 4; i++) {
        ok = ok && v[i] >= 0 && v[i] <= 400;
    }
    ok = ok && v[0] + v[1] <= 400 && v[2] + v[3] <= 700 && v[4] >= -90 && v[4] <= 90;
    if (!ok) { /* nonsense in the file: the defaults */
        v[0] = AA_DEC_INSET_TOP;
        v[1] = AA_DEC_INSET_BOTTOM;
        v[2] = AA_DEC_INSET_LEFT;
        v[3] = AA_DEC_INSET_RIGHT;
        v[4] = AA_DEC_VIEW_DY;
    }
    view_dy = v[4];
    snprintf(line, sizeof(line), "decode: layout %s: insets top %d bottom %d left %d right %d, dy %d",
             !k ? "default" : ok ? "from file" : "file invalid, default", v[0], v[1], v[2], v[3],
             v[4]);
    mcn_log(line);
}

static int plain_wanted(void)
{
    char flag[] = PLAIN_CARD_FLAG;
    int i;

    if (exists(PLAIN_FILE)) {
        return 1;
    }
    for (i = 0; i < 10; i++) {
        flag[11] = (char)('0' + i);
        if (exists(flag)) {
            return 1;
        }
    }
    return 0;
}

/* ---- symbols -------------------------------------------------------------------------------- */

static void *sym(void *lib, const char *name, int *ok)
{
    void *p = lib ? dlsym(lib, name) : NULL;

    if (!p) {
        mcn_log2("decode: missing symbol ", name);
        *ok = 0;
    }
    return p;
}

static int resolve(void)
{
    void *mmr, *strm, *screen;
    int ok = 1;

    if (resolved) {
        return resolved > 0;
    }
    /* All three are DT_NEEDED of SAL: these only return the handles of loaded libraries. */
    mmr = dlopen("libmmrndclient.so.1", RTLD_LAZY | RTLD_GLOBAL);
    strm = dlopen("libstrm.so.1", RTLD_LAZY | RTLD_GLOBAL);
    screen = dlopen("libscreen.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!mmr || !strm || !screen) {
        say(!mmr ? "decode: libmmrndclient.so.1 not found" :
            !strm ? "decode: libstrm.so.1 not found" : "decode: libscreen.so.1 not found");
        ok = 0;
    }
    p_mmr_connect = sym(mmr, "mmr_connect", &ok);
    p_mmr_disconnect = sym(mmr, "mmr_disconnect", &ok);
    p_mmr_context_create = sym(mmr, "mmr_context_create", &ok);
    p_mmr_context_destroy = sym(mmr, "mmr_context_destroy", &ok);
    p_mmr_context_parameters = sym(mmr, "mmr_context_parameters", &ok);
    p_mmr_output_attach = sym(mmr, "mmr_output_attach", &ok);
    p_mmr_output_detach = sym(mmr, "mmr_output_detach", &ok);
    p_mmr_input_parameters = sym(mmr, "mmr_input_parameters", &ok);
    p_mmr_input_attach = sym(mmr, "mmr_input_attach", &ok);
    p_mmr_play = sym(mmr, "mmr_play", &ok);
    p_mmr_stop = sym(mmr, "mmr_stop", &ok);
    p_mmr_error_info = sym(mmr, "mmr_error_info", &ok);
    p_strm_dict_new = sym(strm, "strm_dict_new", &ok);
    p_strm_dict_set = sym(strm, "strm_dict_set", &ok);
    p_screen_create_context = sym(screen, "screen_create_context", &ok);
    p_screen_get_context_property_iv = sym(screen, "screen_get_context_property_iv", &ok);
    p_screen_get_context_property_pv = sym(screen, "screen_get_context_property_pv", &ok);
    p_screen_get_display_property_iv = sym(screen, "screen_get_display_property_iv", &ok);
    p_screen_create_window = sym(screen, "screen_create_window", &ok);
    p_screen_set_window_property_iv = sym(screen, "screen_set_window_property_iv", &ok);
    p_screen_set_window_property_cv = sym(screen, "screen_set_window_property_cv", &ok);
    p_screen_set_window_property_pv = sym(screen, "screen_set_window_property_pv", &ok);
    p_screen_get_window_property_iv = sym(screen, "screen_get_window_property_iv", &ok);
    p_screen_get_window_property_cv = sym(screen, "screen_get_window_property_cv", &ok);
    p_screen_get_window_property_pv = sym(screen, "screen_get_window_property_pv", &ok);
    p_screen_create_window_group = sym(screen, "screen_create_window_group", &ok);
    p_screen_create_window_buffers = sym(screen, "screen_create_window_buffers", &ok);
    p_screen_post_window = sym(screen, "screen_post_window", &ok);
    p_screen_flush_context = sym(screen, "screen_flush_context", &ok);
    p_screen_create_event = sym(screen, "screen_create_event", &ok);
    p_screen_get_event = sym(screen, "screen_get_event", &ok);
    p_screen_get_event_property_iv = sym(screen, "screen_get_event_property_iv", &ok);
    p_screen_get_event_property_pv = sym(screen, "screen_get_event_property_pv", &ok);
    /* Only for a context a crashed SAL left behind (mm-renderer runs without -c). */
    p_mmr_context_open = mmr ? dlsym(mmr, "mmr_context_open") : NULL;
    resolved = ok ? 1 : -1;
    say(ok ? "decode: mm-renderer, strm and screen functions resolved"
           : "decode: functions missing: decode mode stays off, the cluster shows the built-in map");
    return ok;
}

/* ---- the window ----------------------------------------------------------------------------- */

/* One property of two ints; the result is logged, the call made once. */
static int win_iv(void *win, int prop, int a, int b, const char *what)
{
    int v[2], rc;

    v[0] = a;
    v[1] = b;
    rc = p_screen_set_window_property_iv(win, prop, v);
    return called(what, rc, rc == 0);
}

/* The display with id 6 and size 1024x384, chosen by both: the index is not fixed. */
static int find_display(void)
{
    void **displays;
    int count = 0, i, rc, found = 0;
    char line[120];

    rc = p_screen_get_context_property_iv(sctx, P_DISPLAY_COUNT, &count);
    if (!called("screen display count", rc ? rc : count, rc == 0 && count > 0 && count <= 32)) {
        return 0;
    }
    displays = malloc((unsigned)count * sizeof(void *));
    if (!displays) {
        return called("memory for the display list", 0, 0);
    }
    memset(displays, 0, (unsigned)count * sizeof(void *));
    rc = p_screen_get_context_property_pv(sctx, P_DISPLAYS, displays);
    if (!called("screen display list", rc, rc == 0)) {
        free(displays);
        return 0;
    }
    for (i = 0; i < count; i++) {
        int id = -1, size[2] = { 0, 0 }, rc_id, rc_size;

        if (!displays[i]) {
            continue;
        }
        rc_id = p_screen_get_display_property_iv(displays[i], P_ID, &id);
        rc_size = p_screen_get_display_property_iv(displays[i], P_SIZE, size);
        snprintf(line, sizeof(line), "decode: display index %d: id %d, %dx%d (rc %d/%d)", i, id,
                 size[0], size[1], rc_id, rc_size);
        mcn_log(line);
        if (!found && !rc_id && !rc_size && id == CLUSTER_DISPLAY_ID &&
            size[0] == CLUSTER_DISPLAY_W && size[1] == CLUSTER_DISPLAY_H) {
            sdisplay = displays[i];
            sdisplay_index = i;
            found = 1;
        }
    }
    free(displays);
    if (!found) {
        say("decode: no display with id 6 and 1024x384: the cluster is not fed from here");
    }
    return found;
}

/*
 * The window is made once and kept. A step that failed is tried again at the next stream; the
 * steps before it are not repeated. The recipe of the group owner (transparent, 1x1 buffer,
 * the source rectangle beside it, one post) is DisplayManager's own for its window groups.
 */
static int window_ready(void)
{
    static const int dirty[4] = { 0, 0, 1, 1 };
    int rc;

    switch (win_step) {
    case 0:
        rc = p_screen_create_context(&sctx, 0); /* an application context, no manager rights */
        if (!called("screen_create_context", rc, rc == 0 && sctx)) {
            sctx = NULL;
            return 0;
        }
        win_step = 1;
        /* fall through */
    case 1:
        if (!find_display()) {
            return 0;
        }
        win_step = 2;
        /* fall through */
    case 2:
        rc = p_screen_create_window(&swin, sctx); /* top level: no DisplayManager group */
        if (!called("screen_create_window", rc, rc == 0 && swin)) {
            swin = NULL;
            return 0;
        }
        win_step = 3;
        /* fall through */
    case 3:
        /* Before anything else: DisplayManager reads the title when it learns of the window. */
        rc = p_screen_set_window_property_cv(swin, P_ID_STRING, (int)strlen(WIN_ID), WIN_ID);
        if (!called("window id string " WIN_ID, rc, rc == 0)) {
            return 0;
        }
        /* Hidden until a picture is decoded. */
        if (!win_iv(swin, P_VISIBLE, 0, 0, "window visible 0")) {
            return 0;
        }
        win_step = 4;
        /* fall through */
    case 4:
        rc = p_screen_set_window_property_pv(swin, P_DISPLAY, &sdisplay);
        if (!called("window display", rc, rc == 0)) {
            return 0;
        }
        rc = p_screen_flush_context(sctx, 0);
        if (!called("screen_flush_context", rc, rc == 0)) {
            return 0;
        }
        win_step = 5;
        /* fall through */
    case 5:
        rc = p_screen_create_window_group(swin, WIN_GROUP);
        if (!called("screen_create_window_group " WIN_GROUP, rc, rc == 0)) {
            return 0;
        }
        win_step = 6;
        /* fall through */
    case 6:
        /* No usage is set, so never an overlay. */
        if (!win_iv(swin, P_TRANSPARENCY, TRANSPARENCY_SOURCE_OVER, 0, "window transparency") ||
            !win_iv(swin, P_COLOR, 0, 0, "window colour") ||
            !win_iv(swin, P_BUFFER_SIZE, 1, 1, "window buffer size 1x1") ||
            !win_iv(swin, P_POSITION, WIN_X, WIN_Y, "window position") ||
            !win_iv(swin, P_SIZE, WIN_W, WIN_H, "window size 1023x384") ||
            !win_iv(swin, P_SOURCE_SIZE, WIN_W, WIN_H, "window source size") ||
            !win_iv(swin, P_SOURCE_POSITION, -WIN_W, -WIN_H, "window source position") ||
            !win_iv(swin, P_ZORDER, WIN_ZORDER, 0, "window z-order 500")) {
            return 0;
        }
        win_step = 7;
        /* fall through */
    case 7:
        rc = p_screen_create_window_buffers(swin, 1);
        if (!called("screen_create_window_buffers", rc, rc == 0)) {
            return 0;
        }
        win_step = 8;
        /* fall through */
    case 8:
        rc = p_screen_get_window_property_pv(swin, P_RENDER_BUFFERS, &sbuf);
        if (!called("window render buffer", rc, rc == 0 && sbuf)) {
            return 0;
        }
        rc = p_screen_post_window(swin, sbuf, 1, dirty, 0);
        if (!called("screen_post_window", rc, rc == 0)) {
            return 0;
        }
        win_step = 9;
        /* fall through */
    case 9:
        rc = p_screen_create_event(&sevent);
        if (!called("screen_create_event", rc, rc == 0 && sevent)) {
            sevent = NULL;
            return 0;
        }
        win_step = 10;
        /* fall through */
    case 10:
        rc = p_screen_flush_context(sctx, 0);
        if (!called("screen_flush_context", rc, rc == 0)) {
            return 0;
        }
        win_step = 11;
        say("decode: window ready on the cluster display (hidden)");
        /* fall through */
    default:
        return 1;
    }
}

/* The shown part of a w x h picture: *x, *y, *sw, *sh. */
static void view_of(unsigned w, unsigned h, int *x, int *y, int *sw, int *sh)
{
    int vw = (int)w, vh = (int)(w * VIEW_ASPECT_H / VIEW_ASPECT_W);

    if (vh > (int)h) { /* a picture wider than the window: full height, centred width */
        vh = (int)h;
        vw = (int)(h * VIEW_ASPECT_W / VIEW_ASPECT_H);
    }
    *x = ((int)w - vw) / 2 + VIEW_DX;
    *y = ((int)h - vh) / 2 + view_dy;
    if (*x < 0 || *x + vw > (int)w) {
        *x = ((int)w - vw) / 2;
    }
    if (*y < 0 || *y + vh > (int)h) {
        *y = ((int)h - vh) / 2;
    }
    *sw = vw;
    *sh = vh;
}

/* The renderer's window: the wanted part of the picture over our whole window. */
static void video_place(int log)
{
    int x, y, w, h, v[2], rc[6];
    char line[200];

    if (!svideo) {
        return;
    }
    view_of(run_w, run_h, &x, &y, &w, &h);
    v[0] = x;
    v[1] = y;
    rc[0] = p_screen_set_window_property_iv(svideo, P_SOURCE_POSITION, v);
    v[0] = w;
    v[1] = h;
    rc[1] = p_screen_set_window_property_iv(svideo, P_SOURCE_SIZE, v);
    v[0] = v[1] = 0;
    rc[2] = p_screen_set_window_property_iv(svideo, P_POSITION, v);
    v[0] = WIN_W;
    v[1] = WIN_H;
    rc[3] = p_screen_set_window_property_iv(svideo, P_SIZE, v);
    v[0] = VIDEO_ZORDER;
    v[1] = 0;
    rc[4] = p_screen_set_window_property_iv(svideo, P_ZORDER, v);
    v[0] = shown;
    rc[5] = p_screen_set_window_property_iv(svideo, P_VISIBLE, v);
    p_screen_flush_context(sctx, 0);
    if (log) {
        snprintf(line, sizeof(line),
                 "decode: video window shows %dx%d at (%d,%d) of %ux%u in %dx%d, "
                 "rc %d/%d/%d/%d/%d/%d",
                 w, h, x, y, run_w, run_h, WIN_W, WIN_H, rc[0], rc[1], rc[2], rc[3], rc[4], rc[5]);
        mcn_log(line);
    }
}

static void window_show(int on)
{
    int v[2] = { 0, 0 }, rc_visible, rc_order = 0;
    char line[120];

    if (win_step < 11) {
        return;
    }
    /* Plain output: the renderer's own top-level window shows; ours stays hidden, or its black
     * would be composed over the picture at the same z-order. */
    v[0] = plain ? 0 : on;
    rc_visible = p_screen_set_window_property_iv(swin, P_VISIBLE, v);
    if (on && !plain) {
        v[0] = WIN_ZORDER;
        rc_order = p_screen_set_window_property_iv(swin, P_ZORDER, v);
    }
    if (svideo) {
        v[0] = on;
        p_screen_set_window_property_iv(svideo, P_VISIBLE, v);
    }
    p_screen_flush_context(sctx, 0);
    if (on != shown || rc_visible || rc_order) {
        snprintf(line, sizeof(line), "decode: window %s (rc %d/%d)", on ? "shown" : "hidden",
                 rc_visible, rc_order);
        mcn_log(line);
    }
    shown = on;
}

/* DisplayManager sets a "...Clone1" window invisible and to z-order 0 once, when it learns of
 * it; the moment is not known. So both are read back and set again while we show. */
static void window_reassert(void)
{
    int visible[2] = { 1, 0 }, order[2] = { WIN_ZORDER, 0 }, v[2] = { 0, 0 };
    char line[160];

    if (plain) {
        return; /* our window is hidden in plain output; the renderer's is on its own */
    }
    p_screen_get_window_property_iv(swin, P_VISIBLE, visible);
    p_screen_get_window_property_iv(swin, P_ZORDER, order);
    if ((visible[0] != 1 || order[0] != WIN_ZORDER) && reassert_said < 5) {
        reassert_said++;
        snprintf(line, sizeof(line),
                 "decode: window was changed from outside (visible %d, z-order %d): set again",
                 visible[0], order[0]);
        mcn_log(line);
    }
    v[0] = 1;
    p_screen_set_window_property_iv(swin, P_VISIBLE, v);
    v[0] = WIN_ZORDER;
    p_screen_set_window_property_iv(swin, P_ZORDER, v);
    if (svideo) {
        int x, y, w, h, pos[2] = { 0, 0 }, size[2] = { 0, 0 };

        /* The renderer may lay its window out again when the decoder reports the format. */
        view_of(run_w, run_h, &x, &y, &w, &h);
        if (p_screen_get_window_property_iv(svideo, P_SOURCE_POSITION, pos) == 0 &&
            p_screen_get_window_property_iv(svideo, P_SOURCE_SIZE, size) == 0 &&
            (pos[0] != x || pos[1] != y || size[0] != w || size[1] != h)) {
            if (source_said < 5) {
                source_said++;
                snprintf(line, sizeof(line),
                         "decode: video window source was %dx%d at (%d,%d): set again", size[0],
                         size[1], pos[0], pos[1]);
                mcn_log(line);
            }
        }
        video_place(0);
    } else {
        p_screen_flush_context(sctx, 0);
    }
}

/* Windows that join or leave our group: the renderer's video window is the one named VIDEO_ID
 * (screen_writer may add a second, unnamed one: it is left alone). Never waits. */
static void window_events(void)
{
    char id[72], line[200];
    int i, type, rc, size[2], pos[2], order[2], visible[2];
    void *w;

    if (win_step < 11) {
        return;
    }
    for (i = 0; i < 8; i++) {
        rc = p_screen_get_event(sctx, sevent, 0);
        if (rc) {
            if (!event_said) {
                event_said = 1;
                sayn("screen_get_event failed, rc ", rc);
            }
            return;
        }
        type = EVENT_NONE;
        if (p_screen_get_event_property_iv(sevent, P_TYPE, &type) || type == EVENT_NONE) {
            return;
        }
        if (type != EVENT_CREATE && type != EVENT_CLOSE) {
            continue;
        }
        w = NULL;
        if (p_screen_get_event_property_pv(sevent, P_WINDOW, &w) || !w || w == swin) {
            continue;
        }
        if (type == EVENT_CLOSE) {
            if (w == svideo) {
                svideo = NULL;
                say("decode: video window closed");
            }
            continue;
        }
        memset(id, 0, sizeof(id));
        size[0] = size[1] = pos[0] = pos[1] = order[0] = visible[0] = -1;
        p_screen_get_window_property_cv(w, P_ID_STRING, (int)sizeof(id) - 1, id);
        p_screen_get_window_property_iv(w, P_BUFFER_SIZE, size);
        p_screen_get_window_property_iv(w, P_POSITION, pos);
        p_screen_get_window_property_iv(w, P_ZORDER, order);
        p_screen_get_window_property_iv(w, P_VISIBLE, visible);
        id[sizeof(id) - 1] = 0;
        /* Every window of the renderer onto the cluster display (measured on the car: a child is composed
         * on its own display, which defaults to the centre screen). */
        rc = sdisplay ? p_screen_set_window_property_pv(w, P_DISPLAY, &sdisplay) : -1;
        snprintf(line, sizeof(line),
                 "decode: window joined our group: id '%s', buffer %dx%d, at (%d,%d), "
                 "z-order %d, visible %d; moved to the cluster display rc %d",
                 id, size[0], size[1], pos[0], pos[1], order[0], visible[0], rc);
        mcn_log(line);
        if (!strcmp(id, VIDEO_ID)) {
            svideo = w;
            video_place(1);
        }
    }
}

/* ---- the renderer --------------------------------------------------------------------------- */

static uint64_t now_us(void)
{
    struct timespec ts;

    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);
}

#define SEND_NOT_ARMED (-100001)

/* One message: the header, then one or two payload parts. Never waits longer than the timeout. */
static int mdev_send(unsigned cmd, const void *a, unsigned an, const void *b, unsigned bn)
{
    struct mdev_hdr h;
    iov_t iov[3];
    uint64_t ns = (uint64_t)SEND_TIMEOUT_MS * 1000000u;
    int parts = 2;

    if (coid < 0 || !an) {
        return SEND_NOT_ARMED; /* an empty payload would be the end of the stream */
    }
    memset(&h, 0, sizeof(h));
    h.cmd = cmd;
    h.len = an + bn;
    iov[0].iov_base = &h;
    iov[0].iov_len = sizeof(h);
    iov[1].iov_base = (void *)a;
    iov[1].iov_len = an;
    if (bn) {
        iov[2].iov_base = (void *)b;
        iov[2].iov_len = bn;
        parts = 3;
    }
    if (TimerTimeout(CLOCK_MONOTONIC, NTO_TIMEOUT_SEND | NTO_TIMEOUT_REPLY, NULL, &ns, NULL) < 0) {
        if (!timeout_said) {
            timeout_said = 1;
            say("decode: the send timeout cannot be armed: nothing is sent");
        }
        return SEND_NOT_ARMED;
    }
    return MsgSendvs_r(coid, iov, parts, NULL, 0);
}

static void *dict_of(const char *key, const char *value)
{
    void *d = p_strm_dict_new();

    /* strm_dict_set gives a new handle and takes the old one, also when it fails. */
    return d ? p_strm_dict_set(d, key, value) : NULL;
}

/* Releases whatever a session holds, in the stock player's order; the window is only hidden. */
static void teardown(void)
{
    int rc;

    window_show(0);
    if (coid >= 0) {
        rc = name_close(coid);
        called("name_close", rc, 1);
        coid = -1;
    }
    if (mctx) {
        if (playing) {
            rc = p_mmr_stop(mctx);
            if (!called("mmr_stop", rc, rc >= 0)) {
                say_error_info();
            }
        }
        if (mout >= 0) {
            rc = p_mmr_output_detach(mctx, (unsigned)mout);
            called("mmr_output_detach", rc, rc >= 0);
        }
        rc = p_mmr_context_destroy(mctx);
        called("mmr_context_destroy", rc, rc >= 0);
    }
    if (mconn) {
        p_mmr_disconnect(mconn);
        called("mmr_disconnect", 0, 1);
    }
    mctx = NULL;
    mconn = NULL;
    mout = -1;
    playing = 0;
    /* The renderer's window went with its context: take the CLOSE, forget the handle. */
    window_events();
    svideo = NULL;
    primed = 0;
    state = ST_IDLE;
}

static void stop(const char *why)
{
    char line[160];

    snprintf(line, sizeof(line), "decode: stopping (%s): pushed %u, send errors %u", why, pushed,
             send_errors);
    mcn_log(line);
    teardown();
}

static int fail(const char *what)
{
    char line[200];

    snprintf(line, sizeof(line),
             "decode: %s: this stream is not decoded, the cluster shows the built-in map", what);
    mcn_log(line);
    return 0;
}

/* The stock H264VideoPlayer sequence with our names (notes, section 6). */
static int start(const struct unit *u)
{
    struct mdev_format format;
    char url[256], line[200];
    uint32_t one = 1;
    void *dict;
    int rc, i;

    verbose = starts < VERBOSE_STARTS;
    starts++;
    snprintf(line, sizeof(line), "decode: set-up %u for %ux%u", starts, u->width, u->height);
    mcn_log(line);
    if (!resolve()) {
        return fail("functions missing");
    }
    if (!window_ready()) {
        return fail("no window on the cluster display");
    }
    if (exists(HIDE_MAP_FILE)) {
        say("decode: GEM 'Built-in map: hidden' is on; the capture needs the built-in map, "
            "switch it to 'kept'");
    }
    run_w = u->width;
    run_h = u->height;
    mconn = p_mmr_connect(NULL);
    if (!called("mmr_connect", mconn != NULL, mconn != NULL)) {
        return fail("mm-renderer not reachable");
    }
    mctx = p_mmr_context_create(mconn, MMR_CONTEXT, 0, 0500);
    if (!mctx && p_mmr_context_open) {
        /* A SAL that died while decoding left its context (and its mdev name) behind. */
        void *old = p_mmr_context_open(mconn, MMR_CONTEXT);

        called("mmr_context_create failed; mmr_context_open of a left-over one", old != NULL, 1);
        if (old) {
            rc = p_mmr_context_destroy(old);
            called("mmr_context_destroy of the left-over context", rc, 1);
            mctx = p_mmr_context_create(mconn, MMR_CONTEXT, 0, 0500);
        }
    }
    if (!called("mmr_context_create " MMR_CONTEXT, mctx != NULL, mctx != NULL)) {
        return fail("no mm-renderer context");
    }
    dict = dict_of("prefetch_mode", "-1");
    rc = dict ? p_mmr_context_parameters(mctx, dict) : -1;
    if (!called("mmr_context_parameters prefetch_mode=-1", rc, dict && rc >= 0)) {
        say_error_info();
        return fail("context parameters refused");
    }
    plain = plain_wanted();
    if (plain) {
        snprintf(url, sizeof(url), OUTPUT_URL_PLAIN, sdisplay_index, 0u, 0u, (unsigned)WIN_W,
                 (unsigned)WIN_H, WIN_ZORDER);
    } else {
        snprintf(url, sizeof(url), OUTPUT_URL, sdisplay_index, WIN_GROUP, VIDEO_ID, 0u, 0u,
                 (unsigned)WIN_W, (unsigned)WIN_H, VIDEO_ZORDER);
    }
    mout = p_mmr_output_attach(mctx, url, "video");
    mcn_log2("decode: output ", url);
    if (!called("mmr_output_attach", mout, mout >= 0)) {
        say_error_info();
        mout = -1;
        return fail("output refused");
    }
    dict = dict_of("VideoDecoderLowLatencyMode", "1");
    rc = dict ? p_mmr_input_parameters(mctx, dict) : -1; /* before the attach, as stock */
    if (!called("mmr_input_parameters VideoDecoderLowLatencyMode=1", rc, dict && rc == 0)) {
        say_error_info();
        return fail("input parameters refused");
    }
    snprintf(url, sizeof(url),
             "mdevpush:%s?fourcc=H264&video.frame_rate=%u&video.depth=%u&video.width=%u"
             "&video.height=%u&video.duration=%u",
             MDEV_NAME, (unsigned)STREAM_FPS, 16u, u->width, u->height, 0u);
    rc = p_mmr_input_attach(mctx, url, "track");
    mcn_log2("decode: input ", url);
    if (!called("mmr_input_attach", rc, rc == 0)) {
        say_error_info();
        return fail("input refused");
    }
    rc = p_mmr_play(mctx);
    if (!called("mmr_play", rc, rc >= 0)) {
        say_error_info();
        return fail("play refused");
    }
    playing = 1;
    /* The name is answered by a thread the renderer starts with play: only now, and patiently. */
    for (i = 0; i < NAME_OPEN_TRIES && coid < 0; i++) {
        if (i) {
            usleep(NAME_OPEN_GAP_MS * 1000);
        }
        coid = name_open(MDEV_NAME, 0);
    }
    if (!called("name_open " MDEV_NAME, coid, coid >= 0)) {
        coid = -1;
        return fail("the renderer's input name does not open");
    }
    rc = mdev_send(5, &one, sizeof(one), NULL, 0); /* absolute PTS */
    if (!called("mdev command 5 (absolute PTS)", rc, rc >= 0)) {
        return fail("the renderer's input refuses messages");
    }
    memset(&format, 0, sizeof(format));
    format.kind = 2;
    format.fourcc = FOURCC_H264;
    format.width = u->width;
    format.height = u->height;
    format.depth = 16;
    format.frame_rate = STREAM_FPS;
    format.flag = 1;
    rc = mdev_send(2, &format, sizeof(format), NULL, 0);
    if (!called("mdev command 2 (format)", rc, rc >= 0)) {
        return fail("the renderer's input refuses the format");
    }
    config_sent = 0;
    w_skip = 0;
    errors_row = 0;
    pushed = pushed_bytes = send_errors = skipped = q_max = 0;
    st_pushed = st_bytes = st_errors = st_skipped = 0;
    st_dropped = p_dropped;
    st_waiting = p_waiting;
    video_said = 0;
    last_counter = 0;
    run_ms = tick_ms = stat_ms = mcn_now_ms();
    run_token = u->token;
    state = ST_RUN;
    window_events();
    return 1;
}

/* A send that did not return 0. Returns 1 to go on, 0 when the session is over. */
static int send_failed(const char *what, int rc)
{
    char line[160];

    send_errors++;
    errors_row++;
    if (rc == -QNX_EBUSY && errors_row < SEND_ERRORS_MAX) {
        /* The renderer's queue stayed full for 2 s: this unit is lost, the stream goes on. */
        if (send_errors <= 3) {
            snprintf(line, sizeof(line),
                     "decode: the renderer dropped %s (rc -16, its queue was full for 2 s)", what);
            mcn_log(line);
        }
        w_skip = 1;
        w_lost = 1;
        return 1;
    }
    snprintf(line, sizeof(line), "decode: sending %s failed, rc %d", what, rc);
    mcn_log(line);
    return 0;
}

/* One queued unit to the renderer: command 4 with the parameter sets in front of a key frame
 * (the first one, and whenever the phone sent new ones), then command 3. */
static int push(const struct unit *u)
{
    uint64_t pts;
    int rc;

    if (w_skip && !u->idr) {
        skipped++; /* after a lost unit nothing is decodable until the next key frame */
        return 1;
    }
    if (u->idr && u->config_len && (!config_sent || u->config_new)) {
        rc = mdev_send(4, u->data, u->config_len, NULL, 0);
        if (verbose || rc) {
            called("mdev command 4 (SPS and PPS)", rc, rc == 0);
        }
        if (rc) {
            return send_failed("the codec configuration", rc);
        }
        config_sent = 1;
    }
    pts = now_us() + PTS_AHEAD_US;
    rc = mdev_send(3, &pts, sizeof(pts), u->data + u->config_len, u->n);
    if (rc) {
        return send_failed("an access unit", rc);
    }
    errors_row = 0;
    if (u->idr) {
        w_skip = 0;
    }
    pushed++;
    pushed_bytes += u->n;
    if (!primed) {
        primed = 1;
        sayn("first key frame pushed, bytes ", (long)u->n);
        window_show(mcn_active());
    }
    return 1;
}

/* The composition counter of dpy-6 and whether DisplayManager captures it (1, 0, -1 unknown). */
static unsigned dpy_counter(int *capturing)
{
    static char text[8192];
    int fd = open(DPY_FILE, O_RDONLY), n = 0, k;
    unsigned v = 0;
    const char *p;

    *capturing = -1;
    if (fd < 0) {
        return 0;
    }
    while (n < (int)sizeof(text) - 1 && (k = read(fd, text + n, sizeof(text) - 1 - (unsigned)n)) > 0) {
        n += k;
    }
    close(fd);
    text[n] = 0;
    p = strstr(text, "capture buffer");
    if (p) {
        const char *none = strstr(p, "(none)"), *eol = strstr(p, "\n");

        *capturing = !(none && (!eol || none < eol));
    }
    p = strstr(text, "counter");
    while (p && *p && *p != '\n' && (*p < '0' || *p > '9')) {
        p++;
    }
    while (p && *p >= '0' && *p <= '9') {
        v = v * 10 + (unsigned)(*p++ - '0');
    }
    return v;
}

static void stats(unsigned now)
{
    char line[240];
    int capturing;
    unsigned counter = dpy_counter(&capturing), dropped = p_dropped, waiting = p_waiting;

    snprintf(line, sizeof(line),
             "decode: 10 s: pushed %u (%u bytes), dropped %u at the queue, %u waiting for a key "
             "frame, %u skipped, send errors %u, queue max %u; dpy-6 composed +%u, capture %s",
             pushed - st_pushed, pushed_bytes - st_bytes, dropped - st_dropped,
             waiting - st_waiting, skipped - st_skipped, send_errors - st_errors, q_max,
             last_counter ? counter - last_counter : 0,
             capturing > 0 ? "on" : capturing == 0 ? "off" : "unknown");
    mcn_log(line);
    st_pushed = pushed;
    st_bytes = pushed_bytes;
    st_errors = send_errors;
    st_skipped = skipped;
    st_dropped = dropped;
    st_waiting = waiting;
    last_counter = counter;
    q_max = 0;
    stat_ms = now;
}

/* Between units while a session runs: the window and the numbers. */
static void housekeeping(unsigned now)
{
    int due = now - tick_ms >= TICK_MS;

    if (due || (!svideo && now - run_ms < VIDEO_WAIT_MS)) {
        window_events();
    }
    if (due) {
        int want = primed && mcn_active();

        tick_ms = now;
        if (want != shown) {
            window_show(want); /* the mod was switched off, or on again */
        } else if (shown) {
            window_reassert();
        }
        if (!svideo && !video_said && now - run_ms >= VIDEO_WAIT_MS) {
            video_said = 1;
            say(plain ? "decode: plain output: the renderer's own window, no crop"
                      : "decode: the renderer's video window did not join our group (no CREATE event)");
        }
    }
    if (now - stat_ms >= STAT_MS) {
        stats(now);
    }
}

/* 1 when the stock Android Auto context plays, 0 when it does not, -1 when there is none. */
static int stock_playing(void)
{
    char text[512];
    int fd = open(STOCK_STATE, O_RDONLY), n;

    if (fd < 0) {
        return -1;
    }
    n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    text[n] = 0;
    return strstr(text, "state::playing") != NULL;
}

/* Our input is attached once the stock player is through with its own, or after SETTLE_MS. */
static int settled(unsigned token, unsigned now)
{
    char line[120];
    int playing_now;

    if (settle_token != token) {
        settle_token = token;
        settle_ms = now;
        settle_check_ms = now - SETTLE_CHECK_MS;
    }
    if (now - settle_check_ms < SETTLE_CHECK_MS && now - settle_ms < SETTLE_MS) {
        return 0;
    }
    settle_check_ms = now;
    playing_now = stock_playing();
    if (playing_now <= 0 && now - settle_ms < SETTLE_MS) {
        return 0;
    }
    snprintf(line, sizeof(line), "decode: stock video context %s after %u ms",
             playing_now > 0 ? "plays" : playing_now ? "absent" : "not playing", now - settle_ms);
    mcn_log(line);
    return 1;
}

static void drop_head(void)
{
    struct unit *u = &ring[q_head % QUEUE_UNITS];
    unsigned bytes = u->config_len + u->n;

    free(u->data);
    u->data = NULL;
    __sync_synchronize();
    q_bytes_out += bytes;
    q_head++;
}

/* One pass of the worker. Returns 1 when it did something (call again), 0 when it may sleep. */
int mcn_decode_step(void)
{
    unsigned now = mcn_now_ms(), token, depth;
    struct unit *u;

    __sync_synchronize();
    token = want_token;
    if (state == ST_RUN && run_token != token) {
        stop(token ? "another stream begins" : "the stream ended");
        return 1;
    }
    depth = q_tail - q_head;
    if (!depth) {
        if (state == ST_RUN) {
            housekeeping(now);
        }
        return 0;
    }
    if (depth > q_max) {
        q_max = depth;
    }
    u = &ring[q_head % QUEUE_UNITS];
    if (u->token != token || u->token == failed_token) {
        drop_head(); /* of a stream that ended, or that could not be decoded */
        return 1;
    }
    if (state != ST_RUN) {
        if (!u->idr) {
            w_lost = 1; /* a decoder starts at a key frame */
            drop_head();
            return 1;
        }
        if (!settled(u->token, now)) {
            return 0;
        }
        if (!start(u)) {
            failed_token = u->token;
            teardown();
            drop_head();
            return 1;
        }
    }
    housekeeping(now);
    if (!push(u)) {
        failed_token = u->token;
        stop("the renderer's input failed; this stream is not decoded any more");
    }
    drop_head();
    return 1;
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        if (!mcn_decode_step()) {
            usleep(state == ST_RUN || q_tail != q_head ? 3000 : 100000);
        }
    }
    return NULL;
}

/* ---- producers ------------------------------------------------------------------------------ */

/* Held for one copy of a unit at most; a second producer (the lab player beside a phone) waits
 * a few milliseconds, never longer. */
static int lock(void)
{
    int i;

    for (i = 0; i < 100; i++) {
        if (!__sync_lock_test_and_set(&p_lock, 1)) {
            return 1;
        }
        usleep(500);
    }
    return 0;
}

static void unlock(void)
{
    __sync_lock_release(&p_lock);
}

unsigned mcn_decode_begin(unsigned width, unsigned height)
{
    char line[120];
    unsigned token;

    if (width < 16 || height < 16 || width > 1920 || height > 1088) {
        say("decode: stream size not usable");
        return 0;
    }
    if (!lock()) {
        say("decode: busy, stream not taken");
        return 0;
    }
    if (!worker_started) {
        pthread_t thread;

        if (pthread_create(&thread, NULL, worker, NULL)) {
            unlock();
            say("decode: no worker thread: decode mode stays off");
            return 0;
        }
        pthread_detach(thread);
        worker_started = 1;
    }
    token = ++p_tokens;
    if (!token) {
        token = ++p_tokens;
    }
    p_width = width;
    p_height = height;
    p_need_idr = 1;
    p_config_len = 0;
    p_config_new = 0;
    p_overflow_said = p_config_said = p_big_said = 0;
    want_key = 0;
    w_lost = 0;
    __sync_synchronize();
    want_token = token;
    unlock();
    snprintf(line, sizeof(line), "decode: stream %u begins, %ux%u", token, width, height);
    mcn_log(line);
    return token;
}

void mcn_decode_end(unsigned token)
{
    if (token && __sync_bool_compare_and_swap(&want_token, token, 0)) {
        sayn("stream ends: ", (long)token);
    }
}

static void lose(void)
{
    p_need_idr = 1;
    want_key = 1;
    p_dropped++;
}

static void put_locked(unsigned token, const uint8_t *au, unsigned n, int idr,
                       const uint8_t *config, unsigned config_len)
{
    struct unit *u;
    unsigned prefix;
    uint8_t *data;

    if (config && config_len && config_len <= CONFIG_MAX) {
        memcpy(p_config, config, config_len);
        p_config_len = config_len;
        p_config_new = 1;
    }
    if (w_lost) {
        w_lost = 0;
        p_need_idr = 1;
    }
    /* The renderer's packet buffer is width x height x 2; a larger unit would end its stream. */
    if (n > AU_MAX || n + 64 > p_width * p_height * 2) {
        if (!p_big_said) {
            p_big_said = 1;
            sayn("access unit too large, dropped, bytes ", (long)n);
        }
        lose();
        return;
    }
    if (p_need_idr && !idr) {
        want_key = 1;
        p_waiting++;
        return;
    }
    if (idr && !p_config_len) {
        if (!p_config_said) {
            p_config_said = 1;
            say("decode: key frame without SPS and PPS before it, dropped");
        }
        want_key = 1;
        p_waiting++;
        return;
    }
    prefix = idr ? p_config_len : 0;
    data = q_tail - q_head < QUEUE_UNITS && q_bytes_in - q_bytes_out + prefix + n <= QUEUE_BYTES
               ? malloc(prefix + n)
               : NULL;
    if (!data) {
        if (!p_overflow_said) {
            p_overflow_said = 1;
            sayn("queue full, units are dropped until the next key frame; queued ",
                 (long)(q_tail - q_head));
        }
        lose();
        return;
    }
    memcpy(data, p_config, prefix);
    memcpy(data + prefix, au, n);
    u = &ring[q_tail % QUEUE_UNITS];
    u->data = data;
    u->n = n;
    u->config_len = prefix;
    u->token = token;
    u->width = p_width;
    u->height = p_height;
    u->idr = idr;
    u->config_new = idr && p_config_new;
    __sync_synchronize();
    q_bytes_in += prefix + n;
    q_tail++;
    p_queued++;
    if (idr) {
        p_need_idr = 0;
        p_config_new = 0;
        want_key = 0;
    }
}

void mcn_decode_put(unsigned token, const uint8_t *au, unsigned n, int idr, const uint8_t *config,
                    unsigned config_len)
{
    if (!token || token != want_token || token == failed_token || !au || !n) {
        return;
    }
    if (!lock()) {
        w_lost = 1;
        return;
    }
    if (token == want_token) {
        put_locked(token, au, n, idr != 0, config, config_len);
    }
    unlock();
}

void mcn_decode_lost(unsigned token)
{
    if (token && token == want_token) {
        w_lost = 1;
    }
}

int mcn_decode_want_key(unsigned token)
{
    return token && token == want_token && token != failed_token && (want_key || w_lost);
}

int mcn_decode_live(unsigned token)
{
    return token && state == ST_RUN && run_token == token && primed;
}

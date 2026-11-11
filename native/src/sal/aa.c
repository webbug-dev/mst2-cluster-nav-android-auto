/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Android Auto branch. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * Android Auto on the cluster: hooks in SAL around the receiver library
 * libext.google.gal.receiver.so (GAL), which SAL loads and drives (GalReceiver::init on
 * connect, registerService for its endpoints, shutdown on disconnect). The library is left
 * as it is; its exports are interposed from this preload, as sal.c does with the CarPlay
 * plug-in. What is added:
 *
 *   - three endpoints of our own in the receiver's router, built on the library's
 *     ProtocolEndpointBase (vtable copied, our handlers in the slots): a video sink for the
 *     cluster display, the input source a display needs, and the navigation status service;
 *   - the service discovery response rewritten on its way out (queueOutgoing): the stock
 *     main screen gets display id 0, ours are appended (aa_proto.c);
 *   - the cluster video sink speaks the media channel itself (setup, start, stop, focus,
 *     frames, acks) and publishes the phone's H.264 access units into the shared memory
 *     DisplayManager's hook reads (include/mcn_video.h), as the CarPlay receiver does: the
 *     SPS is rewritten to crop the 1280x720 stream to the cluster's 1010x376 viewport;
 *   - the phone is asked for protocol 1.7, so navigation arrives as NavigationState and
 *     NavigationCurrentPosition, which become the PhoneLink snapshot the HMI bundle draws.
 *
 * Layout facts relied on (the same on every receiver build this was checked against, MST2
 * P0480T included): ProtocolEndpointBase +0 vptr, +4 open flag, +5 channel, +8 router,
 * +0xc service id, vtable slots 0/1 dtors, 2 onChannelClosed, 3 mayOpenChannel,
 * 4 onChannelOpened, 5 routeMessage(this, channel, id, shared_ptr<IoBuffer>&),
 * 6 handleRawMessage, 7 addDiscoveryInfo; MessageRouter's endpoint table at
 * router + (id + 0x40) * 4; shared_ptr<IoBuffer> +4 -> IoBuffer { base +0, offset +8,
 * end +12 }. Anything else goes through exported functions. Every export must resolve and
 * the first registration must look as expected, or the hook stays stock.
 * Checked by disassembly on the EU Skoda P0478T library (sha256 b4fae78e..., 2019-06-05):
 * the facts above hold; an endpoint's byte at +0x10 selects handleRawMessage (0 here: slot 5);
 * GalReceiver::init returns a bool SAL tests (0 = no Android Auto), queueOutgoing returns
 * ChannelManager's result to callers that test it, onChannelClosed must return whether the
 * channel was open (the router frees it only then). So every interposer returns what the
 * library's function returned.
 *
 * Key frames: the phone sends one at the start of a stream and otherwise rarely. When
 * DisplayManager asks for one (want_idr), the cluster's video focus is taken away (mode 2)
 * and given back (mode 1); the phone restarts its encoder with SPS, PPS and an IDR.
 *
 * Decode mode (decode.c, GEM "decoded on the unit"): the phone is asked for 800x480 without
 * margins, its access units go to our own decoder on the unit instead of the shared memory
 * (DisplayManager's substitution stays idle: nothing is published, the stream is not marked
 * alive), the SPS is left as it is, and a focus cycle is only run when the decoder lost a
 * unit. The HMI still gets "video=1" while a picture is decoded.
 */
#include "sal.h"
#include "mcn_aa.h"
#include "mcn_video.h"

#define GAL_LIB "/tsd/lib/sal/gal/libext.google.gal.receiver.so"
#define GAL_LIB_NAME "libext.google.gal.receiver.so"
#define AA_OFF_FILE MCN_DIR "/aa-off"
#define AA_RES_FILE MCN_DIR "/aa-res"
#define AA_DPI_FILE MCN_DIR "/aa-dpi"
#define AA_NOKEY_FILE MCN_DIR "/aa-nokeycycle"
#define AA_DPI_DEFAULT 105 /* 1010 px over the cluster's 244 mm */
#define AA_VIDEO_SERVICE_FIRST 0x40
#define AA_VIDEO_SERVICE_LAST 0x7c
#define AA_KEY_GAP_MS 2000
/* A started stream gets this long to deliver its opening key frame before one is asked for. */
#define AA_FIRST_KEY_MS 3000
/* After a key frame, requests are dropped this long (DisplayManager catching up with the ring). */
#define AA_KEY_SETTLE_MS 300
#define AA_STAT_MS 10000
#define AA_FOCUS_RESTART_MS 500
#define AA_ENDPOINT_SIZE 0x100
#define AA_VTABLE_WORDS 10
#define AA_MAX_UNACKED 1

typedef int (*register_fn)(void *, void *);
typedef int (*init_fn)(void *, const void *);
typedef void (*shutdown_fn)(void *);
typedef int (*queue_fn)(void *, unsigned char, void *, unsigned);
typedef int (*route_fn)(void *, unsigned char, const void *);
typedef int (*version_fn)(void *);
typedef int (*version_resp_fn)(void *, void *, unsigned);
typedef int (*unexpected_fn)(void *, unsigned char);
typedef void (*channel_fn)(void *, unsigned char);

static register_fn real_register;
static init_fn real_init;
static shutdown_fn real_shutdown;
static queue_fn real_queue, real_queue_plain;
static route_fn real_route;
static version_fn real_version;
static version_resp_fn real_version_response;
static unexpected_fn real_unexpected;
static channel_fn base_opened;
static const uint32_t *base_vtable;
static int resolved; /* 1 all present, -1 missing (stock), 0 not tried */

/* Our endpoints and their vtables (never freed: static). */
static uint8_t ep_video[AA_ENDPOINT_SIZE] __attribute__((aligned(8)));
static uint8_t ep_input[AA_ENDPOINT_SIZE] __attribute__((aligned(8)));
static uint8_t ep_nav[AA_ENDPOINT_SIZE] __attribute__((aligned(8)));
static uint32_t vt_video[AA_VTABLE_WORDS] __attribute__((aligned(8)));
static uint32_t vt_input[AA_VTABLE_WORDS] __attribute__((aligned(8)));
static uint32_t vt_nav[AA_VTABLE_WORDS] __attribute__((aligned(8)));

static void *receiver, *router;
static int registered, cluster_offered, rewritten;
static unsigned id_video, id_input, id_nav;
static unsigned registrations, outgoing_logged, raw_logged, route_logged;

/* The cluster stream. */
static struct mcn_video *video;
static int streaming, session, need_idr, crop, pending_restart;
/* Decode mode: the stream's token in decode.c (0 until its first SPS), its coded size, and
 * whether the phone sent parameter sets since the last unit went to the decoder. */
static int decode, decode_config;
static unsigned decode_id, decode_w, decode_h;
static unsigned keys, frames, asked_ms, restart_ms, told_ms, acked, begin_ms, key_ms;
/*
 * Set while the stream is being restarted by our own focus cycle (ask_key_frame), until its key
 * frame is out. Such a restart is the same stream to DisplayManager: no new generation, the
 * sequence goes on, and the key frame that opens it is simply the next access unit. A new
 * generation would make DisplayManager start behind that key frame (it takes a stream up at the
 * newest unit) and ask again, and for Android Auto asking IS restarting: the cluster then never
 * got a picture (car, 2026-11-09, one cycle every AA_KEY_GAP_MS).
 */
static unsigned stat_ms, stat_frames, stat_bytes;
static int cycling;
static uint8_t ps[1024];
static unsigned ps_len;
static uint8_t frame_buf[MCN_VIDEO_SLOT_BYTES];

/* Navigation. */
static struct aa_nav nav;
static unsigned nav_sent_ms;
static int nav_active;

static void aa_log(const char *a, const char *b)
{
    mcn_log2(a, b);
}

static void aa_logn(const char *what, long v)
{
    char text[32];

    snprintf(text, sizeof(text), "%ld", v);
    mcn_log2(what, text);
}

static uint32_t rd(const void *p, unsigned off)
{
    uint32_t v;

    memcpy(&v, (const uint8_t *)p + off, 4);
    return v;
}

static void wr(void *p, unsigned off, uint32_t v)
{
    memcpy((uint8_t *)p + off, &v, 4);
}

static int file_exists(const char *path)
{
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        return 0;
    }
    close(fd);
    return 1;
}

static int file_number(const char *path, int fallback)
{
    char text[16] = { 0 };
    int fd = open(path, O_RDONLY), v = 0, i;

    if (fd < 0) {
        return fallback;
    }
    read(fd, text, sizeof(text) - 1);
    close(fd);
    for (i = 0; text[i] >= '0' && text[i] <= '9'; i++) {
        v = v * 10 + (text[i] - '0');
    }
    return i ? v : fallback;
}

/* Android Auto on the cluster is wanted: the mod is active and GEM did not turn it off. */
static int aa_enabled(void)
{
    return mcn_active() && !file_exists(AA_OFF_FILE);
}

static int arrows_only(void)
{
    char mode[8] = { 0 };
    int fd = open(MCN_MODE_FILE, O_RDONLY);

    if (fd < 0) {
        return 0;
    }
    read(fd, mode, sizeof(mode) - 1);
    close(fd);
    return strstr(mode, "arrows") != NULL;
}

static unsigned cluster_resolution(void)
{
    if (decode) {
        return AA_RES_800x480; /* decoded here: the smaller stream, no margins, no cropping */
    }
    return file_number(AA_RES_FILE, 720) == 480 ? AA_RES_800x480 : AA_RES_1280x720;
}

/* The stream is running: the phone's units reach DisplayManager, or our decoder. */
static int stream_running(void)
{
    return decode ? decode_id != 0 : video && video->write_seq;
}

/* ---- the library's exports --------------------------------------------------------------- */

static void *gal_sym(void *lib, const char *name, int *ok)
{
    void *p = lib ? dlsym(lib, name) : NULL;

    if (!p) {
        aa_log("AA: missing in GAL: ", name);
        *ok = 0;
    }
    return p;
}

static int resolve(void)
{
    void *lib;
    int ok = 1;

    if (resolved) {
        return resolved > 0;
    }
    lib = dlopen(GAL_LIB, RTLD_LAZY | RTLD_GLOBAL);
    if (!lib) {
        lib = dlopen(GAL_LIB_NAME, RTLD_LAZY | RTLD_GLOBAL);
    }
    if (!lib) {
        aa_log("AA: GAL library not found; Android Auto stays stock", "");
        resolved = -1;
        return 0;
    }
    real_register = gal_sym(lib, "_ZN11GalReceiver15registerServiceEP20ProtocolEndpointBase", &ok);
    real_init = gal_sym(lib, "_ZN11GalReceiver4initERK10shared_ptrI20IControllerCallbacksE", &ok);
    real_shutdown = gal_sym(lib, "_ZN11GalReceiver8shutdownEv", &ok);
    real_queue = gal_sym(lib, "_ZN13MessageRouter13queueOutgoingEhPvj", &ok);
    base_vtable = gal_sym(lib, "_ZTV20ProtocolEndpointBase", &ok);
    base_opened = gal_sym(lib, "_ZN20ProtocolEndpointBase15onChannelOpenedEh", &ok);
    /* Optional: without them the matching feature stays stock. */
    real_queue_plain = dlsym(lib, "_ZN13MessageRouter24queueOutgoingUnencryptedEhPvj");
    real_route = dlsym(lib, "_ZN13MessageRouter12routeMessageEhRK10shared_ptrI8IoBufferE");
    real_version = dlsym(lib, "_ZN10Controller18sendVersionRequestEv");
    real_version_response = dlsym(lib, "_ZN10Controller21handleVersionResponseEPvj");
    real_unexpected = dlsym(lib, "_ZN13MessageRouter21sendUnexpectedMessageEh");
    resolved = ok ? 1 : -1;
    aa_log(ok ? "AA: GAL exports resolved" : "AA: GAL exports incomplete; Android Auto stays stock",
           "");
    if (ok) {
        aa_logn("AA: optional exports: unencrypted/route/version/version-response/unexpected = ",
                (real_queue_plain != NULL) | (real_route != NULL) << 1 |
                    (real_version != NULL) << 2 | (real_version_response != NULL) << 3 |
                    (real_unexpected != NULL) << 4);
    }
    return ok;
}

/* ---- sending ----------------------------------------------------------------------------- */

static void send_on(void *endpoint, const uint8_t *msg, unsigned n)
{
    const uint8_t *ep = endpoint;

    if (!n || !rd(ep, 8) || !ep[4]) {
        aa_log("AA: cannot send, channel not open", "");
        return;
    }
    real_queue((void *)(uintptr_t)rd(ep, 8), ep[5], (void *)msg, n);
}

static void send_focus(unsigned mode, int unsolicited)
{
    uint8_t msg[16];

    send_on(ep_video, msg, aa_msg_video_focus(msg, sizeof(msg), mode, unsolicited));
    aa_logn("AA: video focus sent, mode ", (long)mode);
}

/* The payload of a routed message: the 2-byte id and what follows. */
static const uint8_t *payload(const void *shared, unsigned *size)
{
    const void *io;
    uint32_t base, off, end;

    *size = 0;
    if (!shared || ((uintptr_t)shared & 3) || (uintptr_t)shared < 0x10000) {
        return NULL;
    }
    io = (const void *)(uintptr_t)rd(shared, 4);
    if (!io || ((uintptr_t)io & 3) || (uintptr_t)io < 0x10000) {
        return NULL;
    }
    base = rd(io, 0);
    off = rd(io, 8);
    end = rd(io, 12);
    if (!base || end < off + 2 || end - off > 4u * 1024u * 1024u) {
        return NULL;
    }
    *size = end - off;
    return (const uint8_t *)(uintptr_t)(base + off);
}

/* ---- the cluster video stream ------------------------------------------------------------ */

/* The phone's cluster stream is open (or paused inside our own focus cycle): for lab.c. */
int mcn_aa_stream_live(void)
{
    return streaming || cycling;
}

static void stream_end(const char *why)
{
    if (streaming) {
        aa_log("AA: cluster stream ended: ", why);
        aa_logn("AA: frames ", frames);
        aa_logn("AA: key frames ", keys);
    }
    streaming = 0;
    cycling = 0;
    mcn_capture_end();
    if (decode_id) {
        mcn_decode_end(decode_id);
        decode_id = 0;
    }
    if (video && !decode) {
        video->alive_ms = 0;
        video->last_frame_ms = 0;
        mcn_video_wake(video);
    }
    mcn_link_send("video=0");
    mcn_busy(0);
}

/* The phone starts again after our focus cycle: the stream goes on (see `cycling`). */
static void stream_resume(int new_session)
{
    session = new_session;
    streaming = 1;
    need_idr = 1;
    keys = 0;
    begin_ms = mcn_now_ms();
    if (!decode) {
        video->alive_ms = begin_ms ? begin_ms : 1;
    }
    aa_logn("AA: cluster stream resumed for a key frame, session ", session);
}

static void stream_begin(int new_session)
{
    if (!video && !decode) {
        video = mcn_video_map();
        if (!video) {
            aa_log("AA: no shared memory for the cluster video", "");
            return;
        }
    }
    session = new_session;
    streaming = 1;
    need_idr = 1;
    frames = keys = acked = 0;
    begin_ms = mcn_now_ms();
    if (decode) {
        /* The decoder is started by the stream's first SPS, which tells the coded size. */
        if (decode_id) {
            mcn_decode_end(decode_id);
        }
        decode_id = 0;
        decode_config = 0;
        ps_len = 0;
    } else {
        video->generation++;
        video->write_seq = 0;
        video->alive_ms = mcn_now_ms() ? mcn_now_ms() : 1;
    }
    mcn_busy(1);
    stat_ms = begin_ms;
    stat_frames = stat_bytes = 0;
    mcn_capture_begin();
    aa_logn(decode ? "AA: cluster stream started for the decoder, session "
                   : "AA: cluster stream started, session ",
            session);
}

/* Taking the focus away and back makes the phone start the stream again with a key frame. */
static void ask_key_frame(unsigned now)
{
    if (!streaming || pending_restart || file_exists(AA_NOKEY_FILE) ||
        now - asked_ms < AA_KEY_GAP_MS) {
        return;
    }
    asked_ms = now;
    pending_restart = 1;
    restart_ms = now;
    cycling = 1;
    if (!decode) {
        video->want_idr = 0;
    }
    send_focus(AA_FOCUS_NATIVE, 1);
}

static void restart_if_due(unsigned now)
{
    if (pending_restart && now - restart_ms >= AA_FOCUS_RESTART_MS) {
        pending_restart = 0;
        send_focus(AA_FOCUS_PROJECTED, 1);
    }
}

/* Everything that happens between frames: liveness, the HMI's "video=1", key frames. */
static void stream_tick(void)
{
    unsigned now = mcn_now_ms();

    if (!streaming || (!video && !decode)) {
        return;
    }
    if (!decode) {
        video->alive_ms = now ? now : 1;
    }
    if ((decode ? mcn_decode_live(decode_id) : (int)video->write_seq) && now - told_ms >= 1000 &&
        mcn_active()) {
        told_ms = now;
        mcn_link_send("video=1");
    }
    mcn_busy_tick();
    if (now - stat_ms >= AA_STAT_MS) {
        /* What the phone really sends: a still map sends nothing, a moving one up to 30 fps. */
        aa_logn("AA: frames in the last 10 s: ", stat_frames);
        aa_logn("AA: bytes in the last 10 s: ", stat_bytes);
        stat_ms = now;
        stat_frames = stat_bytes = 0;
    }
    restart_if_due(now);
    if (decode) {
        /* The decoder has the stream from its start; a focus cycle only after a lost unit,
         * and not while the key frame of a freshly started stream is still on its way. */
        if (mcn_decode_want_key(decode_id) && (keys || now - begin_ms >= AA_FIRST_KEY_MS)) {
            ask_key_frame(now);
        }
        return;
    }
    /*
     * A stream that has just (re)started opens with a key frame of its own, so a request raised
     * while that one is on its way must not start another focus cycle: DisplayManager asks on
     * every pass while it waits for the first frame of the new stream (car, 2026-11-09: left
     * standing, that request restarted the stream every AA_KEY_GAP_MS for ever, and the cluster
     * showed the stock map). Requests count once this stream's key frame is out, or if none
     * came at all.
     */
    if (keys && now - key_ms < AA_KEY_SETTLE_MS) {
        /* DisplayManager may still be passing the units in front of the key frame just sent,
         * raising a request for each; one that is real is raised again on its next pass. */
        video->want_idr = 0;
    }
    if (video->want_idr && (keys || now - begin_ms >= AA_FIRST_KEY_MS)) {
        ask_key_frame(now);
    }
}

/* Decode mode: the unit as the phone sent it (no crop) to the decoder on the unit. */
static void decode_unit(const uint8_t *au, unsigned n)
{
    unsigned mask = aa_nal_types(au, n), now = mcn_now_ms();
    int idr = (mask & AA_NAL_IDR) != 0;

    if (mask & (AA_NAL_SPS | AA_NAL_PPS)) {
        unsigned k = aa_annexb_parameter_sets(au, n, ps, sizeof(ps), 0), w = 0, h = 0, pos;

        if (!k) {
            aa_log("AA: parameter sets not understood", "");
        } else {
            ps_len = k;
            decode_config = 1;
            for (pos = 0; pos + 5 < ps_len; pos++) {
                if (ps[pos] == 0 && ps[pos + 1] == 0 && ps[pos + 2] == 1 &&
                    (ps[pos + 3] & 0x1f) == 7) {
                    aa_sps_size(ps + pos + 3, ps_len - pos - 3, &w, &h);
                    break;
                }
            }
            if (w && h && (!decode_id || w != decode_w || h != decode_h)) {
                /* The first SPS, or another coded size: the decoder (re)starts for it. */
                aa_logn("AA: stream width ", w);
                aa_logn("AA: stream height ", h);
                decode_w = w;
                decode_h = h;
                decode_id = mcn_decode_begin(w, h);
            }
        }
    }
    if (!(mask & (AA_NAL_IDR | AA_NAL_SLICE)) || !decode_id) {
        return; /* codec configuration only, or no SPS yet: nothing to decode */
    }
    mcn_decode_put(decode_id, au, n, idr, decode_config ? ps : NULL, decode_config ? ps_len : 0);
    decode_config = 0;
    frames++;
    stat_frames++;
    stat_bytes += n;
    if (idr && !(mask & AA_NAL_SPS) && ps_len) {
        mcn_capture_put(ps, ps_len, 0); /* the recording stays playable: SPS and PPS first */
    }
    mcn_capture_put(au, n, idr);
    if (idr) {
        cycling = 0;
        key_ms = now;
        keys++;
        if (keys <= 3) {
            aa_logn("AA: key frame, bytes ", n);
        }
    }
}

/* One access unit from the phone into the next slot (include/mcn_video.h). */
static void publish(const uint8_t *au, unsigned n)
{
    unsigned seq, mask = aa_nal_types(au, n), now;
    struct mcn_video_slot *slot;
    int idr;

    if (decode) {
        decode_unit(au, n);
        return;
    }
    if (mask & (AA_NAL_SPS | AA_NAL_PPS)) {
        unsigned k = aa_annexb_parameter_sets(au, n, ps, sizeof(ps), crop);

        if (k) {
            ps_len = k;
            if (!keys) {
                unsigned w = 0, h = 0, pos = 0;

                /* The first SPS tells what the phone encodes. */
                for (pos = 0; pos + 5 < ps_len; pos++) {
                    if (ps[pos] == 0 && ps[pos + 1] == 0 && ps[pos + 2] == 1 &&
                        (ps[pos + 3] & 0x1f) == 7) {
                        aa_sps_size(ps + pos + 3, ps_len - pos - 3, &w, &h);
                        break;
                    }
                }
                aa_logn("AA: stream width ", w);
                aa_logn("AA: stream height ", h);
            }
        } else {
            aa_log("AA: parameter sets not understood", "");
        }
    }
    if (!(mask & (AA_NAL_IDR | AA_NAL_SLICE))) {
        return; /* codec configuration only; it goes in front of the next IDR */
    }
    seq = video->write_seq + 1;
    slot = &video->slot[seq % MCN_VIDEO_SLOTS];
    slot->seq = 0;
    __sync_synchronize();
    slot->size = aa_annexb_rewrite(au, n, slot->data, MCN_VIDEO_AU_MAX, ps, ps_len, crop, &idr);
    now = mcn_now_ms();
    if (!slot->size) {
        need_idr = 1;
        video->want_idr = 1;
        return;
    }
    if (need_idr && !idr) {
        video->want_idr = 1;
        return;
    }
    need_idr = 0;
    slot->flags = idr ? MCN_VIDEO_IDR : 0;
    slot->pts_us = (unsigned long long)now * 1000u;
    __sync_synchronize();
    slot->seq = seq;
    video->write_seq = seq;
    video->last_frame_ms = now;
    video->alive_ms = now ? now : 1;
    frames++;
    stat_frames++;
    stat_bytes += slot->size;
    mcn_capture_put(slot->data, slot->size, idr);
    if (idr) {
        /* This key frame answers every request raised before it, and ends a focus cycle. */
        video->want_idr = 0;
        cycling = 0;
        key_ms = now;
        keys++;
        if (keys <= 3) {
            aa_logn("AA: key frame, bytes ", slot->size);
        }
    }
    mcn_video_wake(video);
}

static void handle_media_data(unsigned id, const uint8_t *p, unsigned n)
{
    unsigned skip = id == AA_MSG_MEDIA_WITH_TIMESTAMP ? 10 : 2;
    uint8_t msg[24];

    if (n <= skip) {
        return;
    }
    if (streaming) {
        if (n - skip <= sizeof(frame_buf)) {
            memcpy(frame_buf, p + skip, n - skip);
            publish(frame_buf, n - skip);
        } else {
            aa_logn("AA: oversized frame dropped, bytes ", n - skip);
            need_idr = 1;
            if (decode) {
                mcn_decode_lost(decode_id);
            } else {
                video->want_idr = 1;
            }
        }
    }
    send_on(ep_video, msg, aa_msg_ack(msg, sizeof(msg), session, 1));
    acked++;
    stream_tick();
}

static int video_route(void *self, unsigned channel, unsigned msg_id, const void *shared)
{
    unsigned n, id;
    const uint8_t *p = payload(shared, &n);
    uint8_t msg[24];

    (void)self;
    (void)channel;
    if (!p) {
        if (raw_logged++ < 3) {
            aa_logn("AA: video message without payload, id ", (long)(msg_id & 0xffff));
        }
        return 0;
    }
    id = ((unsigned)p[0] << 8) | p[1];
    if (raw_logged < 6 && id >= 0x8000) {
        raw_logged++;
        aa_logn("AA: video channel message ", (long)id);
    }
    switch (id) {
    case AA_MSG_MEDIA_WITH_TIMESTAMP:
    case AA_MSG_MEDIA:
        handle_media_data(id, p, n);
        break;
    case AA_MSG_SETUP_REQUEST: {
        int type = aa_parse_setup_request(p + 2, n - 2);

        aa_logn("AA: cluster setup, codec ", type);
        if (type != AA_CODEC_H264) {
            return -8;
        }
        send_on(ep_video, msg, aa_msg_setup_response(msg, sizeof(msg), 2, AA_MAX_UNACKED));
        send_focus(AA_FOCUS_PROJECTED, 1);
        break;
    }
    case AA_MSG_START: {
        int s = 0;
        unsigned config = 0;

        aa_parse_start(p + 2, n - 2, &s, &config);
        pending_restart = 0;
        if (cycling && stream_running()) {
            stream_resume(s);
        } else {
            cycling = 0;
            stream_begin(s);
        }
        break;
    }
    case AA_MSG_STOP:
        if (cycling && stream_running()) {
            /* Our own focus cycle: a pause in the same stream, the cluster keeps the picture. */
            streaming = 0;
        } else {
            cycling = 0;
            stream_end("stop");
        }
        restart_ms = 0; /* the phone stopped: give the focus back now */
        restart_if_due(mcn_now_ms());
        break;
    case AA_MSG_VIDEO_FOCUS_REQUEST: {
        unsigned mode = aa_parse_focus_request(p + 2, n - 2);

        aa_logn("AA: focus requested, mode ", (long)mode);
        if (mode == AA_FOCUS_PROJECTED || mode == AA_FOCUS_NATIVE) {
            send_focus(mode, 0);
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

static int video_closed(void *self, unsigned char channel)
{
    int open = ((uint8_t *)ep_video)[4];

    (void)self;
    (void)channel;
    stream_end("channel closed");
    ((uint8_t *)ep_video)[4] = 0;
    return open; /* as ProtocolEndpointBase::onChannelClosed: the router then frees the channel */
}

static void endpoint_opened(void *self, unsigned char channel)
{
    base_opened(self, channel);
    aa_logn(self == ep_video ? "AA: cluster video channel " :
            self == ep_input ? "AA: cluster input channel " : "AA: navigation channel ",
            (long)channel);
    if (self == ep_nav) {
        uint8_t msg[4];

        send_on(ep_nav, msg, aa_msg_nav_start(msg, sizeof(msg)));
    }
}

static void endpoint_noop(void *self)
{
    (void)self;
}

static void discovery_noop(void *self, void *response)
{
    (void)self;
    (void)response;
}

/* ---- the cluster input source ------------------------------------------------------------ */

static int input_route(void *self, unsigned channel, unsigned msg_id, const void *shared)
{
    unsigned n;
    const uint8_t *p = payload(shared, &n);
    uint8_t msg[8];

    (void)self;
    (void)channel;
    (void)msg_id;
    if (p && (((unsigned)p[0] << 8) | p[1]) == AA_MSG_KEY_BINDING_REQUEST) {
        send_on(ep_input, msg, aa_msg_key_binding_response(msg, sizeof(msg)));
    }
    return 0;
}

/* ---- navigation -------------------------------------------------------------------------- */

static void nav_publish(void)
{
    char text[1024];
    unsigned n = aa_nav_text(&nav, text, sizeof(text));

    if (!n || !mcn_active()) {
        return;
    }
    nav_active = text[7] == '1';
    nav_sent_ms = mcn_now_ms();
    mcn_link_send(text);
}

static int nav_route(void *self, unsigned channel, unsigned msg_id, const void *shared)
{
    unsigned n, id;
    const uint8_t *p = payload(shared, &n);

    (void)self;
    (void)channel;
    (void)msg_id;
    if (!p) {
        return 0;
    }
    id = ((unsigned)p[0] << 8) | p[1];
    if (route_logged < 8) {
        route_logged++;
        aa_logn("AA: navigation message ", (long)id);
    }
    if (aa_nav_message(&nav, id, p + 2, n - 2)) {
        nav_publish();
    }
    return 0;
}

/* ---- registration ------------------------------------------------------------------------ */

static void build_vtable(uint32_t *vt, void *route, void *opened, void *closed)
{
    memcpy(vt, base_vtable, AA_VTABLE_WORDS * 4);
    vt[2 + 0] = (uint32_t)(uintptr_t)endpoint_noop; /* never deleted: static objects */
    vt[2 + 1] = (uint32_t)(uintptr_t)endpoint_noop;
    if (closed) {
        vt[2 + 2] = (uint32_t)(uintptr_t)closed;
    }
    vt[2 + 4] = (uint32_t)(uintptr_t)opened;
    vt[2 + 5] = (uint32_t)(uintptr_t)route;
    vt[2 + 6] = (uint32_t)(uintptr_t)route;
    vt[2 + 7] = (uint32_t)(uintptr_t)discovery_noop;
}

static void build_endpoint(uint8_t *ep, const uint32_t *vt, unsigned id)
{
    memset(ep, 0, AA_ENDPOINT_SIZE);
    wr(ep, 0, (uint32_t)(uintptr_t)(vt + 2));
    wr(ep, 8, (uint32_t)(uintptr_t)router);
    ep[0x0c] = (uint8_t)id;
}

static int table_free(unsigned id)
{
    return rd(router, (id + 0x40) * 4) == 0;
}

static void register_endpoints(void)
{
    unsigned id;
    int ok;

    cluster_offered = !arrows_only();
    decode = mcn_decode_on();
    crop = cluster_resolution() == AA_RES_1280x720;
    for (id = AA_VIDEO_SERVICE_FIRST; id + 2 <= AA_VIDEO_SERVICE_LAST; id += 3) {
        if (table_free(id) && table_free(id + 1) && table_free(id + 2)) {
            break;
        }
    }
    if (id + 2 > AA_VIDEO_SERVICE_LAST) {
        aa_log("AA: no free service ids", "");
        return;
    }
    id_video = id;
    id_input = id + 1;
    id_nav = id + 2;
    build_vtable(vt_video, (void *)video_route, (void *)endpoint_opened, (void *)video_closed);
    build_vtable(vt_input, (void *)input_route, (void *)endpoint_opened, NULL);
    build_vtable(vt_nav, (void *)nav_route, (void *)endpoint_opened, NULL);
    build_endpoint(ep_video, vt_video, id_video);
    build_endpoint(ep_input, vt_input, id_input);
    build_endpoint(ep_nav, vt_nav, id_nav);
    ok = real_register(receiver, ep_nav) != 0;
    if (cluster_offered) {
        ok = real_register(receiver, ep_video) != 0 && ok;
        ok = real_register(receiver, ep_input) != 0 && ok;
    }
    registered = ok;
    aa_nav_reset(&nav);
    aa_logn(ok ? "AA: endpoints registered, first service id " : "AA: registration refused, id ",
            (long)id_video);
    aa_log(cluster_offered ? "AA: cluster display offered" : "AA: native arrows only", "");
    if (cluster_offered) {
        aa_log(decode ? "AA: cluster map decoded on the unit (800x480, our window on dpy-6)"
                      : "AA: cluster map passed through to the cluster",
               "");
    }
}

static void session_end(const char *why)
{
    if (streaming) {
        stream_end(why);
    }
    if (nav_active) {
        mcn_link_send("active=0\n");
        nav_active = 0;
    }
    registered = 0;
    rewritten = 0;
    pending_restart = 0;
    ((uint8_t *)ep_video)[4] = 0;
    ((uint8_t *)ep_input)[4] = 0;
    ((uint8_t *)ep_nav)[4] = 0;
}

/* ---- interposed exports of the receiver library ------------------------------------------ */

MCN_EXPORT int _ZN11GalReceiver4initERK10shared_ptrI20IControllerCallbacksE(void *self,
                                                                             const void *cb)
{
    int rc;

    if (!resolve()) {
        abort(); /* an unresolvable export under our name would leave SAL without init */
    }
    session_end("new session");
    receiver = self;
    router = NULL;
    registrations = 0;
    rc = real_init(self, cb); /* SAL starts Android Auto only when this is not 0 */
    if (!aa_enabled()) {
        aa_log("AA: off (GEM or kill switch): stock Android Auto", "");
        return rc;
    }
    if (!router) {
        /* No registration seen during init: the router sits at receiver + 4 on this build. */
        router = (uint8_t *)self + 4;
        aa_log("AA: router assumed at receiver + 4", "");
    }
    register_endpoints();
    return rc;
}

MCN_EXPORT int _ZN11GalReceiver15registerServiceEP20ProtocolEndpointBase(void *self,
                                                                          void *endpoint)
{
    int rc;

    if (!resolve()) {
        abort();
    }
    if (endpoint && endpoint != (void *)ep_video && endpoint != (void *)ep_input &&
        endpoint != (void *)ep_nav) {
        void *r = (void *)(uintptr_t)rd(endpoint, 8);

        /* The library's own endpoints carry the router: it is the receiver or embedded in it. */
        if (!router && (r == self || r == (void *)((uint8_t *)self + 4))) {
            router = r;
            aa_logn("AA: router at receiver + ", (long)((uint8_t *)r - (uint8_t *)self));
        }
        if (registrations++ < 8) {
            aa_logn("AA: stock endpoint registered, service ", ((uint8_t *)endpoint)[0x0c]);
        }
    }
    rc = real_register(self, endpoint);
    return rc;
}

MCN_EXPORT void _ZN11GalReceiver8shutdownEv(void *self)
{
    if (!resolve()) {
        abort();
    }
    session_end("shutdown");
    real_shutdown(self);
    aa_log("AA: receiver shut down", "");
}

/* The discovery response goes out here: the cluster is added on the wire (aa_proto.c). */
static int queue_outgoing(queue_fn real, void *r, unsigned char channel, void *buf, unsigned n)
{
    static uint8_t out[6144];
    const uint8_t *p = buf;

    if (outgoing_logged < 12 && p && n >= 2) {
        outgoing_logged++;
        aa_logn("AA: outgoing message ", ((long)p[0] << 8) | p[1]);
    }
    if (registered && channel == 0 && p && n >= 2 && p[0] == 0 &&
        p[1] == AA_MSG_SERVICE_DISCOVERY_RESPONSE) {
        struct aa_services s;
        unsigned k;

        memset(&s, 0, sizeof(s));
        s.video = id_video;
        s.input = id_input;
        s.nav = id_nav;
        s.res = cluster_resolution();
        s.dpi = (unsigned)file_number(AA_DPI_FILE, AA_DPI_DEFAULT);
        s.cluster = cluster_offered;
        s.arrows = 1;
        if (s.res == AA_RES_800x480) {
            int v[5];

            mcn_decode_layout(v);
            s.insets_set = 1;
            s.inset[0] = (unsigned)v[0];
            s.inset[1] = (unsigned)v[1];
            s.inset[2] = (unsigned)v[2];
            s.inset[3] = (unsigned)v[3];
        }
        out[0] = 0;
        out[1] = AA_MSG_SERVICE_DISCOVERY_RESPONSE;
        k = aa_discovery_rewrite(p + 2, n - 2, out + 2, sizeof(out) - 2, &s);
        if (k) {
            rewritten = 1;
            aa_logn("AA: discovery response rewritten, bytes ", k + 2);
            return real(r, channel, out, k + 2);
        }
        aa_log("AA: discovery response not understood; sent as it was", "");
    }
    return real(r, channel, buf, n);
}

MCN_EXPORT int _ZN13MessageRouter13queueOutgoingEhPvj(void *r, unsigned char channel, void *buf,
                                                     unsigned n)
{
    if (!resolve()) {
        abort();
    }
    return queue_outgoing(real_queue, r, channel, buf, n);
}

MCN_EXPORT int _ZN13MessageRouter24queueOutgoingUnencryptedEhPvj(void *r, unsigned char channel,
                                                                 void *buf, unsigned n)
{
    if (!resolve() || !real_queue_plain) {
        aa_log("AA: queueOutgoingUnencrypted called but not resolved; dropped", "");
        return 0;
    }
    return queue_outgoing(real_queue_plain, r, channel, buf, n);
}

/* Every incoming message passes here on the receiver's thread: the stream's housekeeping
 * runs between frames too (a still map sends none, the phone's pings keep coming). */
MCN_EXPORT int _ZN13MessageRouter12routeMessageEhRK10shared_ptrI8IoBufferE(void *r,
                                                                           unsigned char channel,
                                                                           const void *shared)
{
    if (!resolve() || !real_route) {
        aa_log("AA: routeMessage called but not resolved; dropped", "");
        return 0;
    }
    if (registered) {
        stream_tick();
    }
    return real_route(r, channel, shared);
}

/* Protocol 1.7: the phone then sends NavigationState / NavigationCurrentPosition. */
MCN_EXPORT int _ZN10Controller18sendVersionRequestEv(void *self)
{
    uint8_t msg[8];
    const uint8_t *ep = self;

    if (!resolve() || !real_version) {
        aa_log("AA: sendVersionRequest called but not resolved; dropped", "");
        return 0;
    }
    if (registered && real_queue_plain && ep && ep[4] && rd(ep, 8)) {
        real_queue_plain((void *)(uintptr_t)rd(ep, 8), ep[5], msg,
                         aa_msg_version_request(msg, sizeof(msg), 1, 7));
        aa_log("AA: version 1.7 requested", "");
        return 0; /* as the library's own request (1.3 on P0478T) */
    }
    return real_version(self);
}

MCN_EXPORT int _ZN10Controller21handleVersionResponseEPvj(void *self, void *data, unsigned n)
{
    uint8_t *p = data;

    if (!resolve() || !real_version_response) {
        aa_log("AA: handleVersionResponse called but not resolved; dropped", "");
        return 0;
    }
    if (registered && p && n >= 4) {
        unsigned major = ((unsigned)p[0] << 8) | p[1], minor = ((unsigned)p[2] << 8) | p[3];

        aa_logn("AA: phone protocol major ", (long)major);
        aa_logn("AA: phone protocol minor ", (long)minor);
        if (major > 1) {
            p[0] = 0;
            p[1] = 1;
            p[2] = 0;
            p[3] = 7;
        }
    }
    return real_version_response(self, data, n);
}

/* With 1.7 the phone sends messages the stock endpoints do not know; the receiver's
 * "unexpected message" reply makes the phone drop the session, silence does not. */
MCN_EXPORT int _ZN13MessageRouter21sendUnexpectedMessageEh(void *r, unsigned char channel)
{
    static unsigned count;

    if (!resolve() || !real_unexpected) {
        return 0;
    }
    if (registered) {
        if (count++ < 16) {
            aa_logn("AA: unexpected-message reply suppressed on channel ", (long)channel);
        }
        return 0;
    }
    return real_unexpected(r, channel);
}

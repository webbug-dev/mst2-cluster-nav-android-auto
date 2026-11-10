/*
 * mst2-cluster-nav - host check of the actual Android Auto hooks (src/sal/aa.c).
 * Android Auto branch. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * Built 32-bit, as the unit, around a stand-in for libext.google.gal.receiver.so that does
 * what the EU Skoda P0478T library does where the hooks depend on it (addresses of the
 * disassembled originals in the comments): the router at receiver + 4, its channel map and
 * endpoint table, the virtual calls it makes on an endpoint, and the values its functions
 * return. Calls the library makes through its PLT go to the interposed exports here too.
 */
#include "mcn_qnx.h"
#include "mcn_aa.h"
#include "mcn_video.h"

int puts(const char *);

/* Unbuffered: the reason must survive abort() when the output is a pipe. */
#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            write(2, "aa hook check failed: " #x "\n", sizeof(#x) + 22);      \
            abort();                                                          \
        }                                                                     \
    } while (0)

/* The interposed exports (aa.c). */
int _ZN11GalReceiver4initERK10shared_ptrI20IControllerCallbacksE(void *, const void *);
int _ZN11GalReceiver15registerServiceEP20ProtocolEndpointBase(void *, void *);
void _ZN11GalReceiver8shutdownEv(void *);
int _ZN13MessageRouter13queueOutgoingEhPvj(void *, unsigned char, void *, unsigned);
int _ZN13MessageRouter24queueOutgoingUnencryptedEhPvj(void *, unsigned char, void *, unsigned);
int _ZN13MessageRouter12routeMessageEhRK10shared_ptrI8IoBufferE(void *, unsigned char,
                                                                const void *);
int _ZN10Controller18sendVersionRequestEv(void *);
int _ZN10Controller21handleVersionResponseEPvj(void *, void *, unsigned);
int _ZN13MessageRouter21sendUnexpectedMessageEh(void *, unsigned char);
#define hook_init _ZN11GalReceiver4initERK10shared_ptrI20IControllerCallbacksE
#define hook_register _ZN11GalReceiver15registerServiceEP20ProtocolEndpointBase
#define hook_shutdown _ZN11GalReceiver8shutdownEv
#define hook_queue _ZN13MessageRouter13queueOutgoingEhPvj
#define hook_queue_plain _ZN13MessageRouter24queueOutgoingUnencryptedEhPvj
#define hook_route _ZN13MessageRouter12routeMessageEhRK10shared_ptrI8IoBufferE
#define hook_version _ZN10Controller18sendVersionRequestEv
#define hook_version_response _ZN10Controller21handleVersionResponseEPvj
#define hook_unexpected _ZN13MessageRouter21sendUnexpectedMessageEh

/* ---- the unit around the hooks ----------------------------------------------------------- */

static int active = 1, aa_off, arrows;
static unsigned now_ms = 5000;
static struct mcn_video video_mem;
static char link_text[1024];
static unsigned link_video;

int mcn_active(void)
{
    return active;
}

void mcn_log2(const char *a, const char *b)
{
    (void)a;
    (void)b;
}

void mcn_busy(int on)
{
    (void)on;
}

void mcn_busy_tick(void)
{
}

void mcn_link_send(const char *text)
{
    if (!strcmp(text, "video=1")) {
        link_video++;
    }
    snprintf(link_text, sizeof(link_text), "%s", text);
}

struct mcn_video *mcn_video_map(void)
{
    return &video_mem;
}

void mcn_video_wake(struct mcn_video *v)
{
    (void)v;
}

/* capture.c stand-in: counts what the hooks hand to the recording. */
static unsigned capture_begins, capture_ends, capture_units;

void mcn_capture_begin(void)
{
    capture_begins++;
}

void mcn_capture_put(const uint8_t *au, unsigned n, int idr)
{
    (void)au;
    (void)n;
    (void)idr;
    capture_units++;
}

void mcn_capture_end(void)
{
    capture_ends++;
}

/* decode.c stand-in: the switch, the stream tokens and what the hooks hand over. */
static int decode_on, decode_live, decode_want_key;
static unsigned decode_token, decode_begins, decode_ends, decode_units, decode_configs,
    decode_lost, decode_w, decode_h, decode_last_n, decode_last_idr, decode_last_token;
static uint8_t decode_last_config[256];
static unsigned decode_last_config_len;

int mcn_decode_on(void)
{
    return decode_on;
}

unsigned mcn_decode_begin(unsigned width, unsigned height)
{
    decode_begins++;
    decode_w = width;
    decode_h = height;
    return ++decode_token;
}

void mcn_decode_put(unsigned token, const uint8_t *au, unsigned n, int idr, const uint8_t *config,
                    unsigned config_len)
{
    CHECK(token == decode_token && au && n);
    decode_units++;
    decode_last_token = token;
    decode_last_n = n;
    decode_last_idr = idr != 0;
    if (config) {
        decode_configs++;
        CHECK(config_len && config_len <= sizeof(decode_last_config));
        memcpy(decode_last_config, config, config_len);
        decode_last_config_len = config_len;
    }
}

void mcn_decode_end(unsigned token)
{
    CHECK(token == decode_token);
    decode_ends++;
}

int mcn_decode_live(unsigned token)
{
    return token == decode_token && decode_live;
}

int mcn_decode_want_key(unsigned token)
{
    return token == decode_token && decode_want_key;
}

void mcn_decode_lost(unsigned token)
{
    CHECK(token == decode_token);
    decode_lost++;
}

int mcn_test_clock_gettime(int clock, struct timespec *ts)
{
    (void)clock;
    ts->tv_sec = (long)(now_ms / 1000);
    ts->tv_nsec = (long)(now_ms % 1000) * 1000000L;
    return 0;
}

/* The settings files: only GEM's "Android Auto: off" and the mode "arrows" exist, on demand. */
int mcn_test_open(const char *path, int flags, ...)
{
    (void)flags;
    if (strstr(path, "/aa-off")) {
        return aa_off ? 3 : -1;
    }
    if (strstr(path, "/mode")) {
        return arrows ? 4 : -1;
    }
    return -1;
}

ssize_t mcn_test_read(int fd, void *buf, size_t n)
{
    if (fd == 4 && n >= 6) {
        memcpy(buf, "arrows", 6);
        return 6;
    }
    return 0;
}

int mcn_test_close(int fd)
{
    (void)fd;
    return 0;
}

/* ---- the receiver library (P0478T) -------------------------------------------------------- */

#define ROUTER_OFFSET 4       /* GalReceiver::registerService 0x74574: add r0, r0, #4 */
#define CONTROLLER_OFFSET 0xd24 /* GalReceiver::init 0x7464c */
#define QUEUED 0x51           /* what ChannelManager::queueOutgoing answered */
#define QUEUED_PLAIN 0x52
#define UNEXPECTED 0x53

static uint32_t receiver_mem[0x400];
static uint8_t *const receiver = (uint8_t *)receiver_mem;
static uint8_t *const router = (uint8_t *)receiver_mem + ROUTER_OFFSET;
static uint8_t *const controller = (uint8_t *)receiver_mem + CONTROLLER_OFFSET;
static int init_result = 1;
static unsigned shutdowns, unexpected_sent, version_major, version_minor;

struct sent {
    unsigned channel, id, n;
    uint8_t data[4096];
};
static struct sent sent[8];
static unsigned sent_count;

static uint32_t word(const void *p, unsigned off)
{
    uint32_t v;

    memcpy(&v, (const uint8_t *)p + off, 4);
    return v;
}

static uint8_t *table(unsigned id)
{
    return (uint8_t *)(uintptr_t)word(router, (id + 0x40) * 4);
}

static const struct sent *last_sent(unsigned back)
{
    CHECK(sent_count > back);
    return &sent[(sent_count - 1 - back) % 8];
}

static int record(void *r, unsigned char channel, void *buf, unsigned n, int result)
{
    struct sent *s = &sent[sent_count++ % 8];

    CHECK(r == router && n >= 2 && n <= sizeof(s->data));
    s->channel = channel;
    s->id = ((unsigned)((uint8_t *)buf)[0] << 8) | ((uint8_t *)buf)[1];
    s->n = n;
    memcpy(s->data, buf, n);
    return result;
}

/* MessageRouter::queueOutgoing 0x73ae4 and queueOutgoingUnencrypted 0x73c78. */
static int lib_queue(void *r, unsigned char channel, void *buf, unsigned n)
{
    return record(r, channel, buf, n, QUEUED);
}

static int lib_queue_plain(void *r, unsigned char channel, void *buf, unsigned n)
{
    return record(r, channel, buf, n, QUEUED_PLAIN);
}

/* ProtocolEndpointBase 0x89120, 0x89130, 0x8913c, 0x703a8, 0x732a8. */
static void lib_opened(uint8_t *ep, unsigned char channel)
{
    ep[5] = channel;
    ep[4] = 1;
}

static int lib_may_open(uint8_t *ep, unsigned char channel)
{
    (void)channel;
    return ep[4] ^ 1;
}

static int lib_closed(uint8_t *ep, unsigned char channel)
{
    int open = ep[4];

    (void)channel;
    ep[4] = 0;
    return open != 0;
}

static int lib_no_route(void *ep, unsigned channel, unsigned type, const void *shared)
{
    (void)ep;
    (void)channel;
    (void)type;
    (void)shared;
    return -4;
}

static void lib_destroy(void *ep)
{
    (void)ep;
    CHECK(!"an endpoint of the library was destroyed");
}

/* _ZTV20ProtocolEndpointBase 0x15e4b0: 40 bytes, two header words and eight slots. */
static uint32_t lib_vtable[10];

/* MessageRouter::registerService 0x73a48, behind GalReceiver::registerService 0x74574. */
static int lib_register(void *self, void *endpoint)
{
    unsigned id = ((uint8_t *)endpoint)[0x0c];
    uint32_t p = (uint32_t)(uintptr_t)endpoint;

    CHECK(self == receiver);
    if (id == 0xff || table(id)) {
        return 0;
    }
    memcpy(router + (id + 0x40) * 4, &p, 4);
    return 1;
}

/* GalReceiver::init 0x7464c with MessageRouter::init 0x73de4: the tables are cleared, the
 * controller registered (through the PLT) and opened on channel 0; a bool comes back. */
static int lib_init(void *self, const void *callbacks)
{
    uint32_t p = (uint32_t)(uintptr_t)(lib_vtable + 2), r = (uint32_t)(uintptr_t)router;
    int ok;

    (void)callbacks;
    CHECK(self == receiver);
    memset(router, 0xff, 0x100);
    memset(router + 0x100, 0, 0x3fc);
    memset(controller, 0, 0x48);
    memcpy(controller, &p, 4);
    memcpy(controller + 8, &r, 4);
    ok = hook_register(self, controller);
    router[0] = 0; /* setupMapping(0, 0) */
    lib_opened(controller, 0);
    return (ok && init_result) & 0xff;
}

static void lib_shutdown(void *self)
{
    CHECK(self == receiver);
    shutdowns++;
}

/* MessageRouter::routeMessage 0x73f1c. */
static int lib_route(void *r, unsigned char channel, const void *shared)
{
    unsigned service = ((uint8_t *)r)[channel], type;
    const uint8_t *io, *p;
    const uint32_t *vt;
    uint8_t *ep;
    int rc;

    if (service == 0xff) {
        return -5;
    }
    ep = table(service);
    if (!ep) {
        return -4;
    }
    vt = (const uint32_t *)(uintptr_t)word(ep, 0);
    if (ep[0x10]) {
        rc = ((int (*)(void *, unsigned, const void *))(uintptr_t)vt[6])(ep, channel, shared);
    } else {
        io = (const uint8_t *)(uintptr_t)word(shared, 4);
        p = (const uint8_t *)(uintptr_t)word(io, 0) + word(io, 8);
        type = ((unsigned)p[0] << 8) | p[1]; /* extractType 0x73a38 */
        rc = ((int (*)(void *, unsigned, unsigned, const void *))(uintptr_t)vt[5])(ep, channel,
                                                                                    type, shared);
    }
    if (rc == -253) {
        return hook_unexpected(r, channel);
    }
    return rc;
}

/* Controller::sendVersionRequest 0x73138: 1.3, unencrypted, through the PLT; returns 0. */
static int lib_version(void *self)
{
    uint8_t msg[6] = { 0, 1, 0, 1, 0, 3 };
    const uint8_t *ep = self;

    if (ep[4]) {
        hook_queue_plain((void *)(uintptr_t)word(ep, 8), ep[5], msg, 6);
    }
    return 0;
}

/* Controller::handleVersionResponse 0x72708. */
static int lib_version_response(void *self, void *data, unsigned n)
{
    const uint8_t *p = data;

    CHECK(n >= 6);
    version_major = ((unsigned)p[0] << 8) | p[1];
    version_minor = ((unsigned)p[2] << 8) | p[3];
    ((uint8_t *)self)[0x11] = p[4] == 0;
    return 0;
}

/* MessageRouter::sendUnexpectedMessage 0x73e1c. */
static int lib_unexpected(void *r, unsigned char channel)
{
    (void)r;
    (void)channel;
    unexpected_sent++;
    return UNEXPECTED;
}

void *mcn_test_dlopen(const char *path, int flags)
{
    (void)flags;
    CHECK(!strcmp(path, "/tsd/lib/sal/gal/libext.google.gal.receiver.so"));
    return lib_vtable;
}

void *mcn_test_dlsym(void *handle, const char *name)
{
    static const struct {
        const char *name;
        void *at;
    } exports[] = {
        { "_ZN11GalReceiver15registerServiceEP20ProtocolEndpointBase", (void *)lib_register },
        { "_ZN11GalReceiver4initERK10shared_ptrI20IControllerCallbacksE", (void *)lib_init },
        { "_ZN11GalReceiver8shutdownEv", (void *)lib_shutdown },
        { "_ZN13MessageRouter13queueOutgoingEhPvj", (void *)lib_queue },
        { "_ZTV20ProtocolEndpointBase", lib_vtable },
        { "_ZN20ProtocolEndpointBase15onChannelOpenedEh", (void *)lib_opened },
        { "_ZN13MessageRouter24queueOutgoingUnencryptedEhPvj", (void *)lib_queue_plain },
        { "_ZN13MessageRouter12routeMessageEhRK10shared_ptrI8IoBufferE", (void *)lib_route },
        { "_ZN10Controller18sendVersionRequestEv", (void *)lib_version },
        { "_ZN10Controller21handleVersionResponseEPvj", (void *)lib_version_response },
        { "_ZN13MessageRouter21sendUnexpectedMessageEh", (void *)lib_unexpected },
    };
    unsigned i;

    CHECK(handle == lib_vtable);
    for (i = 0; i < sizeof(exports) / sizeof(exports[0]); i++) {
        if (!strcmp(name, exports[i].name)) {
            return exports[i].at;
        }
    }
    return NULL;
}

/* ---- the phone ---------------------------------------------------------------------------- */

/* handleChannelOpenReq 0x73964 and notifyChannelOpened 0x73fc0. */
static int phone_opens(unsigned channel, unsigned service)
{
    uint8_t *ep = table(service);
    const uint32_t *vt;

    if (router[channel] != 0xff) {
        return -5;
    }
    if (!ep) {
        return -4;
    }
    vt = (const uint32_t *)(uintptr_t)word(ep, 0);
    if (!((int (*)(void *, unsigned))(uintptr_t)vt[3])(ep, channel)) {
        return -253;
    }
    router[channel] = (uint8_t)service;
    ((void (*)(void *, unsigned))(uintptr_t)vt[4])(ep, channel);
    return 0;
}

/* handleChannelCloseNotif 0x739dc: the mapping goes only when the endpoint says it was open. */
static int phone_closes(unsigned channel)
{
    uint8_t *ep = table(router[channel]);
    const uint32_t *vt = (const uint32_t *)(uintptr_t)word(ep, 0);

    if (!((int (*)(void *, unsigned))(uintptr_t)vt[2])(ep, channel)) {
        return -7;
    }
    router[channel] = 0xff;
    return 0;
}

/* One message into the receiver, as ChannelManager::queueIncoming 0x7a914 hands it over. */
static int phone_sends(unsigned channel, const uint8_t *msg, unsigned n)
{
    static uint8_t data[MCN_VIDEO_SLOT_BYTES + 128];
    struct {
        uint32_t base, capacity, offset, end;
    } io;
    uint32_t shared[2];

    CHECK(n <= sizeof(data) - 16);
    memcpy(data + 16, msg, n); /* the payload does not start the buffer: offset is honoured */
    io.base = (uint32_t)(uintptr_t)data;
    io.capacity = sizeof(data);
    io.offset = 16;
    io.end = 16 + n;
    shared[0] = 0;
    shared[1] = (uint32_t)(uintptr_t)&io;
    return hook_route(router, (unsigned char)channel, shared);
}

static unsigned stock_discovery(uint8_t *out)
{
    /* Service { id 1, MediaSinkService { VIDEO, VideoConfiguration { 800x480, 30 fps } } }. */
    uint8_t config[8], sink[16], service[32];
    unsigned k, m, n;

    k = aa_pb_put_varint(config, sizeof(config), 1, 1);
    k += aa_pb_put_varint(config + k, sizeof(config) - k, 2, 2);
    m = aa_pb_put_varint(sink, sizeof(sink), 1, 3);
    m += aa_pb_put_bytes(sink + m, sizeof(sink) - m, 4, config, k);
    n = aa_pb_put_varint(service, sizeof(service), 1, 1);
    n += aa_pb_put_bytes(service + n, sizeof(service) - n, 3, sink, m);
    out[0] = 0;
    out[1] = AA_MSG_SERVICE_DISCOVERY_RESPONSE;
    return 2 + aa_pb_put_bytes(out + 2, 64, 1, service, n);
}

/* The cluster sink's VideoConfiguration of a rewritten ServiceDiscoveryResponse: resolution
 * (field 1) and margins (3, 4), -1 when absent. Service 2 is the first appended one, ours. */
static void cluster_config(const uint8_t *p, unsigned n, int *res, int *margin_w,
                           int *margin_h)
{
    struct aa_pb b = { p, p + n }, s, x, y, z;
    unsigned f, w;
    uint64_t v;
    int id;

    *res = *margin_w = *margin_h = -1;
    while (aa_pb_next(&b, &f, &w, &v, &s)) {
        if (f != 1 || w != 2) {
            continue;
        }
        id = -1;
        while (aa_pb_next(&s, &f, &w, &v, &x)) {
            if (f == 1 && w == 0) {
                id = (int)v;
            } else if (f == 3 && w == 2 && id >= 0x40) { /* MediaSinkService of the cluster */
                while (aa_pb_next(&x, &f, &w, &v, &y)) {
                    if (f == 4 && w == 2) { /* VideoConfiguration */
                        while (aa_pb_next(&y, &f, &w, &v, &z)) {
                            if (f == 1 && w == 0) {
                                *res = (int)v;
                            } else if (f == 3 && w == 0) {
                                *margin_w = (int)v;
                            } else if (f == 4 && w == 0) {
                                *margin_h = (int)v;
                            }
                        }
                    }
                }
                return;
            }
        }
    }
}

/* Service ids of a ServiceDiscoveryResponse, as a bit mask over 0x40..0x5f plus bit 0 for id 1. */
static unsigned discovered(const uint8_t *p, unsigned n)
{
    struct aa_pb b = { p, p + n }, s, x;
    unsigned f, w, f2, w2, mask = 0;
    uint64_t v, v2;

    while (aa_pb_next(&b, &f, &w, &v, &s)) {
        while (f == 1 && w == 2 && aa_pb_next(&s, &f2, &w2, &v2, &x)) {
            if (f2 == 1 && w2 == 0) {
                mask |= v2 == 1 ? 1u : v2 >= 0x40 && v2 < 0x5f ? 2u << (v2 - 0x40) : 0;
            }
        }
    }
    return mask;
}

int main(void)
{
    /* An SPS as x264 writes it for 1280x720 (test/aa_check.c), a PPS, an IDR slice. */
    static const uint8_t key[] = { 0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
                                   0,    0,    0,    1,    0x67, 0x64, 0x00, 0x1f, 0xac, 0xd9,
                                   0x40, 0x50, 0x05, 0xbb, 0x01, 0x6a, 0x02, 0x02, 0x02, 0x80,
                                   0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x1e, 0x47, 0x8c,
                                   0x18, 0xcb, 0,    0,    0,    1,    0x68, 0xcc, 0,    0,
                                   0,    1,    0x65, 0x11, 0x22 };
    static const uint8_t delta[] = { 0, 1, 0, 0, 0, 1, 0x41, 0x22, 0x33 };
    static const uint8_t setup[] = { 0x80, 0x00, 0x08, 0x03 };
    static const uint8_t start[] = { 0x80, 0x01, 0x08, 0x07, 0x10, 0x00 };
    static const uint8_t stop[] = { 0x80, 0x02 };
    static const uint8_t ping[] = { 0x00, 0x0b };
    static const uint8_t nav_status[] = { 0x80, 0x03, 0x08, 0x01 };
    uint8_t msg[128], stock_ep[0x48], response[8] = { 0, 2, 0, 1, 0, 0 };
    uint8_t *video, *input, *nav;
    uint32_t p;
    unsigned n, i, j;

    lib_vtable[2] = lib_vtable[3] = (uint32_t)(uintptr_t)lib_destroy;
    lib_vtable[4] = (uint32_t)(uintptr_t)lib_closed;
    lib_vtable[5] = (uint32_t)(uintptr_t)lib_may_open;
    lib_vtable[6] = (uint32_t)(uintptr_t)lib_opened;
    lib_vtable[7] = lib_vtable[8] = (uint32_t)(uintptr_t)lib_no_route;

    /* Off (kill switch, not enabled): everything passes through, init's answer included. */
    active = 0;
    CHECK(hook_init(receiver, NULL) == 1);
    CHECK(table(0) == controller && !table(0x40) && !table(0x41) && !table(0x42));
    init_result = 0;
    CHECK(hook_init(receiver, NULL) == 0);
    init_result = 1;
    CHECK(hook_unexpected(router, 0) == UNEXPECTED && unexpected_sent == 1);
    hook_shutdown(receiver);
    CHECK(shutdowns == 1);

    /* GEM "Android Auto: off". */
    active = 1;
    aa_off = 1;
    CHECK(hook_init(receiver, NULL) == 1 && !table(0x40) && !table(0x42));
    aa_off = 0;

    /* GEM "Native arrows only": the navigation status service alone. */
    arrows = 1;
    CHECK(hook_init(receiver, NULL) == 1 && !table(0x40) && !table(0x41) && table(0x42));
    arrows = 0;

    /* On: SAL's answer from init is the library's, and three endpoints are in the table. */
    init_result = 0;
    CHECK(hook_init(receiver, NULL) == 0);
    init_result = 1;
    CHECK(hook_init(receiver, NULL) == 1);
    video = table(0x40);
    input = table(0x41);
    nav = table(0x42);
    CHECK(table(0) == controller && video && input && nav);
    CHECK(video[0x0c] == 0x40 && input[0x0c] == 0x41 && nav[0x0c] == 0x42);
    CHECK(word(video, 8) == (uint32_t)(uintptr_t)router && !video[4] && !video[0x10]);
    CHECK(word((void *)(uintptr_t)word(video, 0), 3 * 4) == lib_vtable[5]); /* mayOpenChannel */

    /* SAL registers its own endpoints after init: the library's answer comes back. */
    memset(stock_ep, 0, sizeof(stock_ep));
    p = (uint32_t)(uintptr_t)(lib_vtable + 2);
    memcpy(stock_ep, &p, 4);
    stock_ep[0x0c] = 1;
    CHECK(hook_register(receiver, stock_ep) == 1 && table(1) == stock_ep);
    CHECK(hook_register(receiver, stock_ep) == 0);

    /* The version request asks for 1.7 and answers 0, as the library's own. */
    CHECK(hook_version(controller) == 0);
    CHECK(last_sent(0)->channel == 0 && last_sent(0)->n == 6 &&
          !memcmp(last_sent(0)->data, "\0\1\0\1\0\7", 6));
    CHECK(hook_version_response(controller, response, 6) == 0);
    CHECK(version_major == 1 && version_minor == 7 && controller[0x11] == 1);

    /* The discovery response leaves through queueOutgoing with the cluster's services added;
     * another message is not touched; both carry ChannelManager's answer back. */
    n = stock_discovery(msg);
    CHECK(hook_queue(router, 0, msg, n) == QUEUED);
    CHECK(last_sent(0)->id == AA_MSG_SERVICE_DISCOVERY_RESPONSE && last_sent(0)->n > n);
    CHECK(discovered(last_sent(0)->data + 2, last_sent(0)->n - 2) == (1u | 2u | 4u | 8u));
    CHECK(hook_queue(router, 0, (void *)ping, sizeof(ping)) == QUEUED);
    CHECK(last_sent(0)->n == sizeof(ping) && !memcmp(last_sent(0)->data, ping, sizeof(ping)));
    CHECK(hook_queue_plain(router, 0, (void *)ping, sizeof(ping)) == QUEUED_PLAIN);

    /* The phone opens the three channels; the navigation one is started at once. */
    CHECK(phone_opens(3, 0x40) == 0 && video[4] == 1 && video[5] == 3);
    CHECK(phone_opens(4, 0x41) == 0 && input[4] == 1 && input[5] == 4);
    CHECK(phone_opens(5, 0x42) == 0 && nav[4] == 1 && nav[5] == 5);
    CHECK(last_sent(0)->channel == 5 && last_sent(0)->id == AA_MSG_NAV_START);
    CHECK(phone_opens(6, 0x40) == -253); /* already open */

    /* Media channel: setup, start, a key frame, a delta frame; each frame is acknowledged. */
    CHECK(phone_sends(3, setup, sizeof(setup)) == 0);
    CHECK(last_sent(1)->id == AA_MSG_SETUP_RESPONSE && last_sent(1)->channel == 3);
    CHECK(last_sent(0)->id == AA_MSG_VIDEO_FOCUS && last_sent(0)->data[3] == AA_FOCUS_PROJECTED);
    i = video_mem.generation;
    CHECK(phone_sends(3, start, sizeof(start)) == 0);
    CHECK(video_mem.generation == i + 1 && video_mem.alive_ms && !video_mem.write_seq);
    CHECK(phone_sends(3, key, sizeof(key)) == 0);
    CHECK(video_mem.write_seq == 1 && (video_mem.slot[1].flags & MCN_VIDEO_IDR));
    CHECK(aa_nal_types(video_mem.slot[1].data, video_mem.slot[1].size) ==
          (AA_NAL_SPS | AA_NAL_PPS | AA_NAL_IDR));
    CHECK(last_sent(0)->id == AA_MSG_ACK && last_sent(0)->channel == 3 &&
          last_sent(0)->data[3] == 7); /* the session the phone started */
    CHECK(link_video == 1);
    CHECK(phone_sends(3, delta, sizeof(delta)) == 0);
    CHECK(video_mem.write_seq == 2 && !(video_mem.slot[2].flags & MCN_VIDEO_IDR));

    /* DisplayManager asks for a key frame: any incoming message runs the focus cycle, and the
     * phone's stop gives the focus back at once. */
    video_mem.want_idr = 1;
    now_ms += 2500;
    CHECK(phone_sends(0, ping, sizeof(ping)) == -4); /* the library's own answer */
    CHECK(last_sent(0)->id == AA_MSG_VIDEO_FOCUS && last_sent(0)->data[3] == AA_FOCUS_NATIVE);
    /* The stop inside our cycle is a pause in the same stream: it stays alive, and the phone's
     * new start neither changes the generation nor resets the sequence, so the key frame that
     * opens it is the next access unit for DisplayManager (car, 2026-11-09: as a new stream it
     * was skipped, asked for again, and the cycle ran every two seconds for ever). */
    i = video_mem.generation;
    CHECK(phone_sends(3, stop, sizeof(stop)) == 0 && video_mem.alive_ms);
    CHECK(last_sent(0)->id == AA_MSG_VIDEO_FOCUS && last_sent(0)->data[3] == AA_FOCUS_PROJECTED);
    CHECK(phone_sends(3, start, sizeof(start)) == 0);
    CHECK(video_mem.generation == i && video_mem.write_seq == 2 && video_mem.alive_ms);
    /* DisplayManager keeps asking while it waits; that must not start another cycle. */
    video_mem.want_idr = 1;
    now_ms += 2500;
    j = sent_count;
    CHECK(phone_sends(0, ping, sizeof(ping)) == -4 && sent_count == j);
    CHECK(phone_sends(3, key, sizeof(key)) == 0);
    CHECK(video_mem.generation == i && video_mem.write_seq == 3 &&
          (video_mem.slot[3].flags & MCN_VIDEO_IDR) && !video_mem.want_idr);
    CHECK(last_sent(0)->id == AA_MSG_ACK);
    now_ms += 2500;
    j = sent_count;
    CHECK(phone_sends(0, ping, sizeof(ping)) == -4 && sent_count == j); /* no further cycle */
    /* A later request (a lost frame) runs one cycle again. */
    video_mem.want_idr = 1;
    CHECK(phone_sends(0, ping, sizeof(ping)) == -4);
    CHECK(last_sent(0)->id == AA_MSG_VIDEO_FOCUS && last_sent(0)->data[3] == AA_FOCUS_NATIVE);
    CHECK(phone_sends(3, stop, sizeof(stop)) == 0 && phone_sends(3, start, sizeof(start)) == 0);
    CHECK(phone_sends(3, key, sizeof(key)) == 0 && video_mem.write_seq == 4 &&
          video_mem.generation == i);
    /* A stop the phone sends by itself ends the stream. */
    CHECK(phone_sends(3, stop, sizeof(stop)) == 0 && !video_mem.alive_ms);

    /* Navigation status reaches the HMI link. */
    CHECK(phone_sends(5, nav_status, sizeof(nav_status)) == 0);
    CHECK(!memcmp(link_text, "active=", 7));

    /* An unknown message on a stock channel: the "unexpected" reply stays in while ours run. */
    CHECK(hook_unexpected(router, 1) == 0 && unexpected_sent == 1);

    /* Closing the video channel frees it in the router, once. */
    CHECK(phone_closes(3) == 0 && !video[4] && router[3] == 0xff);
    CHECK(phone_opens(3, 0x40) == 0 && phone_closes(3) == 0);
    router[3] = 0x40;
    CHECK(phone_closes(3) == -7);

    hook_shutdown(receiver);
    CHECK(shutdowns == 2 && !nav[4] && !input[4]);
    CHECK(hook_unexpected(router, 1) == UNEXPECTED && unexpected_sent == 2);
    /* The diagnostic recording saw the streams begin, their units, and their ends. */
    CHECK(capture_begins >= 1 && capture_units >= 1 && capture_ends >= 1);
    /* Nothing went to the decoder so far: pass-through is untouched by its existence. */
    CHECK(!decode_begins && !decode_units && !decode_ends);

    /* ---- decode mode: the units go to the decoder on the unit, not to DisplayManager ---- */
    decode_on = 1;
    capture_begins = capture_units = capture_ends = 0;
    link_video = 0;
    CHECK(hook_init(receiver, NULL) == 1);
    video = table(0x40);
    CHECK(video && table(0x41) && table(0x42));
    /* The phone is asked for 800x480 without margins, whatever aa-res says (720 here). */
    n = stock_discovery(msg);
    CHECK(hook_queue(router, 0, msg, n) == QUEUED);
    CHECK(last_sent(0)->id == AA_MSG_SERVICE_DISCOVERY_RESPONSE);
    {
        int res, mw, mh;

        cluster_config(last_sent(0)->data + 2, last_sent(0)->n - 2, &res, &mw, &mh);
        CHECK(res == AA_RES_800x480 && mw == 0 && mh == 0);
    }
    CHECK(phone_opens(3, 0x40) == 0 && phone_opens(5, 0x42) == 0);
    CHECK(phone_sends(3, setup, sizeof(setup)) == 0);
    CHECK(last_sent(0)->id == AA_MSG_VIDEO_FOCUS && last_sent(0)->data[3] == AA_FOCUS_PROJECTED);
    i = video_mem.generation;
    j = video_mem.write_seq;
    memset(&video_mem, 0, sizeof(video_mem));
    video_mem.generation = i;
    video_mem.write_seq = j;
    CHECK(phone_sends(3, start, sizeof(start)) == 0);
    /* No decoder before the first SPS: it tells the coded size (1280x720 in this key frame). */
    CHECK(!decode_begins && video_mem.generation == i && !video_mem.alive_ms);
    CHECK(phone_sends(3, key, sizeof(key)) == 0);
    CHECK(decode_begins == 1 && decode_w == 1280 && decode_h == 720);
    CHECK(decode_units == 1 && decode_last_idr && decode_last_n == sizeof(key) - 10 &&
          decode_configs == 1);
    /* The parameter sets go along untouched: the SPS bytes of the phone, no crop rewrite. */
    CHECK(decode_last_config_len == 4 + 28 + 4 + 2);
    CHECK(!memcmp(decode_last_config + 4, key + 14, 28) &&
          !memcmp(decode_last_config + 4 + 28 + 4, key + 46, 2));
    /* The shared memory is left alone: nothing published, the stream not marked alive. */
    CHECK(video_mem.generation == i && video_mem.write_seq == j && !video_mem.alive_ms &&
          !video_mem.last_frame_ms);
    CHECK(last_sent(0)->id == AA_MSG_ACK && capture_begins == 1 && capture_units == 1);
    /* "video=1" only while the decoder shows a picture. */
    CHECK(link_video == 0);
    decode_live = 1;
    now_ms += 1500;
    CHECK(phone_sends(3, delta, sizeof(delta)) == 0);
    CHECK(decode_units == 2 && !decode_last_idr && decode_configs == 1 && link_video == 1);
    CHECK(capture_units == 2);
    /* DisplayManager's request means nothing here; no focus cycle without a lost unit. */
    video_mem.want_idr = 1;
    now_ms += 2500;
    j = sent_count;
    CHECK(phone_sends(0, ping, sizeof(ping)) == -4 && sent_count == j);
    /* A lost unit asks for a key frame by the focus cycle; the stream pauses and resumes
     * without a new decoder, and the key frame's parameter sets go to the decoder again. */
    decode_want_key = 1;
    CHECK(phone_sends(0, ping, sizeof(ping)) == -4);
    CHECK(last_sent(0)->id == AA_MSG_VIDEO_FOCUS && last_sent(0)->data[3] == AA_FOCUS_NATIVE);
    decode_want_key = 0;
    CHECK(phone_sends(3, stop, sizeof(stop)) == 0 && decode_ends == 0);
    CHECK(last_sent(0)->id == AA_MSG_VIDEO_FOCUS && last_sent(0)->data[3] == AA_FOCUS_PROJECTED);
    CHECK(phone_sends(3, start, sizeof(start)) == 0 && decode_begins == 1 && decode_ends == 0);
    CHECK(phone_sends(3, key, sizeof(key)) == 0);
    CHECK(decode_begins == 1 && decode_units == 3 && decode_configs == 2 && decode_last_idr);
    /* An oversized frame is told to the decoder as lost. */
    {
        static uint8_t big[MCN_VIDEO_SLOT_BYTES + 64];

        memcpy(big, delta, sizeof(delta));
        CHECK(phone_sends(3, big, sizeof(big)) == 0 && decode_lost == 1 && decode_units == 3);
    }
    /* The phone's own stop ends the decoder's stream; a new start begins another one. */
    CHECK(phone_sends(3, stop, sizeof(stop)) == 0 && decode_ends == 1 && capture_ends == 1);
    CHECK(phone_sends(3, start, sizeof(start)) == 0 && decode_begins == 1);
    CHECK(phone_sends(3, key, sizeof(key)) == 0 && decode_begins == 2 && decode_units == 4);
    /* Shutdown ends it too; the shared memory was never touched. */
    hook_shutdown(receiver);
    CHECK(decode_ends == 2 && !video_mem.alive_ms && video_mem.generation == i);

    /* Back to pass-through: the next connection reads the switch again. */
    decode_on = 0;
    decode_live = 0;
    CHECK(hook_init(receiver, NULL) == 1);
    CHECK(phone_opens(3, 0x40) == 0 && phone_sends(3, setup, sizeof(setup)) == 0);
    CHECK(phone_sends(3, start, sizeof(start)) == 0 && video_mem.generation == i + 1);
    CHECK(phone_sends(3, key, sizeof(key)) == 0 && video_mem.write_seq == 1 && decode_begins == 2);
    hook_shutdown(receiver);
    puts("aa hooks: init result, registration, discovery, media channel, key-frame cycle, "
         "navigation, version 1.7, channel close, decode mode on the P0478T receiver layout: ok");
    return 0;
}

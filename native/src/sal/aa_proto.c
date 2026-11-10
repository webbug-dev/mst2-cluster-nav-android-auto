/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Android Auto branch. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * Android Auto wire formats, independent of the unit (include/mcn_aa.h). Everything here is
 * plain data in, plain data out, so test/aa_check.c runs it on the host.
 */
#include "mcn_aa.h"

/* ---- protobuf ------------------------------------------------------------------------- */

static int varint(struct aa_pb *b, uint64_t *v)
{
    unsigned shift = 0;

    *v = 0;
    while (b->p < b->e && shift < 64) {
        uint8_t c = *b->p++;

        *v |= (uint64_t)(c & 0x7f) << shift;
        if (!(c & 0x80)) {
            return 1;
        }
        shift += 7;
    }
    return 0;
}

int aa_pb_next(struct aa_pb *b, unsigned *field, unsigned *wire, uint64_t *value,
               struct aa_pb *sub)
{
    uint64_t key, len;

    *value = 0;
    sub->p = sub->e = b->p;
    if (b->p >= b->e || !varint(b, &key) || key > 0xffffffffu) {
        return 0;
    }
    *field = (unsigned)(key >> 3);
    *wire = (unsigned)(key & 7);
    switch (*wire) {
    case 0:
        return varint(b, value);
    case 2:
        if (!varint(b, &len) || len > (uint64_t)(b->e - b->p)) {
            return 0;
        }
        sub->p = b->p;
        sub->e = b->p + len;
        b->p += len;
        return 1;
    case 5:
        if (b->e - b->p < 4) {
            return 0;
        }
        b->p += 4;
        return 1;
    case 1:
        if (b->e - b->p < 8) {
            return 0;
        }
        b->p += 8;
        return 1;
    default:
        return 0;
    }
}

unsigned aa_pb_varint(uint8_t *out, uint64_t v)
{
    unsigned n = 0;

    while (v >= 0x80) {
        out[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    out[n++] = (uint8_t)v;
    return n;
}

unsigned aa_pb_varint_size(uint64_t v)
{
    unsigned n = 1;

    while (v >= 0x80) {
        v >>= 7;
        n++;
    }
    return n;
}

unsigned aa_pb_put_varint(uint8_t *out, unsigned cap, unsigned field, uint64_t v)
{
    unsigned n = aa_pb_varint_size((uint64_t)field << 3) + aa_pb_varint_size(v);

    if (n > cap) {
        return 0;
    }
    n = aa_pb_varint(out, (uint64_t)field << 3);
    n += aa_pb_varint(out + n, v);
    return n;
}

unsigned aa_pb_put_bytes(uint8_t *out, unsigned cap, unsigned field, const uint8_t *p,
                         unsigned n)
{
    unsigned k = aa_pb_varint_size(((uint64_t)field << 3) | 2) + aa_pb_varint_size(n) + n;

    if (k > cap) {
        return 0;
    }
    k = aa_pb_varint(out, ((uint64_t)field << 3) | 2);
    k += aa_pb_varint(out + k, n);
    if (n) {
        memcpy(out + k, p, n);
    }
    return k + n;
}

/* ---- service discovery --------------------------------------------------------------- */

/* Copies one parsed field as it was. */
static unsigned copy_field(uint8_t *out, unsigned cap, unsigned field, unsigned wire,
                           uint64_t value, const struct aa_pb *sub, const uint8_t *raw_start,
                           const uint8_t *raw_end)
{
    unsigned n;

    switch (wire) {
    case 0:
        return aa_pb_put_varint(out, cap, field, value);
    case 2:
        return aa_pb_put_bytes(out, cap, field, sub->p, (unsigned)(sub->e - sub->p));
    default:
        n = (unsigned)(raw_end - raw_start);
        if (n > cap) {
            return 0;
        }
        memcpy(out, raw_start, n);
        return n;
    }
}

/* The main screen's frame-rate enum, from the first video configuration of a stock video
 * sink; 0 when the response has none. */
static unsigned stock_fps(const uint8_t *in, unsigned n)
{
    struct aa_pb b = { in, in + n }, service, sink, config, x;
    unsigned f, w, f2, w2, f3, w3, f4, w4;
    uint64_t v, v2, v3, v4;

    while (aa_pb_next(&b, &f, &w, &v, &service)) {
        if (f != 1 || w != 2) {
            continue;
        }
        while (aa_pb_next(&service, &f2, &w2, &v2, &sink)) {
            int video = 0;
            struct aa_pb s = sink;

            if (f2 != 3 || w2 != 2) {
                continue;
            }
            while (aa_pb_next(&s, &f3, &w3, &v3, &config)) {
                if (f3 == 1 && w3 == 0 && v3 == 3) {
                    video = 1;
                }
                if (f3 == 4 && w3 == 2 && video) {
                    while (aa_pb_next(&config, &f4, &w4, &v4, &x)) {
                        if (f4 == 2 && w4 == 0 && v4 && v4 < 16) {
                            return (unsigned)v4;
                        }
                    }
                }
            }
        }
    }
    return 0;
}

/* Rewrites a Service: display ids on its media sink and input source. */
static unsigned rewrite_service(const struct aa_pb *in, uint8_t *out, unsigned cap)
{
    struct aa_pb b = *in, sub, s2, sub2;
    unsigned f, w, f2, w2, used = 0, n;
    uint64_t v, v2;

    while (1) {
        const uint8_t *start = b.p;

        if (b.p >= b.e) {
            break;
        }
        if (!aa_pb_next(&b, &f, &w, &v, &sub)) {
            return 0;
        }
        if (w == 2 && (f == 3 || f == 4)) {
            /* MediaSinkService (3) or InputSourceService (4): copy, then the display tag. */
            uint8_t body[2048];
            unsigned k = 0, video = 0;

            s2 = sub;
            while (s2.p < s2.e) {
                const uint8_t *s = s2.p;

                if (!aa_pb_next(&s2, &f2, &w2, &v2, &sub2)) {
                    return 0;
                }
                if (f == 3 && f2 == 1 && w2 == 0 && v2 == 3) {
                    video = 1;
                }
                n = copy_field(body + k, sizeof(body) - k, f2, w2, v2, &sub2, s, s2.p);
                if (!n) {
                    return 0;
                }
                k += n;
            }
            if (f == 3 && video) {
                n = aa_pb_put_varint(body + k, sizeof(body) - k, 6, 0); /* display_id MAIN */
                if (!n) {
                    return 0;
                }
                k += n;
                n = aa_pb_put_varint(body + k, sizeof(body) - k, 7, 0); /* display_type MAIN */
                if (!n) {
                    return 0;
                }
                k += n;
            } else if (f == 4) {
                n = aa_pb_put_varint(body + k, sizeof(body) - k, 5, 0); /* display_id */
                if (!n) {
                    return 0;
                }
                k += n;
            }
            n = aa_pb_put_bytes(out + used, cap - used, f, body, k);
        } else {
            n = copy_field(out + used, cap - used, f, w, v, &sub, start, b.p);
        }
        if (!n) {
            return 0;
        }
        used += n;
    }
    return used;
}

/* UiConfig { content_insets = 2 { top 1, bottom 2, left 3, right 4 }; ui_theme = 4 }. */
static unsigned ui_config(uint8_t *out, unsigned cap, unsigned top, unsigned bottom,
                          unsigned left, unsigned right)
{
    uint8_t insets[24];
    unsigned k = 0, n;

    k += aa_pb_put_varint(insets + k, sizeof(insets) - k, 1, top);
    k += aa_pb_put_varint(insets + k, sizeof(insets) - k, 2, bottom);
    k += aa_pb_put_varint(insets + k, sizeof(insets) - k, 3, left);
    k += aa_pb_put_varint(insets + k, sizeof(insets) - k, 4, right);
    n = aa_pb_put_bytes(out, cap, 2, insets, k);
    if (!n) {
        return 0;
    }
    k = aa_pb_put_varint(out + n, cap - n, 4, 2); /* dark */
    return k ? n + k : 0;
}

/* The cluster's video sink service. */
static unsigned cluster_video_service(uint8_t *out, unsigned cap, const struct aa_services *s)
{
    uint8_t config[96], sink[160], service[192], ui[40];
    unsigned k = 0, n, m = 0, margin_w = 0, margin_h = 0, ui_n = 0;

    if (s->res == AA_RES_1280x720) {
        margin_w = AA_STREAM_W - AA_VIEW_W;
        margin_h = AA_STREAM_H - AA_VIEW_H;
        /*
         * The safe area of the stock cluster window (sal.h MCN_SAFE_*: what the dials leave
         * free), in the coordinates of the full 1280x720 frame the phone lays out.
         */
        ui_n = ui_config(ui, sizeof(ui), AA_VIEW_Y + 56, AA_VIEW_Y + (AA_VIEW_H - 306),
                         AA_VIEW_X + 377, AA_VIEW_X + (AA_VIEW_W - 634));
    } else {
        /*
         * 800x480 (decode mode: decode.c shows the centred 800x300 band, rows 90..389). The
         * phone centres the car in the content area and puts its "Google Maps" logo at the
         * area's bottom edge (car, 2026-11-10: insets 56/174 put the logo at rows 282..303,
         * in the middle of the band). With the bottom inset 0 the logo lands in rows ~455..477,
         * below the band, and the car sits at row 285, 65 % down the band: road ahead above.
         */
        ui_n = ui_config(ui, sizeof(ui), AA_DEC_VIEW_Y, 0, 300, 800 - 500);
    }
    k += aa_pb_put_varint(config + k, sizeof(config) - k, 1, s->res);
    k += aa_pb_put_varint(config + k, sizeof(config) - k, 2, s->fps);
    k += aa_pb_put_varint(config + k, sizeof(config) - k, 3, margin_w);
    k += aa_pb_put_varint(config + k, sizeof(config) - k, 4, margin_h);
    k += aa_pb_put_varint(config + k, sizeof(config) - k, 5, s->dpi);
    k += aa_pb_put_varint(config + k, sizeof(config) - k, 6, 3);     /* decoder depth */
    k += aa_pb_put_varint(config + k, sizeof(config) - k, 7, 700);   /* viewing distance */
    k += aa_pb_put_varint(config + k, sizeof(config) - k, 8, 10000); /* square pixels */
    if (ui_n) {
        k += aa_pb_put_bytes(config + k, sizeof(config) - k, 11, ui, ui_n);
    }
    m += aa_pb_put_varint(sink + m, sizeof(sink) - m, 1, 3); /* available_type VIDEO */
    m += aa_pb_put_bytes(sink + m, sizeof(sink) - m, 4, config, k);
    m += aa_pb_put_varint(sink + m, sizeof(sink) - m, 5, 1); /* available while in call */
    m += aa_pb_put_varint(sink + m, sizeof(sink) - m, 6, AA_DISPLAY_CLUSTER);
    m += aa_pb_put_varint(sink + m, sizeof(sink) - m, 7, AA_DISPLAY_CLUSTER);
    n = aa_pb_put_varint(service, sizeof(service), 1, s->video);
    n += aa_pb_put_bytes(service + n, sizeof(service) - n, 3, sink, m);
    return aa_pb_put_bytes(out, cap, 1, service, n);
}

/* The cluster's input source: a display needs one; DPAD_CENTER only, no touch. */
static unsigned cluster_input_service(uint8_t *out, unsigned cap, const struct aa_services *s)
{
    static const uint8_t keycodes[] = { 0x17, 0x80, 0x80, 0x04 }; /* packed 23, 65536 */
    uint8_t input[16], service[32];
    unsigned k, n;

    k = aa_pb_put_bytes(input, sizeof(input), 1, keycodes, sizeof(keycodes));
    k += aa_pb_put_varint(input + k, sizeof(input) - k, 5, AA_DISPLAY_CLUSTER);
    n = aa_pb_put_varint(service, sizeof(service), 1, s->input);
    n += aa_pb_put_bytes(service + n, sizeof(service) - n, 4, input, k);
    return aa_pb_put_bytes(out, cap, 1, service, n);
}

/* NavigationStatusService { minimum_interval_ms = 1; type = 2 (ENUM); } */
static unsigned nav_service(uint8_t *out, unsigned cap, const struct aa_services *s)
{
    uint8_t nav[16], service[32];
    unsigned k, n;

    k = aa_pb_put_varint(nav, sizeof(nav), 1, 1000);
    k += aa_pb_put_varint(nav + k, sizeof(nav) - k, 2, 2);
    n = aa_pb_put_varint(service, sizeof(service), 1, s->nav);
    n += aa_pb_put_bytes(service + n, sizeof(service) - n, 8, nav, k);
    return aa_pb_put_bytes(out, cap, 1, service, n);
}

unsigned aa_discovery_rewrite(const uint8_t *in, unsigned n, uint8_t *out, unsigned cap,
                              const struct aa_services *s)
{
    struct aa_pb b = { in, in + n }, sub;
    struct aa_services cfg = *s;
    unsigned f, w, used = 0, k;
    uint64_t v;

    if (!cfg.fps) {
        cfg.fps = stock_fps(in, n);
        if (!cfg.fps) {
            cfg.fps = 1; /* VideoFrameRateType 30 */
        }
    }
    while (b.p < b.e) {
        const uint8_t *start = b.p;

        if (!aa_pb_next(&b, &f, &w, &v, &sub)) {
            return 0;
        }
        if (f == 1 && w == 2 && cfg.cluster) {
            uint8_t body[2048];

            k = rewrite_service(&sub, body, sizeof(body));
            if (!k) {
                return 0;
            }
            k = aa_pb_put_bytes(out + used, cap - used, 1, body, k);
        } else {
            k = copy_field(out + used, cap - used, f, w, v, &sub, start, b.p);
        }
        if (!k) {
            return 0;
        }
        used += k;
    }
    if (cfg.cluster) {
        k = cluster_video_service(out + used, cap - used, &cfg);
        if (!k) {
            return 0;
        }
        used += k;
        k = cluster_input_service(out + used, cap - used, &cfg);
        if (!k) {
            return 0;
        }
        used += k;
    }
    if (cfg.arrows) {
        k = nav_service(out + used, cap - used, &cfg);
        if (!k) {
            return 0;
        }
        used += k;
    }
    return used;
}

/* ---- messages ------------------------------------------------------------------------- */

static unsigned put_id(uint8_t *out, unsigned cap, unsigned id)
{
    if (cap < 2) {
        return 0;
    }
    out[0] = (uint8_t)(id >> 8);
    out[1] = (uint8_t)id;
    return 2;
}

unsigned aa_msg_setup_response(uint8_t *out, unsigned cap, unsigned status,
                               unsigned max_unacked)
{
    unsigned n = put_id(out, cap, AA_MSG_SETUP_RESPONSE), k;

    if (!n) {
        return 0;
    }
    k = aa_pb_put_varint(out + n, cap - n, 1, status);
    k += aa_pb_put_varint(out + n + k, cap - n - k, 2, max_unacked);
    k += aa_pb_put_varint(out + n + k, cap - n - k, 3, 0); /* configuration index 0 */
    return n + k;
}

unsigned aa_msg_ack(uint8_t *out, unsigned cap, int session, unsigned count)
{
    unsigned n = put_id(out, cap, AA_MSG_ACK), k;

    if (!n) {
        return 0;
    }
    k = aa_pb_put_varint(out + n, cap - n, 1, (uint64_t)(int64_t)session); /* int32: sign-extended */
    k += aa_pb_put_varint(out + n + k, cap - n - k, 2, count);
    return n + k;
}

unsigned aa_msg_video_focus(uint8_t *out, unsigned cap, unsigned mode, int unsolicited)
{
    unsigned n = put_id(out, cap, AA_MSG_VIDEO_FOCUS), k;

    if (!n) {
        return 0;
    }
    k = aa_pb_put_varint(out + n, cap - n, 1, mode);
    k += aa_pb_put_varint(out + n + k, cap - n - k, 2, unsolicited ? 1 : 0);
    return n + k;
}

unsigned aa_msg_version_request(uint8_t *out, unsigned cap, unsigned major, unsigned minor)
{
    if (cap < 6) {
        return 0;
    }
    out[0] = 0;
    out[1] = AA_MSG_VERSION_REQUEST;
    out[2] = (uint8_t)(major >> 8);
    out[3] = (uint8_t)major;
    out[4] = (uint8_t)(minor >> 8);
    out[5] = (uint8_t)minor;
    return 6;
}

unsigned aa_msg_key_binding_response(uint8_t *out, unsigned cap)
{
    unsigned n = put_id(out, cap, AA_MSG_KEY_BINDING_RESPONSE), k;

    if (!n) {
        return 0;
    }
    k = aa_pb_put_varint(out + n, cap - n, 1, 0); /* status OK */
    return n + k;
}

unsigned aa_msg_nav_start(uint8_t *out, unsigned cap)
{
    return put_id(out, cap, AA_MSG_NAV_START);
}

int aa_parse_setup_request(const uint8_t *p, unsigned n)
{
    struct aa_pb b = { p, p + n }, sub;
    unsigned f, w;
    uint64_t v;

    while (aa_pb_next(&b, &f, &w, &v, &sub)) {
        if (f == 1 && w == 0) {
            return (int)v;
        }
    }
    return -1;
}

int aa_parse_start(const uint8_t *p, unsigned n, int *session, unsigned *config)
{
    struct aa_pb b = { p, p + n }, sub;
    unsigned f, w;
    uint64_t v;
    int have = 0;

    *session = 0;
    *config = 0;
    while (aa_pb_next(&b, &f, &w, &v, &sub)) {
        if (f == 1 && w == 0) {
            *session = (int)(int32_t)v;
            have = 1;
        } else if (f == 2 && w == 0) {
            *config = (unsigned)v;
        }
    }
    return have;
}

unsigned aa_parse_focus_request(const uint8_t *p, unsigned n)
{
    struct aa_pb b = { p, p + n }, sub;
    unsigned f, w;
    uint64_t v;

    while (aa_pb_next(&b, &f, &w, &v, &sub)) {
        if (f == 2 && w == 0) {
            return (unsigned)v;
        }
    }
    return 0;
}

/* ---- navigation ----------------------------------------------------------------------- */

void aa_nav_reset(struct aa_nav *n)
{
    memset(n, 0, sizeof(*n));
    n->status = -1;
    n->type = -1;
    n->legacy = -1;
    n->side = 3;
    n->distance = n->time = n->dest_distance = n->dest_time = -1;
}

static void text(char *out, unsigned cap, const struct aa_pb *s)
{
    unsigned i, j = 0;

    for (i = 0; i < (unsigned)(s->e - s->p) && j < cap - 1; i++) {
        char c = (char)s->p[i];

        out[j++] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    out[j] = 0;
}

static void clear_step(struct aa_nav *n)
{
    n->type = -1;
    n->legacy = -1;
    n->side = 3;
    n->exit_number = 0;
    n->exit_angle = 0;
    n->road[0] = 0;
    n->lane_count = 0;
}

static void parse_lane(struct aa_nav *n, struct aa_pb lane)
{
    struct aa_pb dir, x;
    unsigned f, w, f2, w2, count = 0, i;
    uint64_t v, v2;
    unsigned char shapes[4];
    int highlighted = -1;

    if (n->lane_count >= AA_NAV_LANES) {
        return;
    }
    for (i = 0; i < 4; i++) {
        shapes[i] = 10;
    }
    while (aa_pb_next(&lane, &f, &w, &v, &dir)) {
        unsigned shape = 0, hl = 0;

        if (f != 1 || w != 2) {
            continue;
        }
        while (aa_pb_next(&dir, &f2, &w2, &v2, &x)) {
            if (f2 == 1 && w2 == 0) {
                shape = v2 <= 9 ? (unsigned)v2 : 0;
            } else if (f2 == 2 && w2 == 0) {
                hl = v2 != 0;
            }
        }
        if (count < 4) {
            shapes[count] = (unsigned char)shape;
            if (hl && highlighted < 0) {
                highlighted = (int)count;
            }
            count++;
        }
    }
    if (!count) {
        return;
    }
    i = n->lane_count++;
    memcpy(n->lane_shapes[i], shapes, 4);
    n->lane_shape[i] = shapes[highlighted >= 0 ? (unsigned)highlighted : 0];
    n->lane_highlight[i] = highlighted >= 0;
}

static void parse_step(struct aa_nav *n, struct aa_pb step)
{
    struct aa_pb sub, x;
    unsigned f, w, f2, w2;
    uint64_t v, v2;
    char cue[AA_NAV_TEXT];

    cue[0] = 0;
    clear_step(n);
    while (aa_pb_next(&step, &f, &w, &v, &sub)) {
        if (w != 2) {
            continue;
        }
        if (f == 1) { /* NavigationManeuver { type 1, exit number 2, exit angle 3 } */
            while (aa_pb_next(&sub, &f2, &w2, &v2, &x)) {
                if (w2 != 0) {
                    continue;
                }
                if (f2 == 1) {
                    n->type = v2 <= 50 ? (int)v2 : 0;
                } else if (f2 == 2) {
                    n->exit_number = v2 >= 1 && v2 <= 99 ? (int)v2 : 0;
                } else if (f2 == 3) {
                    n->exit_angle = v2 >= 1 && v2 <= 360 ? (int)v2 : 0;
                }
            }
        } else if (f == 2) { /* NavigationText { text 1 } */
            while (aa_pb_next(&sub, &f2, &w2, &v2, &x)) {
                if (f2 == 1 && w2 == 2) {
                    text(cue, sizeof(cue), &x);
                }
            }
        } else if (f == 3) {
            parse_lane(n, sub);
        } else if (f == 4) { /* NavigationRoadInfo { repeated road_names 1 } */
            while (aa_pb_next(&sub, &f2, &w2, &v2, &x)) {
                if (f2 == 1 && w2 == 2 && !n->road[0]) {
                    text(n->road, sizeof(n->road), &x);
                }
            }
        }
    }
    if (!n->road[0]) {
        memcpy(n->road, cue, sizeof(cue));
    }
}

static int parse_state(struct aa_nav *n, const uint8_t *p, unsigned len)
{
    struct aa_pb b = { p, p + len }, sub;
    unsigned f, w;
    uint64_t v;

    while (aa_pb_next(&b, &f, &w, &v, &sub)) {
        if (f == 1 && w == 2) {
            parse_step(n, sub);
            if (n->status < 0) {
                n->status = 1;
            }
            return 1;
        }
    }
    return 0;
}

/* NavigationDistance { meters 1; display 2; units 3 } */
static int parse_distance(struct aa_pb d)
{
    struct aa_pb x;
    unsigned f, w;
    uint64_t v;
    int meters = -1;

    while (aa_pb_next(&d, &f, &w, &v, &x)) {
        if (f == 1 && w == 0) {
            meters = v <= 20000000u ? (int)v : -1;
        }
    }
    return meters;
}

static int parse_position(struct aa_nav *n, const uint8_t *p, unsigned len)
{
    struct aa_pb b = { p, p + len }, sub, x;
    unsigned f, w, f2, w2;
    uint64_t v, v2;
    int any = 0;

    while (aa_pb_next(&b, &f, &w, &v, &sub)) {
        if (w != 2) {
            continue;
        }
        if (f == 1) { /* NavigationStepDistance { distance 1; time_to_step_seconds 2 } */
            while (aa_pb_next(&sub, &f2, &w2, &v2, &x)) {
                if (f2 == 1 && w2 == 2) {
                    n->distance = parse_distance(x);
                    any = 1;
                } else if (f2 == 2 && w2 == 0) {
                    n->time = v2 <= 1000000u ? (int)v2 : -1;
                }
            }
        } else if (f == 2) { /* NavigationDestinationDistance { distance 1; eta 2; time 3 } */
            while (aa_pb_next(&sub, &f2, &w2, &v2, &x)) {
                if (f2 == 1 && w2 == 2) {
                    n->dest_distance = parse_distance(x);
                } else if (f2 == 3 && w2 == 0) {
                    n->dest_time = v2 <= 1000000u ? (int)v2 : -1;
                }
            }
        } else if (f == 3) { /* NavigationRoad { name 1 } */
            while (aa_pb_next(&sub, &f2, &w2, &v2, &x)) {
                if (f2 == 1 && w2 == 2) {
                    text(n->current, sizeof(n->current), &x);
                }
            }
        }
    }
    return any;
}

/* Legacy 1.x pair: NextTurnEvent { road 1, side 2, event 3, image 4, angle 5, number 6 } and
 * NextTurnDistanceEvent { meters 1, seconds 2, display 3, units 4 }. */
static int parse_legacy_turn(struct aa_nav *n, const uint8_t *p, unsigned len)
{
    struct aa_pb b = { p, p + len }, sub;
    unsigned f, w;
    uint64_t v;

    clear_step(n);
    while (aa_pb_next(&b, &f, &w, &v, &sub)) {
        if (f == 1 && w == 2) {
            text(n->road, sizeof(n->road), &sub);
        } else if (f == 2 && w == 0) {
            n->side = v >= 1 && v <= 3 ? (int)v : 3;
        } else if (f == 3 && w == 0) {
            n->legacy = v <= 17 ? (int)v : 0;
        } else if (f == 5 && w == 0) {
            n->exit_angle = v >= 1 && v <= 360 ? (int)v : 0;
        } else if (f == 6 && w == 0) {
            n->exit_number = v >= 1 && v <= 99 ? (int)v : 0;
        }
    }
    if (n->status < 0) {
        n->status = 1;
    }
    return 1;
}

static int parse_legacy_distance(struct aa_nav *n, const uint8_t *p, unsigned len)
{
    struct aa_pb b = { p, p + len }, sub;
    unsigned f, w;
    uint64_t v;

    while (aa_pb_next(&b, &f, &w, &v, &sub)) {
        if (f == 1 && w == 0) {
            n->distance = v <= 20000000u ? (int)v : -1;
        } else if (f == 2 && w == 0) {
            n->time = v <= 1000000u ? (int)v : -1;
        }
    }
    return 1;
}

int aa_nav_message(struct aa_nav *n, unsigned id, const uint8_t *p, unsigned len)
{
    struct aa_pb b = { p, p + len }, sub;
    unsigned f, w;
    uint64_t v;
    int changed = 0;

    switch (id) {
    case AA_MSG_NAV_STATUS:
        while (aa_pb_next(&b, &f, &w, &v, &sub)) {
            if (f == 1 && w == 0 && v <= 3) {
                n->status = (int)v;
                changed = 1;
                if (v != 1 && v != 3) {
                    n->dest_distance = n->dest_time = -1;
                }
            }
        }
        break;
    case AA_MSG_NAV_NEXT_TURN:
        changed = parse_legacy_turn(n, p, len);
        break;
    case AA_MSG_NAV_NEXT_TURN_DISTANCE:
        changed = parse_legacy_distance(n, p, len);
        break;
    case AA_MSG_NAV_STATE:
        changed = parse_state(n, p, len);
        break;
    case AA_MSG_NAV_POSITION:
        changed = parse_position(n, p, len);
        break;
    default:
        break;
    }
    if (changed) {
        n->updated++;
    }
    return changed;
}

/*
 * AA ManeuverType (the 1.7 numbering: 1 DEPART .. 42 DESTINATION_RIGHT, 43..50 the later
 * roundabout and ferry variants) to the iAP2 maneuver type Maneuvers.java draws, with an
 * iAP2-style angle (degrees, negative = left; 1000 = none) where the type alone is not enough.
 */
int aa_nav_iap2_type(int type, int exit_number, int *angle_out)
{
    int angle = 1000, t;

    switch (type) {
    case 1: t = 11; break;                     /* DEPART: start route */
    case 2: t = 5; break;                      /* NAME_CHANGE: follow road */
    case 3: case 25: case 27: t = 13; break;   /* KEEP_LEFT, FORK_LEFT, MERGE_LEFT */
    case 4: case 26: case 28: t = 14; break;   /* KEEP_RIGHT, FORK_RIGHT, MERGE_RIGHT */
    case 29: t = 3; break;                     /* MERGE unspecified: straight */
    case 5: t = 49; break;                     /* SLIGHT_LEFT */
    case 6: t = 50; break;                     /* SLIGHT_RIGHT */
    case 7: t = 1; break;                      /* NORMAL_LEFT */
    case 8: t = 2; break;                      /* NORMAL_RIGHT */
    case 9: t = 47; break;                     /* SHARP_LEFT */
    case 10: t = 48; break;                    /* SHARP_RIGHT */
    case 11: case 19: t = 4; angle = -180; break; /* U_TURN_LEFT, ON_RAMP_U_TURN_LEFT */
    case 12: case 20: t = 4; angle = 180; break;
    case 13: t = 9; angle = -45; break;        /* ON_RAMP_SLIGHT_LEFT */
    case 14: t = 9; angle = 45; break;
    case 15: t = 9; angle = -90; break;        /* ON_RAMP_NORMAL_LEFT */
    case 16: t = 9; angle = 90; break;
    case 17: t = 9; angle = -135; break;       /* ON_RAMP_SHARP_LEFT */
    case 18: t = 9; angle = 135; break;
    case 21: t = 8; angle = -45; break;        /* OFF_RAMP_SLIGHT_LEFT */
    case 22: t = 8; angle = 45; break;
    case 23: t = 8; angle = -90; break;        /* OFF_RAMP_NORMAL_LEFT */
    case 24: t = 8; angle = 90; break;
    case 30: case 43: case 45: t = 6; break;   /* ROUNDABOUT_ENTER */
    case 31: case 44: case 46: t = 7; break;   /* ROUNDABOUT_EXIT */
    case 32: case 33: case 34: case 35:        /* ROUNDABOUT_ENTER_AND_EXIT (CW, CCW, angle) */
        t = 27 + (exit_number >= 1 && exit_number <= 19 ? exit_number : 1);
        break;
    case 36: t = 3; break;                     /* STRAIGHT */
    case 37: case 38: case 47: case 48: case 49: case 50: t = 15; break; /* ferries */
    case 39: case 40: t = 10; break;           /* DESTINATION, _STRAIGHT */
    case 41: t = 24; break;                    /* DESTINATION_LEFT */
    case 42: t = 25; break;                    /* DESTINATION_RIGHT */
    default: t = 0; break;                     /* UNKNOWN: follow road */
    }
    if (angle_out) {
        *angle_out = angle;
    }
    return t;
}

/* Legacy NextTurnEnum + side to the iAP2 type. */
static int legacy_iap2_type(int event, int side, int exit_number, int *angle_out)
{
    int left = side == 1, t, angle = 1000;

    switch (event) {
    case 1: t = 11; break;
    case 2: t = 5; break;
    case 3: t = left ? 49 : 50; break;
    case 4: t = left ? 1 : 2; break;
    case 5: t = left ? 47 : 48; break;
    case 6: t = 4; angle = left ? -180 : 180; break;
    case 7: t = 9; angle = left ? -90 : 90; break;
    case 8: t = 8; angle = left ? -90 : 90; break;
    case 9: case 10: t = left ? 13 : 14; break;
    case 11: t = 6; break;
    case 12: t = 7; break;
    case 13: t = 27 + (exit_number >= 1 && exit_number <= 19 ? exit_number : 1); break;
    case 14: t = 3; break;
    case 15: case 16: t = 15; break;
    case 17: t = 10; break;
    default: t = 0; break;
    }
    *angle_out = angle;
    return t;
}

/* Exit angle of a roundabout as iAP2 sees it: the exit's direction relative to the car's
 * heading, right positive. AA measures the exit along the roundabout's traversal direction
 * (counter-clockwise where traffic keeps right): 90 = first exit, 180 = straight across. */
static int roundabout_angle(int type, int exit_angle)
{
    int clockwise = type == 32 || type == 33;

    if (!exit_angle) {
        return 1000;
    }
    return clockwise ? -(180 - exit_angle) : 180 - exit_angle;
}

/* LaneDirection shape to a signed iAP2-style degree angle; 1000 for unknown. */
static int lane_angle(unsigned shape)
{
    static const int angles[10] = { 1000, 0, -45, 45, -90, 90, -135, 135, -180, 180 };

    return shape < 10 ? angles[shape] : 1000;
}

static unsigned append(char *out, unsigned cap, unsigned used, const char *s)
{
    unsigned n = (unsigned)strlen(s);

    if (used + n + 1 > cap) {
        return cap;
    }
    memcpy(out + used, s, n + 1);
    return used + n;
}

static unsigned append_num(char *out, unsigned cap, unsigned used, const char *key, int v)
{
    char line[48];

    snprintf(line, sizeof(line), "%s=%d\n", key, v);
    return append(out, cap, used, line);
}

unsigned aa_nav_text(const struct aa_nav *n, char *out, unsigned cap)
{
    unsigned used = 0, i, j;
    int type, angle = 1000, active = n->status == 1 || n->status == 3;
    char line[64];

    if (cap < 16) {
        return 0;
    }
    if (!active || (n->type < 0 && n->legacy < 0)) {
        return append(out, cap, 0, "active=0\n");
    }
    if (n->type >= 0) {
        type = aa_nav_iap2_type(n->type, n->exit_number, &angle);
        if (type >= 28 && type <= 46) {
            angle = roundabout_angle(n->type, n->exit_angle);
        }
    } else {
        type = legacy_iap2_type(n->legacy, n->side, n->exit_number, &angle);
    }
    used = append(out, cap, used, "active=1\n");
    used = append_num(out, cap, used, "type", type);
    used = append_num(out, cap, used, "angle", angle);
    used = append(out, cap, used, "lht=0\nroads=\n");
    used = append_num(out, cap, used, "dist", n->distance);
    used = append(out, cap, used, "step=0\n");
    snprintf(line, sizeof(line), "street=%.48s\n", n->current[0] ? n->current : n->road);
    used = append(out, cap, used, line);
    used = append_num(out, cap, used, "rtt", n->dest_time);
    used = append_num(out, cap, used, "dest", n->dest_distance);
    used = append_num(out, cap, used, "lanes", (int)n->lane_count);
    for (i = 0; i < n->lane_count; i++) {
        unsigned k;

        k = (unsigned)snprintf(line, sizeof(line), "lane%u=%u,%d,%d", i, i,
                               n->lane_highlight[i] ? 1 : 0,
                               n->lane_highlight[i] ? lane_angle(n->lane_shape[i]) : 1000);
        for (j = 0; j < 4 && n->lane_shapes[i][j] != 10 && k < sizeof(line) - 8; j++) {
            k += (unsigned)snprintf(line + k, sizeof(line) - k, ",%d",
                                    lane_angle(n->lane_shapes[i][j]));
        }
        if (k < sizeof(line) - 2) {
            line[k++] = '\n';
            line[k] = 0;
        }
        used = append(out, cap, used, line);
    }
    return used >= cap ? 0 : used;
}

/* ---- H.264 SPS ------------------------------------------------------------------------ */

struct bits {
    const uint8_t *p;
    unsigned n, pos; /* bit position */
    int bad;
};

static unsigned bit(struct bits *b)
{
    unsigned v;

    if (b->pos >= b->n * 8) {
        b->bad = 1;
        return 0;
    }
    v = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1u;
    b->pos++;
    return v;
}

static unsigned bits_u(struct bits *b, unsigned count)
{
    unsigned v = 0;

    while (count--) {
        v = (v << 1) | bit(b);
    }
    return v;
}

static unsigned ue(struct bits *b)
{
    unsigned zeros = 0;

    while (!bit(b) && !b->bad && zeros < 32) {
        zeros++;
    }
    if (b->bad || zeros >= 32) {
        b->bad = 1;
        return 0;
    }
    return ((1u << zeros) - 1) + bits_u(b, zeros);
}

static int se(struct bits *b)
{
    unsigned k = ue(b);

    return (k & 1) ? (int)((k + 1) / 2) : -(int)(k / 2);
}

struct writer {
    uint8_t *p;
    unsigned cap, pos;
    int bad;
};

static void put_bit(struct writer *w, unsigned v)
{
    if (w->pos >= w->cap * 8) {
        w->bad = 1;
        return;
    }
    if (!(w->pos & 7)) {
        w->p[w->pos >> 3] = 0;
    }
    if (v) {
        w->p[w->pos >> 3] |= (uint8_t)(0x80u >> (w->pos & 7));
    }
    w->pos++;
}

static void put_ue(struct writer *w, unsigned v)
{
    unsigned x = v + 1, len = 0, i;

    while ((x >> len) > 1) {
        len++;
    }
    for (i = 0; i < len; i++) {
        put_bit(w, 0);
    }
    for (i = 0; i <= len; i++) {
        put_bit(w, (x >> (len - i)) & 1u);
    }
}

static void copy_bits(struct writer *w, const struct bits *b, unsigned from, unsigned to)
{
    struct bits r = *b;

    r.pos = from;
    while (r.pos < to && !w->bad) {
        put_bit(w, bit(&r));
    }
}

static void scaling_list(struct bits *b, unsigned size)
{
    int last = 8, next = 8;
    unsigned j;

    for (j = 0; j < size && !b->bad; j++) {
        if (next) {
            next = (last + se(b) + 256) % 256;
        }
        last = next ? next : last;
    }
}

struct sps {
    unsigned chroma, frame_mbs_only, width_mbs, height_units;
    unsigned crop_pos, after_crop_pos; /* bit positions in the RBSP */
    unsigned crop[4];
    int cropped;
};

/* Unescapes (removes emulation prevention bytes) the SPS payload after its header byte. */
static unsigned unescape(const uint8_t *in, unsigned n, uint8_t *out, unsigned cap)
{
    unsigned i, k = 0, zeros = 0;

    for (i = 1; i < n; i++) {
        if (zeros >= 2 && in[i] == 3) {
            zeros = 0;
            continue;
        }
        if (k >= cap) {
            return 0;
        }
        out[k++] = in[i];
        zeros = in[i] ? 0 : zeros + 1;
    }
    return k;
}

static int parse_sps(const uint8_t *rbsp, unsigned n, struct sps *s)
{
    struct bits b = { rbsp, n, 0, 0 };
    unsigned profile, i, poc_type, count;

    memset(s, 0, sizeof(*s));
    s->chroma = 1;
    profile = bits_u(&b, 8);
    bits_u(&b, 8); /* constraint flags */
    bits_u(&b, 8); /* level */
    ue(&b);        /* seq_parameter_set_id */
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
        profile == 139 || profile == 134 || profile == 135) {
        s->chroma = ue(&b);
        if (s->chroma == 3) {
            bit(&b); /* separate_colour_plane_flag */
        }
        ue(&b); /* bit_depth_luma_minus8 */
        ue(&b); /* bit_depth_chroma_minus8 */
        bit(&b); /* qpprime_y_zero_transform_bypass_flag */
        if (bit(&b)) { /* seq_scaling_matrix_present_flag */
            count = s->chroma != 3 ? 8 : 12;
            for (i = 0; i < count && !b.bad; i++) {
                if (bit(&b)) {
                    scaling_list(&b, i < 6 ? 16 : 64);
                }
            }
        }
    }
    ue(&b); /* log2_max_frame_num_minus4 */
    poc_type = ue(&b);
    if (poc_type == 0) {
        ue(&b);
    } else if (poc_type == 1) {
        bit(&b);
        se(&b);
        se(&b);
        count = ue(&b);
        if (count > 255) {
            return 0;
        }
        for (i = 0; i < count && !b.bad; i++) {
            se(&b);
        }
    }
    ue(&b);  /* max_num_ref_frames */
    bit(&b); /* gaps_in_frame_num_value_allowed_flag */
    s->width_mbs = ue(&b) + 1;
    s->height_units = ue(&b) + 1;
    s->frame_mbs_only = bit(&b);
    if (!s->frame_mbs_only) {
        bit(&b); /* mb_adaptive_frame_field_flag */
    }
    bit(&b); /* direct_8x8_inference_flag */
    s->crop_pos = b.pos;
    s->cropped = (int)bit(&b);
    if (s->cropped) {
        for (i = 0; i < 4; i++) {
            s->crop[i] = ue(&b);
        }
    }
    s->after_crop_pos = b.pos;
    return !b.bad && s->width_mbs < 1024 && s->height_units < 1024;
}

int aa_sps_size(const uint8_t *in, unsigned n, unsigned *w, unsigned *h)
{
    uint8_t rbsp[512];
    struct sps s;
    unsigned k;

    if (n < 4 || (in[0] & 0x1f) != 7) {
        return 0;
    }
    k = unescape(in, n, rbsp, sizeof(rbsp));
    if (!k || !parse_sps(rbsp, k, &s)) {
        return 0;
    }
    *w = s.width_mbs * 16;
    *h = (2 - s.frame_mbs_only) * s.height_units * 16;
    return 1;
}

unsigned aa_sps_crop(const uint8_t *in, unsigned n, uint8_t *out, unsigned cap, unsigned x,
                     unsigned y, unsigned w, unsigned h)
{
    uint8_t rbsp[512], rbsp_out[512];
    struct sps s;
    struct bits b;
    struct writer wr = { rbsp_out, sizeof(rbsp_out), 0, 0 };
    unsigned k, stop, coded_w, coded_h, unit_x, unit_y, i, zeros = 0, used = 0;

    if (n < 4 || (in[0] & 0x1f) != 7) {
        return 0;
    }
    k = unescape(in, n, rbsp, sizeof(rbsp));
    if (!k || !parse_sps(rbsp, k, &s)) {
        return 0;
    }
    coded_w = s.width_mbs * 16;
    coded_h = (2 - s.frame_mbs_only) * s.height_units * 16;
    unit_x = s.chroma == 1 || s.chroma == 2 ? 2 : 1;
    unit_y = (s.chroma == 1 ? 2 : 1) * (2 - s.frame_mbs_only);
    if (unit_y == 4) {
        return 0; /* field-coded 4:2:0: not what a phone sends; the shifts above assume 1 or 2 */
    }
    /* Crop units are 1 or 2: shifts, as the unit's libc has no __aeabi_uidiv for us. */
    if (x + w > coded_w || y + h > coded_h || !w || !h || (x & (unit_x - 1)) ||
        (y & (unit_y - 1)) || ((coded_w - x - w) & (unit_x - 1)) ||
        ((coded_h - y - h) & (unit_y - 1))) {
        return 0;
    }
    /* The RBSP ends with a stop bit: the last 1 in the buffer. */
    stop = k * 8;
    while (stop > s.after_crop_pos) {
        stop--;
        if ((rbsp[stop >> 3] >> (7 - (stop & 7))) & 1u) {
            break;
        }
    }
    if (stop < s.after_crop_pos) {
        return 0;
    }
    b.p = rbsp;
    b.n = k;
    b.pos = 0;
    b.bad = 0;
    copy_bits(&wr, &b, 0, s.crop_pos);
    put_bit(&wr, 1);
    put_ue(&wr, x >> (unit_x - 1));
    put_ue(&wr, (coded_w - x - w) >> (unit_x - 1));
    put_ue(&wr, y >> (unit_y - 1));
    put_ue(&wr, (coded_h - y - h) >> (unit_y - 1));
    copy_bits(&wr, &b, s.after_crop_pos, stop);
    put_bit(&wr, 1); /* rbsp_stop_one_bit */
    while (wr.pos & 7) {
        put_bit(&wr, 0);
    }
    if (wr.bad) {
        return 0;
    }
    /* Escape: a 0x00 0x00 followed by 0x00..0x03 gets an emulation prevention byte. */
    if (cap < 1) {
        return 0;
    }
    out[used++] = in[0];
    for (i = 0; i < wr.pos / 8; i++) {
        if (zeros >= 2 && rbsp_out[i] <= 3) {
            if (used >= cap) {
                return 0;
            }
            out[used++] = 3;
            zeros = 0;
        }
        if (used >= cap) {
            return 0;
        }
        out[used++] = rbsp_out[i];
        zeros = rbsp_out[i] ? 0 : zeros + 1;
    }
    return used;
}

/* ---- Annex-B -------------------------------------------------------------------------- */

/* Next NAL unit in an Annex-B stream from offset *pos: its payload start and length. */
static int next_nal(const uint8_t *p, unsigned n, unsigned *pos, unsigned *start,
                    unsigned *len)
{
    unsigned i = *pos, j;

    while (i + 3 <= n) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            break;
        }
        i++;
    }
    if (i + 3 > n) {
        return 0;
    }
    i += 3;
    *start = i;
    for (j = i; j + 3 <= n; j++) {
        if (p[j] == 0 && p[j + 1] == 0 && (p[j + 2] == 1 || (p[j + 2] == 0 && j + 3 < n &&
                                                              p[j + 3] == 1))) {
            break;
        }
    }
    if (j + 3 > n) {
        j = n;
    }
    /* Trailing zero bytes belong to the next start code. */
    while (j > i && p[j - 1] == 0) {
        j--;
    }
    *len = j - i;
    *pos = j;
    return 1;
}

unsigned aa_nal_types(const uint8_t *p, unsigned n)
{
    unsigned pos = 0, start, len, mask = 0;

    while (next_nal(p, n, &pos, &start, &len)) {
        if (len) {
            mask |= 1u << (p[start] & 0x1f);
        }
    }
    return mask;
}

static unsigned put_nal(uint8_t *out, unsigned cap, unsigned used, const uint8_t *p,
                        unsigned len)
{
    if (used + 4 + len > cap) {
        return 0;
    }
    out[used] = 0;
    out[used + 1] = 0;
    out[used + 2] = 0;
    out[used + 3] = 1;
    memcpy(out + used + 4, p, len);
    return used + 4 + len;
}

unsigned aa_annexb_parameter_sets(const uint8_t *in, unsigned n, uint8_t *ps, unsigned cap,
                                  int crop)
{
    unsigned pos = 0, start, len, used = 0, type;
    uint8_t sps[512];

    while (next_nal(in, n, &pos, &start, &len)) {
        if (!len) {
            continue;
        }
        type = in[start] & 0x1f;
        if (type == 7) {
            unsigned k = crop ? aa_sps_crop(in + start, len, sps, sizeof(sps), AA_CROP_X,
                                            AA_CROP_Y, AA_VIEW_W, AA_VIEW_H)
                              : 0;

            used = k ? put_nal(ps, cap, used, sps, k) : put_nal(ps, cap, used, in + start, len);
        } else if (type == 8) {
            used = put_nal(ps, cap, used, in + start, len);
        }
        if (!used && (type == 7 || type == 8)) {
            return 0;
        }
    }
    return used;
}

unsigned aa_annexb_rewrite(const uint8_t *in, unsigned n, uint8_t *out, unsigned cap,
                           const uint8_t *ps, unsigned ps_len, int crop, int *idr)
{
    unsigned pos = 0, start, len, used = 0, type, mask = aa_nal_types(in, n);
    uint8_t sps[512];

    *idr = (mask & AA_NAL_IDR) != 0;
    if (*idr && !(mask & AA_NAL_SPS) && ps_len) {
        if (ps_len > cap) {
            return 0;
        }
        memcpy(out, ps, ps_len);
        used = ps_len;
    }
    while (next_nal(in, n, &pos, &start, &len)) {
        if (!len) {
            continue;
        }
        type = in[start] & 0x1f;
        if (type == 7 && crop) {
            unsigned k = aa_sps_crop(in + start, len, sps, sizeof(sps), AA_CROP_X, AA_CROP_Y,
                                     AA_VIEW_W, AA_VIEW_H);

            used = k ? put_nal(out, cap, used, sps, k) : put_nal(out, cap, used, in + start, len);
        } else {
            used = put_nal(out, cap, used, in + start, len);
        }
        if (!used) {
            return 0;
        }
    }
    return used;
}

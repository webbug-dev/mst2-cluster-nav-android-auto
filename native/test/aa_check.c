/*
 * mst2-cluster-nav - host check of the Android Auto wire formats (src/sal/aa_proto.c).
 * Android Auto branch. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * aa-check               runs the protocol checks
 * aa-check sps IN OUT    rewrites every SPS of the Annex-B file IN (crop to the cluster
 *                        viewport) into OUT, for ffprobe to confirm the picture size
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcn_aa.h"

#define CHECK(x)                                 \
    do {                                         \
        if (!(x)) {                              \
            printf("aa check failed: %s\n", #x); \
            abort();                             \
        }                                        \
    } while (0)

/* A Service with one field (number `field`) holding `body`. */
static unsigned service(uint8_t *out, unsigned id, unsigned field, const uint8_t *body,
                        unsigned n)
{
    uint8_t s[256];
    unsigned k = aa_pb_put_varint(s, sizeof(s), 1, id);

    k += aa_pb_put_bytes(s + k, sizeof(s) - k, field, body, n);
    return aa_pb_put_bytes(out, 1024, 1, s, k);
}

struct found {
    unsigned id, has_media, media_type, display_id, display_type, has_input, input_display,
        has_nav, nav_type, video_res, video_fps, margin_w, margin_h, has_ui;
};

static void find_services(const uint8_t *p, unsigned n, struct found *f, unsigned *count)
{
    struct aa_pb b = { p, p + n }, s, x, y, z;
    unsigned fld, wire, f2, w2, f3, w3, f4, w4;
    uint64_t v, v2, v3, v4;

    *count = 0;
    while (aa_pb_next(&b, &fld, &wire, &v, &s)) {
        struct found *o;

        if (fld != 1 || wire != 2) {
            continue;
        }
        o = &f[(*count)++];
        memset(o, 0, sizeof(*o));
        o->display_id = o->display_type = o->input_display = 99;
        while (aa_pb_next(&s, &f2, &w2, &v2, &x)) {
            if (f2 == 1 && w2 == 0) {
                o->id = (unsigned)v2;
            } else if (f2 == 3 && w2 == 2) {
                o->has_media = 1;
                while (aa_pb_next(&x, &f3, &w3, &v3, &y)) {
                    if (f3 == 1 && w3 == 0) o->media_type = (unsigned)v3;
                    if (f3 == 6 && w3 == 0) o->display_id = (unsigned)v3;
                    if (f3 == 7 && w3 == 0) o->display_type = (unsigned)v3;
                    if (f3 == 4 && w3 == 2) {
                        while (aa_pb_next(&y, &f4, &w4, &v4, &z)) {
                            if (f4 == 1 && w4 == 0) o->video_res = (unsigned)v4;
                            if (f4 == 2 && w4 == 0) o->video_fps = (unsigned)v4;
                            if (f4 == 3 && w4 == 0) o->margin_w = (unsigned)v4;
                            if (f4 == 4 && w4 == 0) o->margin_h = (unsigned)v4;
                            if (f4 == 11 && w4 == 2) o->has_ui = 1;
                        }
                    }
                }
            } else if (f2 == 4 && w2 == 2) {
                o->has_input = 1;
                while (aa_pb_next(&x, &f3, &w3, &v3, &y)) {
                    if (f3 == 5 && w3 == 0) o->input_display = (unsigned)v3;
                }
            } else if (f2 == 8 && w2 == 2) {
                o->has_nav = 1;
                while (aa_pb_next(&x, &f3, &w3, &v3, &y)) {
                    if (f3 == 2 && w3 == 0) o->nav_type = (unsigned)v3;
                }
            }
        }
    }
}

static void check_discovery(void)
{
    uint8_t video_config[16], sink[64], audio_sink[16], input[16], in[1024], out[2048];
    unsigned k, n = 0, m, count;
    struct found f[8];
    struct aa_services s = { 0x40, 0x41, 0x42, AA_RES_1280x720, 0, 110, 1, 1 };
    static const uint8_t name[] = "Amundsen";

    /* Stock: video sink (res 1, fps enum 2), an audio sink, an input source, a name. */
    k = aa_pb_put_varint(video_config, sizeof(video_config), 1, 1);
    k += aa_pb_put_varint(video_config + k, sizeof(video_config) - k, 2, 2);
    m = aa_pb_put_varint(sink, sizeof(sink), 1, 3);
    m += aa_pb_put_bytes(sink + m, sizeof(sink) - m, 4, video_config, k);
    n += service(in + n, 1, 3, sink, m);
    m = aa_pb_put_varint(audio_sink, sizeof(audio_sink), 1, 1);
    m += aa_pb_put_varint(audio_sink + m, sizeof(audio_sink) - m, 2, 1);
    n += service(in + n, 2, 3, audio_sink, m);
    m = aa_pb_put_bytes(input, sizeof(input), 1, (const uint8_t *)"\x17", 1);
    n += service(in + n, 3, 4, input, m);
    n += aa_pb_put_bytes(in + n, sizeof(in) - n, 2, name, 8);
    n += aa_pb_put_varint(in + n, sizeof(in) - n, 6, 1);

    k = aa_discovery_rewrite(in, n, out, sizeof(out), &s);
    CHECK(k > n);
    find_services(out, k, f, &count);
    CHECK(count == 6);
    CHECK(f[0].id == 1 && f[0].has_media && f[0].media_type == 3 && f[0].display_id == 0 &&
          f[0].display_type == 0 && f[0].video_res == 1 && f[0].video_fps == 2);
    CHECK(f[1].id == 2 && f[1].has_media && f[1].media_type == 1 && f[1].display_id == 99);
    CHECK(f[2].id == 3 && f[2].has_input && f[2].input_display == 0);
    CHECK(f[3].id == 0x40 && f[3].has_media && f[3].media_type == 3 && f[3].display_id == 1 &&
          f[3].display_type == 1 && f[3].video_res == 2 && f[3].video_fps == 2 &&
          f[3].margin_w == 270 && f[3].margin_h == 344 && f[3].has_ui);
    CHECK(f[4].id == 0x41 && f[4].has_input && f[4].input_display == 1);
    CHECK(f[5].id == 0x42 && f[5].has_nav && f[5].nav_type == 2);
    /* The other top-level fields survive, in place. */
    CHECK(memmem(out, k, "\x30\x01", 2) != NULL);
    CHECK(memmem(out, k, "Amundsen", 8) != NULL);

    /* Arrows only: no cluster display, only the navigation service added. */
    s.cluster = 0;
    k = aa_discovery_rewrite(in, n, out, sizeof(out), &s);
    find_services(out, k, f, &count);
    CHECK(count == 4 && f[0].display_id == 99 && f[2].input_display == 99 && f[3].has_nav);
    /* 800x480 fallback: no margins. */
    s.cluster = 1;
    s.res = AA_RES_800x480;
    k = aa_discovery_rewrite(in, n, out, sizeof(out), &s);
    find_services(out, k, f, &count);
    CHECK(count == 6 && f[3].video_res == 1 && f[3].margin_w == 0 && f[3].margin_h == 0);
    /* Malformed input: nothing. */
    CHECK(aa_discovery_rewrite(in, 3, out, sizeof(out), &s) == 0);
    CHECK(aa_discovery_rewrite(in, n, out, 40, &s) == 0);
    puts("aa discovery: ok");
}

static void check_messages(void)
{
    uint8_t m[32];
    int session;
    unsigned config;
    static const uint8_t setup[] = { 0x08, 0x03 };
    static const uint8_t start[] = { 0x08, 0x07, 0x10, 0x00 };
    static const uint8_t focus[] = { 0x08, 0x01, 0x10, 0x02, 0x18, 0x01 };

    CHECK(aa_msg_setup_response(m, sizeof(m), 2, 1) == 8 &&
          !memcmp(m, "\x80\x03\x08\x02\x10\x01\x18\x00", 8));
    CHECK(aa_msg_ack(m, sizeof(m), 7, 1) == 6 && !memcmp(m, "\x80\x04\x08\x07\x10\x01", 6));
    CHECK(aa_msg_ack(m, sizeof(m), -1, 1) == 15); /* negative session as a 10-byte varint */
    CHECK(aa_msg_video_focus(m, sizeof(m), 2, 1) == 6 &&
          !memcmp(m, "\x80\x08\x08\x02\x10\x01", 6));
    CHECK(aa_msg_version_request(m, sizeof(m), 1, 7) == 6 &&
          !memcmp(m, "\x00\x01\x00\x01\x00\x07", 6));
    CHECK(aa_msg_key_binding_response(m, sizeof(m)) == 4 && !memcmp(m, "\x80\x03\x08\x00", 4));
    CHECK(aa_msg_nav_start(m, sizeof(m)) == 2 && !memcmp(m, "\x80\x01", 2));
    CHECK(aa_msg_ack(m, 3, 7, 1) == 2); /* truncated: the id only; the caller checks */
    CHECK(aa_parse_setup_request(setup, sizeof(setup)) == 3);
    CHECK(aa_parse_setup_request(setup, 1) == -1);
    CHECK(aa_parse_start(start, sizeof(start), &session, &config) && session == 7 && !config);
    CHECK(aa_parse_focus_request(focus, sizeof(focus)) == 2);
    puts("aa messages: ok");
}

static void check_nav(void)
{
    struct aa_nav n;
    char out[1024];
    uint8_t state[256], step[128], man[16], road[32], names[32], lane[32], dir[16], dirs[32];
    uint8_t pos[128], sd[64], dist[16], dd[64], cur[32];
    unsigned k, m, j;
    int angle;

    aa_nav_reset(&n);
    CHECK(aa_nav_text(&n, out, sizeof(out)) == 9 && !strcmp(out, "active=0\n"));

    /* NavigationState: one step, TURN_NORMAL_RIGHT onto "Main St", two lanes. */
    k = aa_pb_put_varint(man, sizeof(man), 1, 8);
    m = aa_pb_put_bytes(step, sizeof(step), 1, man, k);
    k = aa_pb_put_bytes(names, sizeof(names), 1, (const uint8_t *)"Main St", 7);
    m += aa_pb_put_bytes(step + m, sizeof(step) - m, 4, names, k);
    /* lane 0: straight (not highlighted); lane 1: straight + normal right, right highlighted */
    k = aa_pb_put_varint(dir, sizeof(dir), 1, 1);
    j = aa_pb_put_bytes(lane, sizeof(lane), 1, dir, k);
    m += aa_pb_put_bytes(step + m, sizeof(step) - m, 3, lane, j);
    k = aa_pb_put_varint(dir, sizeof(dir), 1, 1);
    j = aa_pb_put_bytes(dirs, sizeof(dirs), 1, dir, k);
    k = aa_pb_put_varint(dir, sizeof(dir), 1, 5);
    k += aa_pb_put_varint(dir + k, sizeof(dir) - k, 2, 1);
    j += aa_pb_put_bytes(dirs + j, sizeof(dirs) - j, 1, dir, k);
    m += aa_pb_put_bytes(step + m, sizeof(step) - m, 3, dirs, j);
    k = aa_pb_put_bytes(state, sizeof(state), 1, step, m);
    CHECK(aa_nav_message(&n, AA_MSG_NAV_STATE, state, k));
    CHECK(n.type == 8 && !strcmp(n.road, "Main St") && n.lane_count == 2 && n.status == 1);
    CHECK(n.lane_highlight[1] && n.lane_shape[1] == 5 && !n.lane_highlight[0]);

    /* NavigationCurrentPosition: 250 m / 30 s to the step, 12 km / 900 s to go, on "Elm". */
    k = aa_pb_put_varint(dist, sizeof(dist), 1, 250);
    m = aa_pb_put_bytes(sd, sizeof(sd), 1, dist, k);
    m += aa_pb_put_varint(sd + m, sizeof(sd) - m, 2, 30);
    j = aa_pb_put_bytes(pos, sizeof(pos), 1, sd, m);
    k = aa_pb_put_varint(dist, sizeof(dist), 1, 12000);
    m = aa_pb_put_bytes(dd, sizeof(dd), 1, dist, k);
    m += aa_pb_put_varint(dd + m, sizeof(dd) - m, 3, 900);
    j += aa_pb_put_bytes(pos + j, sizeof(pos) - j, 2, dd, m);
    k = aa_pb_put_bytes(cur, sizeof(cur), 1, (const uint8_t *)"Elm", 3);
    j += aa_pb_put_bytes(pos + j, sizeof(pos) - j, 3, cur, k);
    CHECK(aa_nav_message(&n, AA_MSG_NAV_POSITION, pos, j));
    CHECK(n.distance == 250 && n.time == 30 && n.dest_distance == 12000 && n.dest_time == 900);
    CHECK(aa_nav_text(&n, out, sizeof(out)) > 0);
    CHECK(!strcmp(out, "active=1\ntype=2\nangle=1000\nlht=0\nroads=\ndist=250\nstep=0\n"
                       "street=Elm\nrtt=900\ndest=12000\nlanes=2\n"
                       "lane0=0,0,1000,0\nlane1=1,1,90,0,90\n"));

    /* Status 2 (inactive) ends guidance; 3 (rerouting) keeps it. */
    CHECK(aa_nav_message(&n, AA_MSG_NAV_STATUS, (const uint8_t *)"\x08\x03", 2) && n.status == 3);
    CHECK(aa_nav_text(&n, out, sizeof(out)) && !strncmp(out, "active=1\n", 9));
    CHECK(aa_nav_message(&n, AA_MSG_NAV_STATUS, (const uint8_t *)"\x08\x02", 2) && n.status == 2);
    CHECK(aa_nav_text(&n, out, sizeof(out)) == 9 && n.dest_distance == -1);

    /* Roundabout, 3rd exit, counter-clockwise with an angle of 270 (left). */
    aa_nav_reset(&n);
    k = aa_pb_put_varint(man, sizeof(man), 1, 35);
    k += aa_pb_put_varint(man + k, sizeof(man) - k, 2, 3);
    k += aa_pb_put_varint(man + k, sizeof(man) - k, 3, 270);
    m = aa_pb_put_bytes(step, sizeof(step), 1, man, k);
    k = aa_pb_put_bytes(state, sizeof(state), 1, step, m);
    CHECK(aa_nav_message(&n, AA_MSG_NAV_STATE, state, k) && n.exit_number == 3);
    CHECK(aa_nav_text(&n, out, sizeof(out)) &&
          !strncmp(out, "active=1\ntype=30\nangle=-90\n", 27));
    CHECK(aa_nav_iap2_type(12, 0, &angle) == 4 && angle == 180);
    CHECK(aa_nav_iap2_type(23, 0, &angle) == 8 && angle == -90);
    CHECK(aa_nav_iap2_type(41, 0, &angle) == 24);
    CHECK(aa_nav_iap2_type(99, 0, &angle) == 0);

    /* Legacy pair (a 1.x phone): TURN left onto "Oak", 120 m. */
    aa_nav_reset(&n);
    k = aa_pb_put_bytes(road, sizeof(road), 1, (const uint8_t *)"Oak", 3);
    k += aa_pb_put_varint(road + k, sizeof(road) - k, 2, 1);
    k += aa_pb_put_varint(road + k, sizeof(road) - k, 3, 4);
    CHECK(aa_nav_message(&n, AA_MSG_NAV_NEXT_TURN, road, k) && n.legacy == 4 && n.side == 1);
    CHECK(aa_nav_message(&n, AA_MSG_NAV_NEXT_TURN_DISTANCE, (const uint8_t *)"\x08\x78", 2));
    CHECK(aa_nav_text(&n, out, sizeof(out)) &&
          !strcmp(out, "active=1\ntype=1\nangle=1000\nlht=0\nroads=\ndist=120\nstep=0\n"
                       "street=Oak\nrtt=-1\ndest=-1\nlanes=0\n"));
    /* Garbage never changes anything. */
    CHECK(!aa_nav_message(&n, AA_MSG_NAV_STATE, (const uint8_t *)"\x0a\xff", 2));
    puts("aa navigation: ok");
}

static void check_annexb(void)
{
    /* SPS (3 bytes), PPS, IDR, then a P slice: start codes of 3 and 4 bytes. */
    static const uint8_t au[] = { 0, 0, 0, 1, 0x67, 0xaa, 0xbb, 0, 0, 1, 0x68, 0xcc,
                                  0, 0, 0, 1, 0x65, 0x11, 0x00 };
    static const uint8_t p[] = { 0, 0, 1, 0x41, 0x22 };
    uint8_t ps[64], out[64];
    int idr;
    unsigned n;

    CHECK(aa_nal_types(au, sizeof(au)) == (AA_NAL_SPS | AA_NAL_PPS | AA_NAL_IDR));
    CHECK(aa_nal_types(p, sizeof(p)) == AA_NAL_SLICE);
    n = aa_annexb_parameter_sets(au, sizeof(au), ps, sizeof(ps), 0);
    CHECK(n == 13 && !memcmp(ps, "\0\0\0\1\x67\xaa\xbb\0\0\0\1\x68\xcc", 13));
    n = aa_annexb_rewrite(au, sizeof(au), out, sizeof(out), ps, 13, 0, &idr);
    CHECK(idr && n == 19 && !memcmp(out + 13, "\0\0\0\1\x65\x11", 6)); /* zero dropped */
    n = aa_annexb_rewrite(p, sizeof(p), out, sizeof(out), ps, 13, 0, &idr);
    CHECK(!idr && n == 6 && !memcmp(out, "\0\0\0\1\x41\x22", 6));
    /* An IDR without parameter sets gets them in front. */
    n = aa_annexb_rewrite(au + 12, sizeof(au) - 12, out, sizeof(out), ps, 13, 0, &idr);
    CHECK(idr && n == 19 && !memcmp(out, ps, 13));
    CHECK(aa_annexb_rewrite(au, sizeof(au), out, 10, ps, 13, 0, &idr) == 0);
    puts("aa annexb: ok");
}

/* An SPS as x264 writes it for 1280x720 High 3.1 (no cropping), and one for 1280x736 with
 * cropping to 720; both must come back as 1010x376 at (135, 172). */
static void check_sps(void)
{
    static const uint8_t sps720[] = { 0x67, 0x64, 0x00, 0x1f, 0xac, 0xd9, 0x40, 0x50, 0x05,
                                      0xbb, 0x01, 0x6a, 0x02, 0x02, 0x02, 0x80, 0x00, 0x00,
                                      0x03, 0x00, 0x80, 0x00, 0x00, 0x1e, 0x47, 0x8c, 0x18,
                                      0xcb };
    uint8_t out[128];
    unsigned n, w, h, w2, h2;

    CHECK(aa_sps_size(sps720, sizeof(sps720), &w, &h) && w == 1280 && h == 720);
    n = aa_sps_crop(sps720, sizeof(sps720), out, sizeof(out), AA_CROP_X, AA_CROP_Y, AA_VIEW_W,
                    AA_VIEW_H);
    CHECK(n > 0 && n < sizeof(out));
    CHECK(aa_sps_size(out, n, &w2, &h2) && w2 == 1280 && h2 == 720);
    /* Odd offsets are not representable in 4:2:0 crop units. */
    CHECK(aa_sps_crop(sps720, sizeof(sps720), out, sizeof(out), 135, 172, 1010, 376) == 0);
    CHECK(aa_sps_crop(sps720, sizeof(sps720), out, sizeof(out), 0, 0, 1300, 376) == 0);
    CHECK(aa_sps_crop(sps720, 5, out, sizeof(out), 0, 0, 16, 16) == 0);
    puts("aa sps: ok (ffprobe check in aa_sps_check.sh)");
}

static int sps_file(const char *in_path, const char *out_path)
{
    FILE *f = fopen(in_path, "rb");
    uint8_t *in, *out, ps[1024];
    long n;
    unsigned k, m, pos = 0, used = 0;
    int idr, ok;

    if (!f) {
        return 1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    in = malloc((size_t)n);
    out = malloc((size_t)n * 2 + 4096);
    ok = fread(in, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) {
        return 1;
    }
    /* The whole file as one access unit: every SPS in it is rewritten. */
    m = aa_annexb_parameter_sets(in, (unsigned)n, ps, sizeof(ps), 1);
    k = aa_annexb_rewrite(in, (unsigned)n, out, (unsigned)n * 2 + 4096, ps, m, 1, &idr);
    (void)pos;
    (void)used;
    if (!k) {
        return 2;
    }
    f = fopen(out_path, "wb");
    if (!f) {
        return 1;
    }
    fwrite(out, 1, k, f);
    fclose(f);
    free(in);
    free(out);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "sps")) {
        return sps_file(argv[2], argv[3]);
    }
    check_discovery();
    check_messages();
    check_nav();
    check_annexb();
    check_sps();
    return 0;
}

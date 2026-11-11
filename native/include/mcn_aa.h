/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Android Auto branch. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * Android Auto protocol pieces that do not touch the unit: protobuf-lite readers and writers,
 * the service-discovery rewrite, the media-sink messages, the navigation messages as a
 * PhoneLink snapshot, an H.264 SPS cropping rewrite and an Annex-B access-unit scan. Built for
 * the unit (sal/aa_proto.c into libmcn_sal.so) and for the host checks (test/aa_check.c).
 * Wire schema: the public Android Auto protocol descriptions (openauto/aasdk, aa-proxy-rs
 * protos); nothing here depends on the receiver library's object layouts.
 */
#ifndef MCN_AA_H
#define MCN_AA_H

#ifdef MCN_HOST
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#else
#include "mcn_qnx.h"
#endif

/* Control channel (0) message ids. */
#define AA_MSG_VERSION_REQUEST 0x0001
#define AA_MSG_VERSION_RESPONSE 0x0002
#define AA_MSG_SERVICE_DISCOVERY_RESPONSE 0x0006
#define AA_MSG_CHANNEL_OPEN_REQUEST 0x0007
/* Media channel message ids. */
#define AA_MSG_MEDIA_WITH_TIMESTAMP 0x0000
#define AA_MSG_MEDIA 0x0001
#define AA_MSG_SETUP_REQUEST 0x8000
#define AA_MSG_START 0x8001
#define AA_MSG_STOP 0x8002
#define AA_MSG_SETUP_RESPONSE 0x8003
#define AA_MSG_ACK 0x8004
#define AA_MSG_VIDEO_FOCUS_REQUEST 0x8007
#define AA_MSG_VIDEO_FOCUS 0x8008
/* Navigation status channel message ids. */
#define AA_MSG_NAV_START 0x8001
#define AA_MSG_NAV_STOP 0x8002
#define AA_MSG_NAV_STATUS 0x8003
#define AA_MSG_NAV_NEXT_TURN 0x8004
#define AA_MSG_NAV_NEXT_TURN_DISTANCE 0x8005
#define AA_MSG_NAV_STATE 0x8006
#define AA_MSG_NAV_POSITION 0x8007
/* Input channel. */
#define AA_MSG_KEY_BINDING_REQUEST 0x8002
#define AA_MSG_KEY_BINDING_RESPONSE 0x8003

#define AA_CODEC_H264 3
#define AA_RES_800x480 1
#define AA_RES_1280x720 2
#define AA_RES_1920x1080 3
#define AA_FOCUS_PROJECTED 1
#define AA_FOCUS_NATIVE 2
#define AA_DISPLAY_CLUSTER 1

/* The cluster stream the phone is asked for: 1280x720 with margins, so the phone lays its map
 * and cards out in the centred 1010x376 viewport, which the SPS rewrite crops the picture to
 * (the cluster's window, as the stock stream: sal.h MCN_CLUSTER_WIDTH/HEIGHT). "480" mode asks
 * for 800x480 without margins or cropping: the cluster stretches it (fallback, GEM). */
#define AA_STREAM_W 1280
#define AA_STREAM_H 720
#define AA_VIEW_W 1010
#define AA_VIEW_H 376
#define AA_VIEW_X ((AA_STREAM_W - AA_VIEW_W) / 2)
#define AA_VIEW_Y ((AA_STREAM_H - AA_VIEW_H) / 2)
/* The picture is cropped to the viewport; H.264 crops 4:2:0 in steps of 2, so the odd
 * x origin of the centred viewport rounds down by one pixel. */
#define AA_CROP_X (AA_VIEW_X & ~1)
#define AA_CROP_Y (AA_VIEW_Y & ~1)
/* Decode mode shows the centred 8:3 band of the 800x480 stream: 800x300 from row 90
 * (decode.c view_of, VIEW_DY 0). The content insets sent to the phone are laid out in it. */
#define AA_DEC_VIEW_Y 90
#define AA_DEC_VIEW_H 300
/*
 * Decode mode layout: the content insets the phone lays its map out in (pixels of the 800x480
 * picture) and VIEW_DY. Measured on the car (photos of 2026-11-11, wide cluster view): the
 * shown band maps to the cluster as frame row = (picture row - 90 - dy) * 1.28; the status
 * line covers frame rows 0..56, the bottom bar starts at frame row ~325, with the two carbon
 * corners rising above it at the sides. Google Maps puts its maneuver card on the right of the
 * content area, anchored to its bottom edge (card bottom ~74 px above it), draws its logo
 * 17 px above that edge and centres the car in the area left of the card (card left edge at
 * x ~560 with right inset 10). So: bottom edge at row 352 (card fully visible between the
 * status line and the right carbon corner, logo hidden under the bottom bar), right inset 10
 * (card at the cluster's right edge), left inset 240 (car centred: (240 + 560) / 2 = 400).
 * A card file mst2-cluster-nav-layout.txt (or MCN_DIR/layout) overrides them at the next
 * phone connection: "top bottom left right dy", e.g. "83 128 240 10 0".
 */
#define AA_DEC_INSET_TOP 83
#define AA_DEC_INSET_BOTTOM 128
#define AA_DEC_INSET_LEFT 240
#define AA_DEC_INSET_RIGHT 10
#define AA_DEC_VIEW_DY 0

/* ---- protobuf-lite reader (bounded by the buffer end) and writer ---------------------- */

struct aa_pb {
    const uint8_t *p, *e;
};

int aa_pb_next(struct aa_pb *b, unsigned *field, unsigned *wire, uint64_t *value,
               struct aa_pb *sub);
unsigned aa_pb_varint(uint8_t *out, uint64_t v);
unsigned aa_pb_varint_size(uint64_t v);
/* tag + varint; tag + length-delimited copy. Return bytes written, 0 if they do not fit. */
unsigned aa_pb_put_varint(uint8_t *out, unsigned cap, unsigned field, uint64_t v);
unsigned aa_pb_put_bytes(uint8_t *out, unsigned cap, unsigned field, const uint8_t *p,
                         unsigned n);

/* ---- service discovery --------------------------------------------------------------- */

struct aa_services {
    unsigned video, input, nav; /* service ids chosen for the cluster */
    unsigned res;               /* AA_RES_1280x720 or AA_RES_800x480 */
    unsigned fps;               /* as the main screen's configuration carries it */
    unsigned dpi;
    int cluster;                /* offer the cluster display at all */
    int arrows;                 /* offer the navigation status service */
    int insets_set;             /* 800x480: use inset[] (top, bottom, left, right) */
    unsigned inset[4];
};

/*
 * Rewrites a ServiceDiscoveryResponse (the payload after the 2-byte id): every stock media
 * sink gets display_id 0 / display_type MAIN and every stock input source display_id 0, and
 * the cluster's video sink, its input source and the navigation status service are appended.
 * Returns the new length, 0 if the input is malformed or the output does not fit.
 */
unsigned aa_discovery_rewrite(const uint8_t *in, unsigned n, uint8_t *out, unsigned cap,
                              const struct aa_services *s);

/* ---- media sink messages (payloads include the 2-byte id) ----------------------------- */

unsigned aa_msg_setup_response(uint8_t *out, unsigned cap, unsigned status,
                               unsigned max_unacked);
unsigned aa_msg_ack(uint8_t *out, unsigned cap, int session, unsigned count);
unsigned aa_msg_video_focus(uint8_t *out, unsigned cap, unsigned mode, int unsolicited);
unsigned aa_msg_version_request(uint8_t *out, unsigned cap, unsigned major, unsigned minor);
unsigned aa_msg_key_binding_response(uint8_t *out, unsigned cap);
unsigned aa_msg_nav_start(uint8_t *out, unsigned cap);
/* MediaSetupRequest { type = 1 }; -1 if absent. */
int aa_parse_setup_request(const uint8_t *p, unsigned n);
/* Start { session_id = 1; config = 2 }: fills both, returns 1 if a session was present. */
int aa_parse_start(const uint8_t *p, unsigned n, int *session, unsigned *config);
/* VideoFocusRequest { disp_channel_id = 1; mode = 2; reason = 3 }: mode, 0 if absent. */
unsigned aa_parse_focus_request(const uint8_t *p, unsigned n);

/* ---- navigation: AA messages to the PhoneLink snapshot ------------------------------- */

#define AA_NAV_TEXT 128
#define AA_NAV_LANES 16

struct aa_nav {
    int status;      /* 0 unavailable, 1 active, 2 inactive, 3 rerouting; -1 unknown */
    int type;        /* AA maneuver type (new numbering), -1 none */
    int legacy;      /* legacy NextTurnEnum when the 1.x pair came instead, -1 none */
    int side;        /* legacy turn side 1 left 2 right 3 unspecified */
    int exit_number; /* roundabout exit, 0 unknown */
    int exit_angle;  /* roundabout exit angle (degrees, 0 unknown) */
    int distance;    /* metres to the maneuver, -1 unknown */
    int time;        /* seconds to the maneuver, -1 unknown */
    int dest_distance, dest_time;
    char road[AA_NAV_TEXT];    /* the step's road (or the instruction text) */
    char current[AA_NAV_TEXT]; /* the road the car is on (0x8007) */
    unsigned lane_count;
    uint8_t lane_shape[AA_NAV_LANES]; /* preferred direction shape 0..9 */
    uint8_t lane_highlight[AA_NAV_LANES];
    unsigned char lane_shapes[AA_NAV_LANES][4]; /* every direction of the lane, 10 = none */
    unsigned updated;                           /* bumped on every parsed message */
};

void aa_nav_reset(struct aa_nav *n);
/* One navigation channel message (payload after the id). Returns 1 if something changed. */
int aa_nav_message(struct aa_nav *n, unsigned id, const uint8_t *p, unsigned len);
/* AA maneuver type (+ exit number) to the iAP2 type the HMI bundle maps (Maneuvers.java). */
int aa_nav_iap2_type(int type, int exit_number, int *angle_out);
/* The PhoneLink datagram (PhoneLink.java): "active=1\ntype=..\n...". Returns its length. */
unsigned aa_nav_text(const struct aa_nav *n, char *out, unsigned cap);

/* ---- H.264 ----------------------------------------------------------------------------- */

/*
 * Rewrites one SPS NAL unit (with its 0x67 header, no start code) so that the decoded picture
 * is cropped to w x h at (x, y). Returns the new length, 0 if the SPS cannot be parsed (then
 * the caller keeps the original). Scaling matrices in the SPS are not supported (0).
 */
unsigned aa_sps_crop(const uint8_t *in, unsigned n, uint8_t *out, unsigned cap, unsigned x,
                     unsigned y, unsigned w, unsigned h);
/* Coded width and height of an SPS (macroblock-aligned, before cropping); 0 if unparsable. */
int aa_sps_size(const uint8_t *in, unsigned n, unsigned *w, unsigned *h);

#define AA_NAL_SPS 0x80u
#define AA_NAL_PPS 0x100u
#define AA_NAL_IDR 0x20u
#define AA_NAL_SLICE 0x02u
/* Bit mask of NAL unit types (1 << type) in an Annex-B byte stream. */
unsigned aa_nal_types(const uint8_t *p, unsigned n);
/*
 * Copies an Annex-B access unit to out with every SPS rewritten by aa_sps_crop (when crop is
 * set) and, if it is an IDR without parameter sets, ps (the last SPS and PPS) in front.
 * Returns the length, 0 if it does not fit. *idr tells whether it holds an IDR slice.
 */
unsigned aa_annexb_rewrite(const uint8_t *in, unsigned n, uint8_t *out, unsigned cap,
                           const uint8_t *ps, unsigned ps_len, int crop, int *idr);
/* Keeps the SPS and PPS of an Annex-B unit (cropped when crop is set) in ps; returns the length. */
unsigned aa_annexb_parameter_sets(const uint8_t *in, unsigned n, uint8_t *ps, unsigned cap,
                                  int crop);

#endif

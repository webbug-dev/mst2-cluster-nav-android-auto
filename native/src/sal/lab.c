/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * Lab input for the cluster video: when MCN_LAB_VIDEO exists (an H.264 Annex-B recording of a
 * phone's cluster stream, SPS and PPS in front of every IDR), it is played into the shared
 * memory at MCN_CLUSTER_FPS in a loop, the way stream.c publishes the phone's access units,
 * so DisplayManager and the HMI bundle see a phone without one. A key-frame request goes
 * back to the latest IDR. On a car only when GEM "Test video on" copied one from the card
 * (mcn_set.sh lab-on), to check the way to the cluster without a phone.
 *
 * In decode mode (decode.c) the recording goes to the decoder on the unit instead, as the
 * Android Auto sink would send it: the whole file as one stream in a loop, the decoder started
 * again at every loop start and at an SPS of another coded size.
 */
#include "sal.h"
#include "mcn_aa.h"
#include "mcn_video.h"

#define MAP_PRIVATE 0x0002
#define MAX_UNITS 65536
#define LAB_START_MS 4000

static void lab_logn(const char *what, unsigned v)
{
    char line[64];

    snprintf(line, sizeof(line), "%s%u", what, v);
    mcn_log(line);
}

static const uint8_t *file;
static unsigned file_size, units;
static unsigned unit_start[MAX_UNITS + 1];
static uint8_t unit_idr[MAX_UNITS];
/* Formats in the recording: a part begins at a key frame whose SPS differs from the one before
 * (coded size, cropping, profile). One part is played in a loop; DisplayManager's next switch
 * to the phone's stream (struct mcn_video.entries) moves on to the next part, so every format
 * is seen by the cluster as the first one of a freshly started stream. */
#define MAX_PARTS 16
static unsigned part_start[MAX_PARTS + 1], parts;

/* The SPS NAL of the unit starting at `from` (up to `to`): offset and length; 0 if none. */
static unsigned sps_of(unsigned from, unsigned to, unsigned *len)
{
    unsigned i, j;

    for (i = from; i + 4 < to; i++) {
        if (file[i] == 0 && file[i + 1] == 0 && file[i + 2] == 1 && (file[i + 3] & 0x1f) == 7) {
            for (j = i + 3; j + 2 < to; j++) {
                if (file[j] == 0 && file[j + 1] == 0 && file[j + 2] <= 1) {
                    break;
                }
            }
            if (j + 2 >= to) {
                j = to;
            }
            *len = j - (i + 3);
            return i + 3;
        }
    }
    *len = 0;
    return 0;
}

static void find_parts(void)
{
    unsigned u, sps, len, last = 0, last_len = 0;

    parts = 0;
    for (u = 0; u < units; u++) {
        if (!unit_idr[u]) {
            continue;
        }
        sps = sps_of(unit_start[u], unit_start[u + 1], &len);
        if (!len) {
            continue;
        }
        if (!parts || len != last_len || memcmp(file + sps, file + last, len)) {
            if (parts < MAX_PARTS) {
                part_start[parts++] = u;
            }
            last = sps;
            last_len = len;
        }
    }
    if (!parts) {
        part_start[parts++] = 0;
    }
    part_start[parts] = units;
}

/* Splits the recording into access units (H.264 7.4.1.2.3: a new one starts at an AUD, SEI,
 * SPS or PPS, or at a slice with first_mb_in_slice 0, once the current one has a slice). */
static void split(void)
{
    unsigned i;
    int vcl = 0;

    units = 0;
    for (i = 0; i + 4 < file_size && units < MAX_UNITS; i++) {
        unsigned type;
        int starts;

        if (file[i] || file[i + 1] || file[i + 2] != 1) {
            continue;
        }
        type = file[i + 3] & 0x1f;
        starts = units == 0 || (vcl && (type == 9 || type == 6 || type == 7 || type == 8 ||
                                        ((type == 1 || type == 5) && (file[i + 4] & 0x80))));
        if (starts) {
            /* Include the zero byte of a 4-byte start code. */
            unit_start[units] = i && !file[i - 1] ? i - 1 : i;
            unit_idr[units] = 0;
            units++;
            vcl = 0;
        }
        if (type == 1 || type == 5) {
            vcl = 1;
        }
        if (type == 5) {
            unit_idr[units - 1] = 1;
        }
        i += 2;
    }
    unit_start[units] = file_size;
}

static void put(struct mcn_video *video, unsigned u)
{
    unsigned seq = video->write_seq + 1, size = unit_start[u + 1] - unit_start[u];
    struct mcn_video_slot *slot = &video->slot[seq % MCN_VIDEO_SLOTS];

    if (size > MCN_VIDEO_AU_MAX) {
        return;
    }
    slot->seq = 0;
    __sync_synchronize();
    memcpy(slot->data, file + unit_start[u], size);
    slot->size = size;
    slot->flags = unit_idr[u] ? MCN_VIDEO_IDR : 0;
    slot->pts_us = (unsigned long long)mcn_now_ms() * 1000u;
    __sync_synchronize();
    slot->seq = seq;
    video->write_seq = seq;
    video->last_frame_ms = mcn_now_ms();
    video->alive_ms = video->last_frame_ms; /* the recording's "connection" is open while it plays */
    mcn_video_wake(video);
}

/* Coded size from the SPS of unit u; 0 if it has none. */
static int size_of(unsigned u, unsigned *w, unsigned *h)
{
    unsigned len, sps = sps_of(unit_start[u], unit_start[u + 1], &len);

    return len && aa_sps_size(file + sps, len, w, h);
}

/* Decode mode: the whole recording to the decoder, in a loop, until the mode is off. */
static void play_decoded(void)
{
    static uint8_t ps[1024];
    unsigned u = 0, token = 0, told = 0, checked = 0, w = 0, h = 0, cur_w = 0, cur_h = 0, n, k;

    mcn_log("lab video: decode mode, the whole recording to the decoder in a loop");
    for (;;) {
        unsigned now = mcn_now_ms();
        int idr = unit_idr[u];

        if (!checked || now - checked >= 1000) {
            checked = now ? now : 1;
            if (!mcn_decode_on()) {
                break;
            }
            if (!mcn_active() || mcn_stream_live() || mcn_aa_stream_live()) {
                if (token) {
                    mcn_decode_end(token);
                    token = 0;
                }
                usleep(1000000);
                continue;
            }
        }
        if (idr && size_of(u, &w, &h) && (!token || !u || w != cur_w || h != cur_h)) {
            /* The loop starts again, or the coded size changes: a fresh decoder. */
            if (token) {
                mcn_decode_end(token);
            }
            cur_w = w;
            cur_h = h;
            token = mcn_decode_begin(w, h);
        }
        if (token && u && mcn_decode_want_key(token)) {
            u = 0; /* a unit was lost: the stream starts over at its key frame */
            continue;
        }
        n = unit_start[u + 1] - unit_start[u];
        k = idr ? aa_annexb_parameter_sets(file + unit_start[u], n, ps, sizeof(ps), 0) : 0;
        if (token) {
            mcn_decode_put(token, file + unit_start[u], n, idr, k ? ps : NULL, k);
            if (mcn_decode_live(token) && now - told >= 1000) {
                told = now;
                mcn_link_send("video=1");
            }
        }
        u = u + 1 < units ? u + 1 : 0;
        usleep(1000000 / MCN_CLUSTER_FPS);
    }
    if (token) {
        mcn_decode_end(token);
    }
    mcn_log("lab video: decode mode off, playing into the shared memory");
}

static void *play(void *arg)
{
    struct mcn_video *video;
    unsigned u = 0, told = 0, part = 0, entries = 0, checked = 0;
    int ours = 0;

    (void)arg;
    /* Let SAL come up first, but be there before the cluster's encoder starts (about 30 s after
     * power-on, SAL at 22 s), so that a format is the first thing the cluster sees. */
    usleep(LAB_START_MS * 1000);
    video = mcn_video_map();
    if (!video) {
        return NULL;
    }
    mcn_log("lab video: playing");
    /* DisplayManager's first switch to this stream then starts with the first part. */
    part = parts - 1;
    for (;;) {
        if (!checked || mcn_now_ms() - checked >= 1000) {
            checked = mcn_now_ms() ? mcn_now_ms() : 1;
            if (mcn_decode_on()) {
                play_decoded();
                ours = 0;
                continue;
            }
        }
        if (!mcn_active() || mcn_stream_live() || mcn_aa_stream_live()) {
            /* Never mixed with a real phone's stream, CarPlay's or Android Auto's: the memory
             * has one writer (car, 2026-11-09: only CarPlay was looked at, and the recording
             * reset the sequence under a running Android Auto stream). */
            ours = 0;
            usleep(1000000);
            continue;
        }
        if (!ours || video->entries != entries) {
            /* Ours from here, or DisplayManager starts again: a new stream, the next format. */
            if (ours && parts > 1) {
                part = part + 1 < parts ? part + 1 : 0;
                lab_logn("lab video: part ", part + 1);
            }
            ours = 1;
            entries = video->entries;
            video->generation++;
            video->write_seq = 0;
            u = part_start[part];
        }
        if (video->want_idr) {
            video->want_idr = 0;
            while (u > part_start[part] && !unit_idr[u]) {
                u--;
            }
        }
        put(video, u);
        if (video->last_frame_ms - told >= 1000) {
            told = video->last_frame_ms;
            mcn_link_send("video=1");
        }
        u = u + 1 < part_start[part + 1] ? u + 1 : part_start[part];
        usleep(1000000 / MCN_CLUSTER_FPS);
    }
    return NULL;
}

void mcn_lab_start(void)
{
    pthread_t thread;
    int fd = open(MCN_LAB_VIDEO, O_RDONLY);
    off_t size;
    void *p;

    if (fd < 0) {
        return;
    }
    size = lseek(fd, 0, SEEK_END);
    p = size > 0 ? mmap(NULL, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    close(fd);
    if (p == MAP_FAILED) {
        mcn_log("lab video: cannot map " MCN_LAB_VIDEO);
        return;
    }
    file = p;
    file_size = (unsigned)size;
    split();
    find_parts();
    lab_logn("lab video: parts ", parts);
    if (!units || !unit_idr[0] || pthread_create(&thread, NULL, play, NULL)) {
        mcn_log("lab video: no key frame at the start, not played");
        return;
    }
    pthread_detach(thread);
}

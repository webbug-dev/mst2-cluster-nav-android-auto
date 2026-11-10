/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * Diagnostic recording of the phone's cluster stream. When a mounted card has the empty file
 * mst2-cluster-nav-capture-on in its root, the access units of the next stream are written, as
 * they are published to DisplayManager, to mst2-cluster-nav-capture.h264 on that card (Annex-B,
 * playable by ffmpeg and by the lab player), with one line a unit in
 * mst2-cluster-nav-capture.txt: milliseconds, bytes, I or P. Nothing is written without the
 * flag file, and never more than CAPTURE_MAX bytes.
 */
#include "sal.h"

#define CAPTURE_MAX (12u * 1024u * 1024u)
#define SYNC_MS 1000

int fsync(int fd);

static int data = -1, index_fd = -1;
static unsigned written, synced_ms;

static int exists(const char *path)
{
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        return 0;
    }
    close(fd);
    return 1;
}

void mcn_capture_end(void)
{
    if (data >= 0) {
        fsync(data);
        close(data);
        data = -1;
        mcn_log("capture: file closed");
    }
    if (index_fd >= 0) {
        fsync(index_fd);
        close(index_fd);
        index_fd = -1;
    }
}

void mcn_capture_begin(void)
{
    char flag[] = "/media/mp000/mst2-cluster-nav-capture-on";
    char out[] = "/media/mp000/mst2-cluster-nav-capture.h264";
    char idx[] = "/media/mp000/mst2-cluster-nav-capture.txt";
    int i;

    mcn_capture_end();
    for (i = 0; i < 10; i++) {
        flag[11] = out[11] = idx[11] = (char)('0' + i); /* /media/mp000 .. mp009 */
        if (!exists(flag)) {
            continue;
        }
        data = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        index_fd = open(idx, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        written = 0;
        synced_ms = mcn_now_ms();
        mcn_log(data < 0 ? "capture: cannot create the file on the card"
                         : "capture: recording the phone's stream to the card");
        return;
    }
}

void mcn_capture_put(const uint8_t *au, unsigned n, int idr)
{
    char line[40];
    unsigned now;
    int k;

    if (data < 0) {
        return;
    }
    if (written + n > CAPTURE_MAX || write(data, au, n) != (int)n) {
        mcn_log("capture: full or write error");
        mcn_capture_end();
        return;
    }
    written += n;
    now = mcn_now_ms();
    if (index_fd >= 0) {
        k = snprintf(line, sizeof(line), "%u %u %c\n", now, n, idr ? 'I' : 'P');
        if (k > 0) {
            write(index_fd, line, (unsigned)k);
        }
    }
    if (now - synced_ms >= SYNC_MS) {
        synced_ms = now;
        fsync(data);
    }
}

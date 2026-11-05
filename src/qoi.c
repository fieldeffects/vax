/* qoi.c - QOI v1.0 decoder for vax layer images (SPEC.md section 6).
 * Layers are complete QOI streams with 4 channels (RGBA). */

#include "vax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QOI_HEADER_SIZE 14
#define QOI_END_MARKER_SIZE 8

static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static VaxStatus qerr(VaxStatus st, char *err, size_t errlen, const char *msg) {
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
    return st;
}

/* Decodes one QOI stream. If img is non-NULL, pixels are stored there;
 * otherwise the stream is only scanned (used to find stream boundaries).
 * width/height always receive the image dimensions; *consumed receives the
 * exact stream length including the end marker. */
static VaxStatus qoi_decode_core(const unsigned char *d, size_t len, VaxImage *img,
                                 uint32_t *width, uint32_t *height, size_t *consumed,
                                 char *err, size_t errlen) {
    if (len < QOI_HEADER_SIZE + QOI_END_MARKER_SIZE)
        return qerr(VAX_EFORMAT, err, errlen, "truncated QOI header");
    if (memcmp(d, "qoif", 4) != 0)
        return qerr(VAX_EFORMAT, err, errlen, "bad QOI magic");

    uint32_t w = be32(d + 4);
    uint32_t h = be32(d + 8);
    uint8_t channels = d[12];
    uint8_t colorspace = d[13];
    *width = w;
    *height = h;
    if (channels != 4)
        return qerr(VAX_EFORMAT, err, errlen, "QOI channels must be 4 (RGBA)");
    if (w == 0 || h == 0)
        return qerr(VAX_EFORMAT, err, errlen, "QOI zero dimension");
    uint64_t total = (uint64_t)w * h;
    if (total > VAX_MAX_LAYER_PX)
        return qerr(VAX_ELIMIT, err, errlen, "layer exceeds pixel limit");

    unsigned char *px = nullptr;
    if (img) {
        px = malloc((size_t)total * 4);
        if (!px)
            return qerr(VAX_EIO, err, errlen, "out of memory");
        img->width = w;
        img->height = h;
        img->channels = channels;
        img->colorspace = colorspace;
        img->pixels = px;
    }

    unsigned char index[64][4] = {{0}};
    unsigned char cur[4] = {0, 0, 0, 255};
    size_t pos = QOI_HEADER_SIZE;
    uint64_t written = 0;

    while (written < total) {
        if (pos >= len)
            goto fail;
        unsigned char b = d[pos++];
        if (b == 0xFE) { /* QOI_OP_RGB */
            if (pos + 3 > len)
                goto fail;
            cur[0] = d[pos];
            cur[1] = d[pos + 1];
            cur[2] = d[pos + 2];
            pos += 3;
        } else if (b == 0xFF) { /* QOI_OP_RGBA */
            if (pos + 4 > len)
                goto fail;
            memcpy(cur, d + pos, 4);
            pos += 4;
        } else if ((b & 0xC0) == 0x00) { /* QOI_OP_INDEX */
            memcpy(cur, index[b & 0x3F], 4);
        } else if ((b & 0xC0) == 0x40) { /* QOI_OP_DIFF */
            cur[0] = (unsigned char)(cur[0] + (int)((b >> 4) & 0x03) - 2);
            cur[1] = (unsigned char)(cur[1] + (int)((b >> 2) & 0x03) - 2);
            cur[2] = (unsigned char)(cur[2] + (int)(b & 0x03) - 2);
        } else if ((b & 0xC0) == 0x80) { /* QOI_OP_LUMA */
            if (pos >= len)
                goto fail;
            unsigned char b2 = d[pos++];
            int dg = (int)(b & 0x3F) - 32;
            int dr = dg + (int)((b2 >> 4) & 0x0F) - 8;
            int db = dg + (int)(b2 & 0x0F) - 8;
            cur[0] = (unsigned char)(cur[0] + dr);
            cur[1] = (unsigned char)(cur[1] + dg);
            cur[2] = (unsigned char)(cur[2] + db);
        } else { /* QOI_OP_RUN */
            uint64_t run = (uint64_t)(b & 0x3F) + 1;
            if (written + run > total) {
                if (img) {
                    free(px);
                    img->pixels = nullptr;
                }
                return qerr(VAX_EFORMAT, err, errlen, "QOI run overflows image");
            }
            if (px)
                for (uint64_t k = 0; k < run; k++)
                    memcpy(px + (size_t)(written + k) * 4, cur, 4);
            written += run;
            continue;
        }
        if (px)
            memcpy(px + (size_t)written * 4, cur, 4);
        unsigned char *slot = index[(cur[0] * 3 + cur[1] * 5 + cur[2] * 7 + cur[3] * 11) % 64];
        memcpy(slot, cur, 4);
        written++;
    }

    if (len - pos < QOI_END_MARKER_SIZE)
        goto fail;
    static const unsigned char marker[QOI_END_MARKER_SIZE] = {0, 0, 0, 0, 0, 0, 0, 1};
    if (memcmp(d + pos, marker, QOI_END_MARKER_SIZE) != 0) {
        if (img) {
            free(px);
            img->pixels = nullptr;
        }
        return qerr(VAX_EFORMAT, err, errlen, "missing QOI end marker");
    }
    *consumed = pos + QOI_END_MARKER_SIZE;
    return VAX_OK;

fail:
    if (img) {
        free(px);
        img->pixels = nullptr;
    }
    return qerr(VAX_EFORMAT, err, errlen, "truncated QOI stream");
}

VaxStatus vax_image_decode(const unsigned char *data, size_t len, VaxImage *img,
                           size_t *consumed, char *err, size_t errlen) {
    img->pixels = nullptr;
    uint32_t w, h;
    return qoi_decode_core(data, len, img, &w, &h, consumed, err, errlen);
}

VaxStatus vax_image_scan(const unsigned char *data, size_t len, uint32_t *width,
                         uint32_t *height, size_t *consumed, char *err, size_t errlen) {
    return qoi_decode_core(data, len, nullptr, width, height, consumed, err, errlen);
}

void vax_image_free(VaxImage *img) {
    if (img) {
        free(img->pixels);
        img->pixels = nullptr;
    }
}

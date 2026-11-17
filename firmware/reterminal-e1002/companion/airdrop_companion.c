#include "airdrop_companion.h"

#include <stdlib.h>
#include <string.h>

#define AC_SOF1 0xA5
#define AC_SOF2 0x5A
#define AC_HEADER 6 /* sof(2) + type + seq + len(2) */
#define AC_CRC 2

struct ac_parser {
    ac_sink sink;
    void *ctx;
    uint8_t buf[AC_MAX_PAYLOAD];
    size_t len;
    size_t want;
    uint8_t type;
    uint8_t seq;
    uint16_t crc_rx;
    uint8_t state;
    int last_error;
};

enum {
    ST_SOF1 = 0,
    ST_SOF2,
    ST_TYPE,
    ST_SEQ,
    ST_LEN_H,
    ST_LEN_L,
    ST_PAYLOAD,
    ST_CRC_H,
    ST_CRC_L,
};

static uint16_t crc16_ccitt(const uint8_t *data, size_t len, uint16_t crc)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}

ac_parser *ac_parser_new(ac_sink sink, void *ctx)
{
    ac_parser *p = (ac_parser *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->sink = sink;
    p->ctx = ctx;
    p->state = ST_SOF1;
    return p;
}

void ac_parser_free(ac_parser *p)
{
    free(p);
}

void ac_parser_reset(ac_parser *p)
{
    if (!p)
        return;
    p->state = ST_SOF1;
    p->len = 0;
    p->want = 0;
}

static int emit(ac_parser *p)
{
    ac_event ev;
    if (!p->sink)
        return 0;

    ev.seq = p->seq;
    ev.data = p->buf;
    ev.len = p->len;
    ev.img_id = 0;
    ev.chunk = 0;

    switch (p->type) {
    case AC_MSG_READY:
        ev.type = AC_EV_READY;
        break;
    case AC_MSG_SESSION_OPEN:
        ev.type = AC_EV_SESSION_OPEN;
        if (p->len >= 1)
            ev.img_id = p->buf[0];
        break;
    case AC_MSG_DATA:
        ev.type = AC_EV_DATA;
        if (p->len >= 1) {
            ev.chunk = p->buf[0];
            ev.data = p->buf + 1;
            ev.len = p->len - 1;
        } else {
            ev.len = 0;
        }
        break;
    case AC_MSG_SESSION_CLOSE:
        ev.type = AC_EV_SESSION_CLOSE;
        if (p->len >= 1)
            ev.img_id = p->buf[0];
        break;
    case AC_MSG_ERROR:
        ev.type = AC_EV_ERROR;
        break;
    default:
        /* Unknown but well-framed message: ignore, do not tear the link down. */
        return 0;
    }
    p->sink(p->ctx, &ev);
    return 0;
}

static int fail(ac_parser *p, uint8_t current)
{
    p->state = ST_SOF1;
    p->len = 0;
    p->want = 0;
    if (current == AC_SOF1)
        p->state = ST_SOF2;
    return -1;
}

int ac_parser_feed(ac_parser *p, const uint8_t *buf, size_t len)
{
    int rc = 0;

    if (!p || (!buf && len))
        return -1;

    for (size_t i = 0; i < len; i++) {
        uint8_t b = buf[i];
        switch (p->state) {
        case ST_SOF1:
            if (b == AC_SOF1)
                p->state = ST_SOF2;
            break;
        case ST_SOF2:
            if (b == AC_SOF2)
                p->state = ST_TYPE;
            else if (b != AC_SOF1)
                p->state = ST_SOF1;
            break;
        case ST_TYPE:
            p->type = b;
            p->state = ST_SEQ;
            break;
        case ST_SEQ:
            p->seq = b;
            p->state = ST_LEN_H;
            break;
        case ST_LEN_H:
            p->want = (size_t)b << 8;
            p->state = ST_LEN_L;
            break;
        case ST_LEN_L:
            p->want |= b;
            p->len = 0;
            if (p->want > AC_MAX_PAYLOAD) {
                rc = fail(p, b);
                break;
            }
            p->state = p->want ? ST_PAYLOAD : ST_CRC_H;
            break;
        case ST_PAYLOAD:
            p->buf[p->len++] = b;
            if (p->len == p->want)
                p->state = ST_CRC_H;
            break;
        case ST_CRC_H:
            p->crc_rx = (uint16_t)b << 8;
            p->state = ST_CRC_L;
            break;
        case ST_CRC_L: {
            uint16_t want_crc;
            p->crc_rx |= b;
            want_crc = crc16_ccitt(&p->type, 1, 0xFFFF);
            want_crc = crc16_ccitt(&p->seq, 1, want_crc);
            uint8_t lh = (uint8_t)(p->want >> 8), ll = (uint8_t)p->want;
            want_crc = crc16_ccitt(&lh, 1, want_crc);
            want_crc = crc16_ccitt(&ll, 1, want_crc);
            want_crc = crc16_ccitt(p->buf, p->want, want_crc);
            if (want_crc != p->crc_rx) {
                rc = fail(p, b);
            } else {
                rc = emit(p) ? -1 : rc;
                p->state = ST_SOF1;
                p->len = 0;
            }
            break;
        }
        default:
            p->state = ST_SOF1;
            break;
        }
    }
    return rc;
}

int ac_encode(uint8_t type, uint8_t seq, const uint8_t *payload, size_t len,
              uint8_t *out, size_t out_cap)
{
    size_t total = AC_HEADER + len + AC_CRC;
    uint16_t crc = 0xFFFF;
    size_t off = 0;

    if (len > AC_MAX_PAYLOAD || !out || out_cap < total)
        return -1;

    out[off++] = AC_SOF1;
    out[off++] = AC_SOF2;
    out[off++] = type;
    out[off++] = seq;
    out[off++] = (uint8_t)(len >> 8);
    out[off++] = (uint8_t)len;
    if (len) {
        memcpy(out + off, payload, len);
        off += len;
    }

    crc = crc16_ccitt(out + 2, 4, crc);
    crc = crc16_ccitt(payload, len, crc);
    out[off++] = (uint8_t)(crc >> 8);
    out[off++] = (uint8_t)crc;
    return (int)off;
}

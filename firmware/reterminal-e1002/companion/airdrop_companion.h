/*
 * E1002 <-> AirDrop companion link.
 *
 * The E1002 has one 2.4 GHz radio and ESP32Drop needs it exclusively for AWDL.
 * The long-term answer is a second radio (an ESP32-C6/H2 running ESP32Drop)
 * that owns AWDL, while the E1002 keeps its normal Wi-Fi station link.
 *
 * This module is the E1002 half of that split: a small, transport-agnostic
 * frame codec. The caller feeds it bytes from UART/SPI and receives session
 * events; it has no ESP-IDF or hardware dependency, so it is host-testable.
 *
 * Wire format (big endian):
 *
 *   A5 5A | type | seq | len_hi len_lo | payload[len] | crc_hi crc_lo
 *
 * The CRC is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over
 * type..payload. Payloads are at most AC_MAX_PAYLOAD bytes.
 */
#ifndef AIRDROP_COMPANION_H
#define AIRDROP_COMPANION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AC_MAX_PAYLOAD 512

/* Frame types. */
enum {
    AC_MSG_READY = 0x01,        /* companion booted, AWDL available */
    AC_MSG_SESSION_OPEN = 0x02, /* payload[0] = img_id */
    AC_MSG_DATA = 0x03,         /* payload[0] = idx<<4|total, rest = blob */
    AC_MSG_SESSION_CLOSE = 0x04,/* payload[0] = img_id */
    AC_MSG_ERROR = 0x7f,        /* payload[0] = error code */
};

typedef enum {
    AC_EV_READY,
    AC_EV_SESSION_OPEN,
    AC_EV_DATA,
    AC_EV_SESSION_CLOSE,
    AC_EV_ERROR,
} ac_event_type;

typedef struct {
    ac_event_type type;
    uint8_t seq;
    uint8_t img_id;
    uint8_t chunk; /* DATA only: idx<<4 | total */
    const uint8_t *data;
    size_t len;
} ac_event;

typedef void (*ac_sink)(void *ctx, const ac_event *ev);

typedef struct ac_parser ac_parser;

ac_parser *ac_parser_new(ac_sink sink, void *ctx);
void ac_parser_free(ac_parser *p);

/* Drop any partial frame. */
void ac_parser_reset(ac_parser *p);

/*
 * Feed bytes. Returns 0 normally, or -1 if a malformed frame was discarded.
 * The parser resynchronises on the next start-of-frame, so one bad frame does
 * not take the link down. `ev.data` is only valid during the callback.
 */
int ac_parser_feed(ac_parser *p, const uint8_t *buf, size_t len);

/* Encode one frame. Returns the frame length, or -1 if it does not fit. */
int ac_encode(uint8_t type, uint8_t seq, const uint8_t *payload, size_t len,
              uint8_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* AIRDROP_COMPANION_H */

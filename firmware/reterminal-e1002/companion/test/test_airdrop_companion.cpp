// Host test for the E1002 <-> AirDrop companion frame link.
//
// Run by the flake's host-tests derivation; no hardware or ESP-IDF needed.

#include "airdrop_companion.h"

#include <cstdio>
#include <cstring>
#include <vector>

struct Event {
    ac_event_type type;
    uint8_t seq;
    uint8_t img_id;
    uint8_t chunk;
    std::vector<uint8_t> data;
};

static void sink(void *ctx, const ac_event *ev)
{
    std::vector<Event> *out = static_cast<std::vector<Event> *>(ctx);
    Event e;
    e.type = ev->type;
    e.seq = ev->seq;
    e.img_id = ev->img_id;
    e.chunk = ev->chunk;
    e.data.assign(ev->data, ev->data + ev->len);
    out->push_back(e);
}

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static int feed_all(ac_parser *p, const uint8_t *buf, size_t len, size_t step)
{
    int rc = 0;
    for (size_t off = 0; off < len; off += step) {
        size_t n = len - off < step ? len - off : step;
        int r = ac_parser_feed(p, buf + off, n);
        if (r < 0)
            rc = r;
    }
    return rc;
}

int main()
{
    // 1. One DATA frame, byte at a time, after junk that must be skipped.
    uint8_t payload[164];
    payload[0] = 0x23; // idx = 2, total = 3
    for (size_t i = 1; i < sizeof(payload); i++)
        payload[i] = static_cast<uint8_t>(i * 7);

    uint8_t frame[200];
    int n = ac_encode(AC_MSG_DATA, 9, payload, sizeof(payload), frame,
                      sizeof(frame));
    CHECK(n == static_cast<int>(6 + sizeof(payload) + 2));

    std::vector<Event> events;
    ac_parser *p = ac_parser_new(sink, &events);
    CHECK(p != nullptr);

    const uint8_t junk[] = {0x00, 0xA5, 0x00, 0x11, 0xA5};
    CHECK(ac_parser_feed(p, junk, sizeof(junk)) == 0);
    CHECK(feed_all(p, frame, static_cast<size_t>(n), 1) == 0);

    CHECK(events.size() == 1);
    if (events.size() == 1) {
        CHECK(events[0].type == AC_EV_DATA);
        CHECK(events[0].seq == 9);
        CHECK(events[0].chunk == 0x23);
        CHECK(events[0].data.size() == sizeof(payload) - 1);
        CHECK(events[0].data[0] == 7);
        CHECK(events[0].data[events[0].data.size() - 1] ==
              payload[sizeof(payload) - 1]);
    }

    // 2. Four back-to-back frames fed in 3-byte pieces.
    events.clear();
    uint8_t stream[512];
    size_t used = 0;
    uint8_t body[3] = {0x11, 0xAA, 0xBB};
    uint8_t open[1] = {7};
    int r;
    r = ac_encode(AC_MSG_READY, 1, nullptr, 0, stream + used,
                  sizeof(stream) - used);
    CHECK(r > 0);
    used += static_cast<size_t>(r);
    r = ac_encode(AC_MSG_SESSION_OPEN, 2, open, sizeof(open), stream + used,
                  sizeof(stream) - used);
    CHECK(r > 0);
    used += static_cast<size_t>(r);
    r = ac_encode(AC_MSG_DATA, 3, body, sizeof(body), stream + used,
                  sizeof(stream) - used);
    CHECK(r > 0);
    used += static_cast<size_t>(r);
    r = ac_encode(AC_MSG_SESSION_CLOSE, 4, open, sizeof(open), stream + used,
                  sizeof(stream) - used);
    CHECK(r > 0);
    used += static_cast<size_t>(r);

    CHECK(feed_all(p, stream, used, 3) == 0);
    CHECK(events.size() == 4);
    if (events.size() == 4) {
        CHECK(events[0].type == AC_EV_READY);
        CHECK(events[1].type == AC_EV_SESSION_OPEN && events[1].img_id == 7);
        CHECK(events[2].type == AC_EV_DATA && events[2].chunk == 0x11);
        CHECK(events[2].data.size() == 2 && events[2].data[1] == 0xBB);
        CHECK(events[3].type == AC_EV_SESSION_CLOSE && events[3].img_id == 7);
    }

    // 3. A corrupt frame is dropped, the next good one still parses.
    events.clear();
    n = ac_encode(AC_MSG_READY, 5, nullptr, 0, frame, sizeof(frame));
    CHECK(n > 0);
    frame[n - 1] ^= 0xFF;
    CHECK(ac_parser_feed(p, frame, static_cast<size_t>(n)) < 0);
    n = ac_encode(AC_MSG_READY, 6, nullptr, 0, frame, sizeof(frame));
    CHECK(ac_parser_feed(p, frame, static_cast<size_t>(n)) == 0);
    CHECK(events.size() == 1 && events[0].seq == 6);

    // 4. An oversized length is rejected and the link recovers.
    events.clear();
    const uint8_t oversized[] = {0xA5, 0x5A, 0x03, 0x00, 0x02, 0x01};
    CHECK(ac_parser_feed(p, oversized, sizeof(oversized)) < 0);
    n = ac_encode(AC_MSG_READY, 7, nullptr, 0, frame, sizeof(frame));
    CHECK(ac_parser_feed(p, frame, static_cast<size_t>(n)) == 0);
    CHECK(events.size() == 1 && events[0].type == AC_EV_READY);

    // 5. Encoder bounds.
    uint8_t too_big[AC_MAX_PAYLOAD + 1] = {0};
    CHECK(ac_encode(AC_MSG_DATA, 0, too_big, sizeof(too_big), frame,
                    sizeof(frame)) < 0);
    CHECK(ac_encode(AC_MSG_DATA, 0, body, sizeof(body), frame, 6) < 0);

    ac_parser_free(p);

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS: companion link framing, resync and bounds\n");
    return 0;
}

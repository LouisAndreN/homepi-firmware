// fx_sender.h: bridge side of the remote Teensy update (packets, retries). Header only, C.
// The UART and the reply queue are passed in (fxs_io_t): the same code runs on the ESP32 and in the
// host tests against the Teensy state machine (fx_receiver.h).
#pragma once

#include "fxproto.h"
#include <stdbool.h>

typedef struct {
    // Writes n bytes to the Teensy. Returns 0 on success.
    int (*send)(void *ctx, const uint8_t *buf, size_t n);
    // Waits up to timeout_ms for a reply to (seq, cmd). Returns 0 and fills status/value, or -1.
    int (*wait_reply)(void *ctx, uint16_t seq, uint8_t cmd, uint32_t timeout_ms, uint8_t *status, uint32_t *value);
    void *ctx;
} fxs_io_t;

typedef struct {
    fxs_io_t io;
    uint16_t seq;
    uint32_t size, sent, crc, buffer_size;
    uint8_t  chunk[FXP_CHUNK];   // bytes waiting for a full chunk
    uint32_t chunk_len;
    uint8_t  pkt[FXP_MAX_PACKET];
    uint8_t  last_status;        // status of the last refused packet
    uint32_t retries;            // total retransmissions (diagnostics)
} fxs_t;

#define FXS_RETRIES       3
#define FXS_TIMEOUT_MS    1500   // a 1 KB flash write takes a few ms; the reply comes back at once
#define FXS_END_TIMEOUT   5000   // END re-reads the whole buffer to check the crc32

// One packet, same seq on every retransmission. Returns 0 (status OK), >0 (Teensy status), -1 (no reply).
static inline int fxs_transact(fxs_t *s, uint8_t type, const uint8_t *pl, uint16_t len, uint32_t timeout_ms,
                               uint32_t *value)
{
    uint16_t seq = ++s->seq;
    size_t n = fxp_build(s->pkt, type, seq, pl, len);
    if (!n) return -1;
    for (int attempt = 0; attempt <= FXS_RETRIES; attempt++) {
        if (attempt) s->retries++;
        if (s->io.send(s->io.ctx, s->pkt, n) != 0) continue;
        uint8_t status; uint32_t v = 0;
        if (s->io.wait_reply(s->io.ctx, seq, type, timeout_ms, &status, &v) == 0) {
            if (value) *value = v;
            if (status != FXP_OK) s->last_status = status;
            return status;
        }
    }
    return -1;
}

static inline int fxs_begin(fxs_t *s, uint32_t size, uint8_t major, uint8_t minor)
{
    s->size = size; s->sent = 0; s->crc = 0; s->chunk_len = 0; s->last_status = 0; s->retries = 0;
    uint8_t pl[6];
    fxp_put32(pl, size); pl[4] = major; pl[5] = minor;
    return fxs_transact(s, FXP_BEGIN, pl, 6, FXS_TIMEOUT_MS, &s->buffer_size);
}

static inline int fxs_flush_chunk(fxs_t *s)
{
    if (!s->chunk_len) return 0;
    uint8_t pl[4 + FXP_CHUNK];
    fxp_put32(pl, s->sent);
    memcpy(pl + 4, s->chunk, s->chunk_len);
    int r = fxs_transact(s, FXP_DATA, pl, (uint16_t)(4 + s->chunk_len), FXS_TIMEOUT_MS, NULL);
    if (r != 0) return r;
    s->crc = fxp_crc32_update(s->crc, s->chunk, s->chunk_len);
    s->sent += s->chunk_len;
    s->chunk_len = 0;
    return 0;
}

// Feeds image bytes as they arrive (any size); full chunks are sent at once
static inline int fxs_data(fxs_t *s, const uint8_t *d, size_t n)
{
    while (n) {
        size_t take = FXP_CHUNK - s->chunk_len;
        if (take > n) take = n;
        memcpy(s->chunk + s->chunk_len, d, take);
        s->chunk_len += (uint32_t)take; d += take; n -= take;
        if (s->chunk_len == FXP_CHUNK) { int r = fxs_flush_chunk(s); if (r) return r; }
    }
    return 0;
}

// Sends the last partial chunk and END. The image size is a multiple of 4 (teensy-sign pads it).
static inline int fxs_end(fxs_t *s)
{
    int r = fxs_flush_chunk(s);
    if (r) return r;
    if (s->sent != s->size) { s->last_status = FXP_E_SIZE; return FXP_E_SIZE; }
    uint8_t pl[4];
    fxp_put32(pl, s->crc);
    return fxs_transact(s, FXP_END, pl, 4, FXS_END_TIMEOUT, NULL);
}

static inline int fxs_commit(fxs_t *s) { return fxs_transact(s, FXP_COMMIT, NULL, 0, FXS_TIMEOUT_MS, NULL); }
static inline int fxs_abort(fxs_t *s)  { return fxs_transact(s, FXP_ABORT, NULL, 0, FXS_TIMEOUT_MS, NULL); }
static inline int fxs_ping(fxs_t *s, uint32_t *fw) { return fxs_transact(s, FXP_PING, NULL, 0, FXS_TIMEOUT_MS, fw); }
static inline int fxs_reboot(fxs_t *s, uint32_t *fw) { return fxs_transact(s, FXP_REBOOT, NULL, 0, FXS_TIMEOUT_MS, fw); }

static inline const char *fxs_status_name(int st)
{
    switch (st) {
    case -1: return "no_reply";
    case FXP_OK: return "ok";
    case FXP_E_STATE: return "bad_state";
    case FXP_E_TOO_BIG: return "image_too_big";
    case FXP_E_NO_BUFFER: return "no_free_flash";
    case FXP_E_OFFSET: return "bad_offset";
    case FXP_E_ALIGN: return "bad_alignment";
    case FXP_E_FLASH: return "flash_write_error";
    case FXP_E_SIZE: return "incomplete_image";
    case FXP_E_CRC: return "crc_mismatch";
    case FXP_E_TARGET: return "not_a_teensy40_image";
    case FXP_E_MARKER: return "no_remote_update_marker";
    case FXP_E_ARG: return "bad_packet";
    default: return "unknown";
    }
}

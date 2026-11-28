// fxproto.h: remote update protocol between the NILM bridge (XIAO ESP32-S3) and the Teensy 4.0.
//
// IDENTICAL COPY in homepi-firmware (homepi_nilm/main/fxproto.h) and homepi-nilm
// (teensy/teensy_nilm_phase2_v3_1/fxproto.h): change both or neither. Header only, C and C++.
//
// Bridge -> Teensy (Teensy Serial1 RX, 921600 8N1), one packet:
//   0xC3 0x3C | type u8 | seq u16 | len u16 | payload[len] | crc32 u32   (little-endian)
//   crc32 (IEEE 802.3, the zlib one) covers type .. payload. Anything that does not parse is ignored:
//   the ESP32-S3 ROM prints its boot messages on the same wire at 115200 at every reset of the XIAO
//   (only an eFuse could stop that), so noise on this line is normal, never a command.
//
// Teensy -> bridge: a frame of the existing stream (0xAA 0x55 type len payload crc8), type 0x04,
// len 8: seq u16 | cmd u8 (type answered) | status u8 | value u32. Data frames (type 0x03) stop
// while an update is in progress and resume after the reboot.
//
// Sequence: BEGIN(size, version) -> DATA(offset, bytes)... -> END(crc32 of the image) -> COMMIT.
// The Teensy writes into the free flash above its program (FlasherX buffer) and replaces the program
// only on COMMIT, after checking size, crc32, the target id and the remote-update marker. The bridge
// sends COMMIT only after the SHA-256 of the image matched the signed manifest. ABORT, a timeout or
// any error on the Teensy side erases the buffer and reboots into the unchanged program.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define FXP_SYNC0        0xC3
#define FXP_SYNC1        0x3C
#define FXP_HDR_LEN      7          // sync(2) type(1) seq(2) len(2)
#define FXP_CRC_LEN      4
#define FXP_CHUNK        1024       // image bytes per DATA packet (multiple of 4)
#define FXP_MAX_PAYLOAD  (4 + FXP_CHUNK)
#define FXP_MAX_PACKET   (FXP_HDR_LEN + FXP_MAX_PAYLOAD + FXP_CRC_LEN)

// Packet types (bridge -> Teensy)
#define FXP_BEGIN   0x10   // size u32, version major u8, minor u8
#define FXP_DATA    0x11   // offset u32, bytes (len - 4, multiple of 4)
#define FXP_END     0x12   // crc32 u32 of the whole image
#define FXP_COMMIT  0x13   // -
#define FXP_ABORT   0x14   // -
#define FXP_PING    0x15   // -
#define FXP_REBOOT  0x16   // - (v3.3: restart the Teensy, e.g. after a bad start of the ADCs; any update in progress is aborted)

// Reply frame (Teensy -> bridge), in the AA 55 stream
#define FXP_REPLY_TYPE  0x04
#define FXP_REPLY_LEN   8

// Status codes in a reply
#define FXP_OK            0
#define FXP_E_STATE       1   // packet not valid in the current state
#define FXP_E_TOO_BIG     2   // image larger than the free flash
#define FXP_E_NO_BUFFER   3   // no free flash above the program
#define FXP_E_OFFSET      4   // DATA not at the expected offset
#define FXP_E_ALIGN       5   // DATA length not a multiple of 4
#define FXP_E_FLASH       6   // flash write error
#define FXP_E_SIZE        7   // END before all bytes arrived
#define FXP_E_CRC         8   // crc32 of the image differs
#define FXP_E_TARGET      9   // target id ("fw_teensy40") not in the image
#define FXP_E_MARKER      10  // remote-update marker not in the image: it could never be updated again
#define FXP_E_ARG         11  // malformed payload

// Present in every image that can receive the next remote update. The Teensy refuses an image
// without it: flashing it would make the next update USB-only.
#define FXP_MARKER "HOMEPI_FX_REMOTE_UPDATE_V1"

static inline uint32_t fxp_crc32_update(uint32_t crc, const uint8_t *d, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *d++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static inline void fxp_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void fxp_put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static inline uint16_t fxp_get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t fxp_get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// Builds a packet into out (FXP_MAX_PACKET bytes). Returns its length, 0 if len is too large.
static inline size_t fxp_build(uint8_t *out, uint8_t type, uint16_t seq, const uint8_t *payload, uint16_t len)
{
    if (len > FXP_MAX_PAYLOAD) return 0;
    out[0] = FXP_SYNC0; out[1] = FXP_SYNC1; out[2] = type;
    fxp_put16(out + 3, seq); fxp_put16(out + 5, len);
    if (len) memcpy(out + FXP_HDR_LEN, payload, len);
    uint32_t crc = fxp_crc32_update(0, out + 2, (size_t)5 + len);
    fxp_put32(out + FXP_HDR_LEN + len, crc);
    return FXP_HDR_LEN + (size_t)len + FXP_CRC_LEN;
}

// Byte-at-a-time parser (Teensy side)
typedef struct {
    uint8_t  state;          // 0 sync0, 1 sync1, 2 header, 3 payload+crc
    uint8_t  hdr[5];         // type seq len
    uint16_t pos, len;
    uint8_t  type;
    uint16_t seq;
    uint8_t  payload[FXP_MAX_PAYLOAD + FXP_CRC_LEN];
    uint32_t bad_crc;        // packets dropped on crc (diagnostics)
} fxp_parser_t;

static inline void fxp_parser_reset(fxp_parser_t *p) { p->state = 0; p->pos = 0; }

// Feeds one byte. Returns 1 when a complete packet with a valid crc is in p (type, seq, len,
// payload), 0 otherwise. A bad crc or an impossible length drops the packet silently.
static inline int fxp_feed(fxp_parser_t *p, uint8_t b)
{
    switch (p->state) {
    case 0: if (b == FXP_SYNC0) p->state = 1; return 0;
    case 1: p->state = (b == FXP_SYNC1) ? 2 : (b == FXP_SYNC0 ? 1 : 0); p->pos = 0; return 0;
    case 2:
        p->hdr[p->pos++] = b;
        if (p->pos < 5) return 0;
        p->type = p->hdr[0]; p->seq = fxp_get16(p->hdr + 1); p->len = fxp_get16(p->hdr + 3);
        if (p->len > FXP_MAX_PAYLOAD || p->type < FXP_BEGIN || p->type > FXP_REBOOT) { p->state = (b == FXP_SYNC0) ? 1 : 0; return 0; }
        p->pos = 0; p->state = 3;
        return 0;
    default:
        p->payload[p->pos++] = b;
        if (p->pos < p->len + FXP_CRC_LEN) return 0;
        p->state = 0;
        {
            uint32_t crc = fxp_crc32_update(0, p->hdr, 5);
            crc = fxp_crc32_update(crc, p->payload, p->len);
            if (crc != fxp_get32(p->payload + p->len)) { p->bad_crc++; return 0; }
        }
        return 1;
    }
}

// Reply payload (8 bytes, carried in an AA 55 04 08 ... crc8 frame)
static inline void fxp_reply_payload(uint8_t *out8, uint16_t seq, uint8_t cmd, uint8_t status, uint32_t value)
{
    fxp_put16(out8, seq); out8[2] = cmd; out8[3] = status; fxp_put32(out8 + 4, value);
}

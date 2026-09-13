#include "aiud_protocol.h"

#include <string.h>

static uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int16_t read_i16_le(const uint8_t *p)
{
    return (int16_t)read_u16_le(p);
}

static int64_t read_i64_le(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value |= (uint64_t)p[i] << (i * 8);
    }
    return (int64_t)value;
}

static bool valid_status(uint8_t status)
{
    return status >= AIUD_STATUS_AVAILABLE &&
           status <= AIUD_STATUS_AUTHENTICATION_REQUIRED;
}

static aiud_provider_status_t legacy_status(uint8_t flags,
                                            uint8_t available_bit,
                                            uint8_t configured_bit)
{
    if ((flags & available_bit) != 0) {
        return AIUD_STATUS_AVAILABLE;
    }
    if ((flags & configured_bit) != 0) {
        return AIUD_STATUS_TEMPORARILY_UNAVAILABLE;
    }
    return AIUD_STATUS_NOT_CONFIGURED;
}

static void read_window(const uint8_t *p, aiud_usage_window_t *window)
{
    const uint16_t permille = read_u16_le(p);
    window->valid = permille != AIUD_UNKNOWN_PERMILLE;
    window->used_permille = window->valid ? permille : 0;
    window->resets_at = read_u32_le(p + 2);
}

static void read_daily(const uint8_t *p, aiud_daily_usage_t *daily)
{
    for (unsigned i = 0; i < AIUD_DAILY_DAYS; ++i) {
        daily->percent[i] = p[i];
        if (p[i] != AIUD_UNKNOWN_DAILY) daily->valid = true;
    }
}

aiud_parse_result_t aiud_parse_packet(const uint8_t *packet, size_t length,
                                      aiud_snapshot_t *snapshot)
{
    if (packet == NULL || snapshot == NULL || length != AIUD_PACKET_SIZE) {
        return AIUD_PARSE_BAD_LENGTH;
    }
    if (memcmp(packet, "AIUD", 4) != 0) {
        return AIUD_PARSE_BAD_MAGIC;
    }
    if (packet[4] != AIUD_PROTOCOL_VERSION) {
        return AIUD_PARSE_UNSUPPORTED_VERSION;
    }
    if ((packet[6] != 0 && !valid_status(packet[6])) ||
        (packet[7] != 0 && !valid_status(packet[7]))) {
        return AIUD_PARSE_BAD_STATUS;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->protocol_version = packet[4];
    snapshot->flags = packet[5];
    snapshot->generated_at = read_i64_le(packet + 8);
    snapshot->utc_offset_minutes = read_i16_le(packet + 40);
    snapshot->codex.status = packet[6] != 0
        ? (aiud_provider_status_t)packet[6]
        : legacy_status(packet[5], 1U << 0, 1U << 2);
    snapshot->claude.status = packet[7] != 0
        ? (aiud_provider_status_t)packet[7]
        : legacy_status(packet[5], 1U << 1, 1U << 3);

    read_window(packet + 16, &snapshot->codex.session);
    read_window(packet + 22, &snapshot->codex.weekly);
    read_window(packet + 28, &snapshot->claude.session);
    read_window(packet + 34, &snapshot->claude.weekly);
    snapshot->current_weekday = packet[56] <= 6 ? packet[56] : 0;
    if ((packet[5] & (1U << 4)) != 0) {
        read_daily(packet + 42, &snapshot->codex_daily);
        read_daily(packet + 49, &snapshot->claude_daily);
    } else {
        memset(snapshot->codex_daily.percent, AIUD_UNKNOWN_DAILY,
               sizeof(snapshot->codex_daily.percent));
        memset(snapshot->claude_daily.percent, AIUD_UNKNOWN_DAILY,
               sizeof(snapshot->claude_daily.percent));
    }
    return AIUD_PARSE_OK;
}

const char *aiud_parse_result_name(aiud_parse_result_t result)
{
    switch (result) {
    case AIUD_PARSE_OK: return "ok";
    case AIUD_PARSE_BAD_LENGTH: return "bad_length";
    case AIUD_PARSE_BAD_MAGIC: return "bad_magic";
    case AIUD_PARSE_UNSUPPORTED_VERSION: return "unsupported_version";
    case AIUD_PARSE_BAD_STATUS: return "bad_status";
    default: return "unknown";
    }
}

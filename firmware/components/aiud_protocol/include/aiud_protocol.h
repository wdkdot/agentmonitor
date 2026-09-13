#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIUD_PACKET_SIZE 64U
#define AIUD_PROTOCOL_VERSION 1U
#define AIUD_UNKNOWN_PERMILLE UINT16_MAX
#define AIUD_DAILY_DAYS 7U
#define AIUD_UNKNOWN_DAILY UINT8_MAX

typedef enum {
    AIUD_STATUS_UNKNOWN = 0,
    AIUD_STATUS_AVAILABLE = 1,
    AIUD_STATUS_NOT_CONFIGURED = 2,
    AIUD_STATUS_TEMPORARILY_UNAVAILABLE = 3,
    AIUD_STATUS_AUTHENTICATION_REQUIRED = 4,
} aiud_provider_status_t;

typedef struct {
    bool valid;
    uint16_t used_permille;
    uint32_t resets_at;
} aiud_usage_window_t;

typedef struct {
    aiud_provider_status_t status;
    aiud_usage_window_t session;
    aiud_usage_window_t weekly;
} aiud_provider_t;

typedef struct {
    bool valid;
    uint8_t percent[AIUD_DAILY_DAYS];
} aiud_daily_usage_t;

typedef struct {
    uint8_t protocol_version;
    uint8_t flags;
    int64_t generated_at;
    int16_t utc_offset_minutes;
    aiud_provider_t codex;
    aiud_provider_t claude;
    aiud_daily_usage_t codex_daily;
    aiud_daily_usage_t claude_daily;
    uint8_t current_weekday;
} aiud_snapshot_t;

typedef enum {
    AIUD_PARSE_OK = 0,
    AIUD_PARSE_BAD_LENGTH,
    AIUD_PARSE_BAD_MAGIC,
    AIUD_PARSE_UNSUPPORTED_VERSION,
    AIUD_PARSE_BAD_STATUS,
} aiud_parse_result_t;

aiud_parse_result_t aiud_parse_packet(const uint8_t *packet, size_t length,
                                      aiud_snapshot_t *snapshot);
const char *aiud_parse_result_name(aiud_parse_result_t result);

#ifdef __cplusplus
}
#endif

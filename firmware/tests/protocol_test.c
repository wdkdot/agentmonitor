#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "aiud_protocol.h"

static void detailed_statuses_decode(void)
{
    uint8_t packet[AIUD_PACKET_SIZE] = {0};
    memcpy(packet, "AIUD", 4);
    packet[4] = AIUD_PROTOCOL_VERSION;
    packet[5] = (1U << 2) | (1U << 3);
    packet[6] = AIUD_STATUS_TEMPORARILY_UNAVAILABLE;
    packet[7] = AIUD_STATUS_AUTHENTICATION_REQUIRED;
    packet[16] = 0x7b;
    packet[5] |= 1U << 4;
    packet[42] = 7;
    packet[43] = AIUD_UNKNOWN_DAILY;
    packet[49] = 12;
    packet[56] = 1;

    aiud_snapshot_t snapshot;
    assert(aiud_parse_packet(packet, sizeof(packet), &snapshot) == AIUD_PARSE_OK);
    assert(snapshot.codex.status == AIUD_STATUS_TEMPORARILY_UNAVAILABLE);
    assert(snapshot.claude.status == AIUD_STATUS_AUTHENTICATION_REQUIRED);
    assert(snapshot.codex.session.valid);
    assert(snapshot.codex.session.used_permille == 123);
    assert(snapshot.codex_daily.valid);
    assert(snapshot.codex_daily.percent[0] == 7);
    assert(snapshot.codex_daily.percent[1] == AIUD_UNKNOWN_DAILY);
    assert(snapshot.claude_daily.percent[0] == 12);
    assert(snapshot.current_weekday == 1);
}

static void legacy_flags_decode(void)
{
    uint8_t packet[AIUD_PACKET_SIZE] = {0};
    memcpy(packet, "AIUD", 4);
    packet[4] = AIUD_PROTOCOL_VERSION;
    packet[5] = 1U << 0;

    aiud_snapshot_t snapshot;
    assert(aiud_parse_packet(packet, sizeof(packet), &snapshot) == AIUD_PARSE_OK);
    assert(snapshot.codex.status == AIUD_STATUS_AVAILABLE);
    assert(snapshot.claude.status == AIUD_STATUS_NOT_CONFIGURED);
    assert(!snapshot.codex_daily.valid);
    assert(snapshot.codex_daily.percent[0] == AIUD_UNKNOWN_DAILY);
}

static void malformed_packets_are_rejected(void)
{
    uint8_t packet[AIUD_PACKET_SIZE] = {0};
    aiud_snapshot_t snapshot;
    assert(aiud_parse_packet(packet, sizeof(packet), &snapshot) == AIUD_PARSE_BAD_MAGIC);
    memcpy(packet, "AIUD", 4);
    packet[4] = 99;
    assert(aiud_parse_packet(packet, sizeof(packet), &snapshot) ==
           AIUD_PARSE_UNSUPPORTED_VERSION);
    assert(aiud_parse_packet(packet, sizeof(packet) - 1, &snapshot) ==
           AIUD_PARSE_BAD_LENGTH);
}

int main(void)
{
    detailed_statuses_decode();
    legacy_flags_decode();
    malformed_packets_are_rejected();
    puts("protocol tests passed");
    return 0;
}

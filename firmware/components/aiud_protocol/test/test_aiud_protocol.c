#include "aiud_protocol.h"
#include <string.h>
#include "unity.h"

TEST_CASE("v1 packet decodes detailed provider states", "[aiud]")
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
    packet[17] = 0x00;
    packet[28] = 0xff;
    packet[29] = 0xff;
    packet[40] = 0x1c;
    packet[41] = 0x02;

    aiud_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(AIUD_PARSE_OK,
                      aiud_parse_packet(packet, sizeof(packet), &snapshot));
    TEST_ASSERT_EQUAL(AIUD_STATUS_TEMPORARILY_UNAVAILABLE,
                      snapshot.codex.status);
    TEST_ASSERT_EQUAL(AIUD_STATUS_AUTHENTICATION_REQUIRED,
                      snapshot.claude.status);
    TEST_ASSERT_TRUE(snapshot.codex.session.valid);
    TEST_ASSERT_EQUAL_UINT16(123, snapshot.codex.session.used_permille);
    TEST_ASSERT_TRUE(snapshot.codex_daily.valid);
    TEST_ASSERT_EQUAL_UINT8(7, snapshot.codex_daily.percent[0]);
    TEST_ASSERT_EQUAL_UINT8(AIUD_UNKNOWN_DAILY, snapshot.codex_daily.percent[1]);
    TEST_ASSERT_EQUAL_UINT8(12, snapshot.claude_daily.percent[0]);
    TEST_ASSERT_EQUAL_UINT8(1, snapshot.current_weekday);
    TEST_ASSERT_FALSE(snapshot.claude.session.valid);
    TEST_ASSERT_EQUAL_INT16(540, snapshot.utc_offset_minutes);
}

TEST_CASE("legacy zero status bytes fall back to flags", "[aiud]")
{
    uint8_t packet[AIUD_PACKET_SIZE] = {0};
    memcpy(packet, "AIUD", 4);
    packet[4] = AIUD_PROTOCOL_VERSION;
    packet[5] = (1U << 0);

    aiud_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(AIUD_PARSE_OK,
                      aiud_parse_packet(packet, sizeof(packet), &snapshot));
    TEST_ASSERT_EQUAL(AIUD_STATUS_AVAILABLE, snapshot.codex.status);
    TEST_ASSERT_EQUAL(AIUD_STATUS_NOT_CONFIGURED, snapshot.claude.status);
}

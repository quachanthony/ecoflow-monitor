// Host-side protocol tests: the C++ port's byte output must match vectors
// generated from the Python reference implementation (see
// tools/gen_test_vectors.py). Run with: pio test -e native
#include <string.h>
#include <unity.h>

#include <vector>

#include "../../src/ef/crc.h"
#include "../../src/ef/crypto.h"
#include "../../src/ef/packet.h"
#include "../../src/ef/river2.h"
#include "vectors.h"

using namespace ef;

static void expectBytes(const uint8_t *expected, size_t expected_len,
                        const std::vector<uint8_t> &actual) {
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(expected_len), (uint32_t)(actual.size()));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, actual.data(), expected_len);
}

// --- CRC ---

static void test_crc(void) {
  TEST_ASSERT_EQUAL_HEX8(CRC8_SAMPLE, crc8(CRC_INPUT, CRC_INPUT_len));
  TEST_ASSERT_EQUAL_HEX16(CRC16_SAMPLE, crc16(CRC_INPUT, CRC_INPUT_len));
}

// --- Packet encoding vs reference ---

static void test_encode_ac_on(void) {
  expectBytes(PKT_AC_ON, PKT_AC_ON_len, river2::buildAcToggle(true).toBytes());
}

static void test_encode_dc_off(void) {
  expectBytes(PKT_DC_OFF, PKT_DC_OFF_len, river2::buildDcToggle(false).toBytes());
}

static void test_encode_auth_status(void) {
  Packet p(0x21, 0x35, 0x35, 0x89, {}, 2);
  expectBytes(PKT_AUTH_STATUS, PKT_AUTH_STATUS_len, p.toBytes());
}

static void test_encode_auth_submit(void) {
  // MD5("1234567890123456789" + "R621ZAB1TEST0001") as uppercase hex ASCII
  const char *user_id = "1234567890123456789";
  const char *sn = "R621ZAB1TEST0001";
  std::vector<uint8_t> material;
  for (const char *c = user_id; *c; c++) material.push_back(*c);
  for (const char *c = sn; *c; c++) material.push_back(*c);
  uint8_t digest[16];
  md5(material.data(), material.size(), digest);

  std::vector<uint8_t> payload(32);
  static const char HEX[] = "0123456789ABCDEF";
  for (int i = 0; i < 16; i++) {
    payload[i * 2] = HEX[digest[i] >> 4];
    payload[i * 2 + 1] = HEX[digest[i] & 0x0F];
  }
  Packet p(0x21, 0x35, 0x35, 0x86, payload, 2);
  expectBytes(PKT_AUTH_SUBMIT, PKT_AUTH_SUBMIT_len, p.toBytes());
}

static void test_encode_v3(void) {
  Packet p(0x35, 0x21, 0x35, 0x86, {0x00}, 3);
  expectBytes(PKT_V3, PKT_V3_len, p.toBytes());
}

// --- Packet decoding ---

static void test_decode_telemetry(void) {
  Packet p;
  TEST_ASSERT_TRUE(Packet::fromBytes(PKT_TELEMETRY, PKT_TELEMETRY_len, p));
  TEST_ASSERT_EQUAL_HEX8(0x05, p.src);
  TEST_ASSERT_EQUAL_HEX8(0x21, p.dst);
  TEST_ASSERT_EQUAL_HEX8(0x20, p.cmd_set);
  TEST_ASSERT_EQUAL_HEX8(0x02, p.cmd_id);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(PKT_TELEMETRY_PAYLOAD_len), (uint32_t)(p.payload.size()));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(PKT_TELEMETRY_PAYLOAD, p.payload.data(),
                               PKT_TELEMETRY_PAYLOAD_len);
}

static void test_decode_rejects_bad_crc(void) {
  std::vector<uint8_t> corrupted(PKT_TELEMETRY, PKT_TELEMETRY + PKT_TELEMETRY_len);
  corrupted[20] ^= 0xFF;
  Packet p;
  TEST_ASSERT_FALSE(Packet::fromBytes(corrupted.data(), corrupted.size(), p));
}

static void test_decode_v3_roundtrip(void) {
  Packet p;
  TEST_ASSERT_TRUE(Packet::fromBytes(PKT_V3, PKT_V3_len, p));
  TEST_ASSERT_EQUAL_HEX8(0x35, p.src);
  TEST_ASSERT_EQUAL_HEX8(0x86, p.cmd_id);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(1), (uint32_t)(p.payload.size()));
}

// --- Handshake framing ---

static void test_simple_packet_encode(void) {
  std::vector<uint8_t> payload = {0x01, 0x00};
  for (int i = 0; i < 40; i++) payload.push_back((uint8_t)i);
  expectBytes(SIMPLE_PUBKEY_FRAME, SIMPLE_PUBKEY_FRAME_len,
              simplePacketEncode(payload.data(), payload.size()));
}

static void test_simple_packet_parse_fragmented(void) {
  // handshake frames arrive split across BLE notifications
  SimplePacketParser parser;
  std::vector<uint8_t> out;
  size_t half = SIMPLE_PUBKEY_FRAME_len / 2;
  TEST_ASSERT_FALSE(parser.feed(SIMPLE_PUBKEY_FRAME, half, out));
  TEST_ASSERT_TRUE(parser.feed(SIMPLE_PUBKEY_FRAME + half,
                               SIMPLE_PUBKEY_FRAME_len - half, out));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(42), (uint32_t)(out.size()));  // 01 00 + 40-byte key
  TEST_ASSERT_EQUAL_HEX8(0x01, out[0]);
  TEST_ASSERT_EQUAL_HEX8(24, out[26]);  // pubkey byte 24 (vector is 0..39)
}

// --- Session key derivation ---

static void test_session_key(void) {
  uint8_t key[16];
  TEST_ASSERT_TRUE(gen_session_key(SESSION_SEED, SESSION_SRAND, key));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(SESSION_KEY, key, 16);
}

// --- Type 7 encrypted frames (full round trip vs reference) ---

static void test_t7_frame_encode(void) {
  Aes128Cbc cipher;
  cipher.setKey(T7_KEY, T7_IV, Aes128Cbc::PKCS7);
  FrameAssembler assembler(7, &cipher);

  Packet p(0x05, 0x21, 0x20, 0x02,
           std::vector<uint8_t>(PKT_TELEMETRY_PAYLOAD,
                                PKT_TELEMETRY_PAYLOAD + PKT_TELEMETRY_PAYLOAD_len),
           2);
  memcpy(p.seq, "\x11\x22\x33\x44", 4);
  expectBytes(T7_FRAME, T7_FRAME_len, assembler.encode(p));
}

static void test_t7_frame_reassemble_fragmented(void) {
  Aes128Cbc cipher;
  cipher.setKey(T7_KEY, T7_IV, Aes128Cbc::PKCS7);
  FrameAssembler assembler(7, &cipher);

  // drip-feed the encrypted frame in 20-byte chunks (default BLE MTU style)
  std::vector<std::vector<uint8_t>> packets;
  for (size_t off = 0; off < T7_FRAME_len; off += 20) {
    size_t n = T7_FRAME_len - off < 20 ? T7_FRAME_len - off : 20;
    assembler.feed(T7_FRAME + off, n, packets);
  }
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(1), (uint32_t)(packets.size()));

  Packet p;
  TEST_ASSERT_TRUE(Packet::fromBytes(packets[0].data(), packets[0].size(), p));
  TEST_ASSERT_EQUAL_HEX8(0x05, p.src);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(PKT_TELEMETRY_PAYLOAD, p.payload.data(),
                               PKT_TELEMETRY_PAYLOAD_len);
}

static void test_t7_two_frames_one_notification(void) {
  Aes128Cbc cipher;
  cipher.setKey(T7_KEY, T7_IV, Aes128Cbc::PKCS7);
  FrameAssembler assembler(7, &cipher);

  std::vector<uint8_t> doubled(T7_FRAME, T7_FRAME + T7_FRAME_len);
  doubled.insert(doubled.end(), T7_FRAME, T7_FRAME + T7_FRAME_len);
  std::vector<std::vector<uint8_t>> packets;
  assembler.feed(doubled.data(), doubled.size(), packets);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(2), (uint32_t)(packets.size()));
}

// --- Key info decryption (handshake step 3) ---

static void test_keyinfo_decrypt(void) {
  Aes128Cbc cipher;
  cipher.setKey(T7_KEY, T7_IV, Aes128Cbc::PKCS7);

  TEST_ASSERT_EQUAL_HEX8(0x02, KEYINFO_PAYLOAD[0]);
  std::vector<uint8_t> plain(KEYINFO_PAYLOAD_len);
  size_t n = cipher.decrypt(KEYINFO_PAYLOAD + 1, KEYINFO_PAYLOAD_len - 1,
                            plain.data());
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(18), (uint32_t)(n));  // sRand[16] + seed[2]
  TEST_ASSERT_EQUAL_HEX8_ARRAY(SESSION_SRAND, plain.data(), 16);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(SESSION_SEED, plain.data() + 16, 2);

  // and the derived session key matches the reference
  uint8_t key[16];
  TEST_ASSERT_TRUE(gen_session_key(plain.data() + 16, plain.data(), key));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(SESSION_KEY, key, 16);
}

// --- Telemetry parsing (offsets vs reference struct layout) ---

static void test_parse_mppt_heartbeat(void) {
  // Mr330MpptHeart: in_watts @16 (u16), car_state @56, cfg_ac_enabled @66
  std::vector<uint8_t> payload(94, 0);
  payload[16] = 0x2C;  // 300 W
  payload[17] = 0x01;
  payload[56] = 0x01;  // 12V on
  payload[66] = 0x00;  // AC off

  Packet p(0x05, 0x21, 0x20, 0x02, payload, 2);
  river2::DeviceState s;
  TEST_ASSERT_TRUE(river2::parseTelemetry(p, s, 1000));
  TEST_ASSERT_EQUAL_INT(300, s.dc_in_w);
  TEST_ASSERT_EQUAL_INT(1, s.dc_on);
  TEST_ASSERT_EQUAL_INT(0, s.ac_on);
  TEST_ASSERT_EQUAL_UINT32(1000, s.last_port_state_ms);
}

static void test_parse_bms_heartbeat(void) {
  // DirectBmsMDeltaHeartbeatPack: f32_show_soc @53, temp @20
  std::vector<uint8_t> payload(69, 0);
  float soc = 78.4f;
  memcpy(payload.data() + 53, &soc, 4);
  payload[20] = 27;

  Packet p(0x03, 0x00, 0x20, 0x32, payload, 2);
  river2::DeviceState s;
  TEST_ASSERT_TRUE(river2::parseTelemetry(p, s, 5));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 78.4f, s.battery_pct);
  TEST_ASSERT_EQUAL_INT(27, s.battery_temp_c);
}

static void test_parse_pd_heartbeat_truncated(void) {
  // Older firmware sends shorter packs: fields past the end stay unset
  std::vector<uint8_t> payload(19, 0);
  payload[15] = 0x8E;  // watts_out_sum = 142
  payload[17] = 0x0A;  // watts_in_sum = 10

  Packet p(0x02, 0x21, 0x20, 0x02, payload, 2);
  river2::DeviceState s;
  TEST_ASSERT_TRUE(river2::parseTelemetry(p, s, 5));
  TEST_ASSERT_EQUAL_INT(142, s.watts_out);
  TEST_ASSERT_EQUAL_INT(-1, s.dc12v_out_w);  // offset 34 not covered
}

static void test_parse_ignores_unknown(void) {
  Packet p(0x35, 0x21, 0x35, 0x86, {0x00}, 2);
  river2::DeviceState s;
  TEST_ASSERT_FALSE(river2::parseTelemetry(p, s, 5));
  TEST_ASSERT_EQUAL_UINT32(0, s.last_telemetry_ms);
}

static void test_sn_prefix_match(void) {
  TEST_ASSERT_TRUE(river2::matchesSerialNumber("R621ZAB1TEST0001"));
  TEST_ASSERT_TRUE(river2::matchesSerialNumber("R601AAAA00000001"));
  TEST_ASSERT_FALSE(river2::matchesSerialNumber("R351XXXX00000001"));  // Delta 2
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_crc);
  RUN_TEST(test_encode_ac_on);
  RUN_TEST(test_encode_dc_off);
  RUN_TEST(test_encode_auth_status);
  RUN_TEST(test_encode_auth_submit);
  RUN_TEST(test_encode_v3);
  RUN_TEST(test_decode_telemetry);
  RUN_TEST(test_decode_rejects_bad_crc);
  RUN_TEST(test_decode_v3_roundtrip);
  RUN_TEST(test_simple_packet_encode);
  RUN_TEST(test_simple_packet_parse_fragmented);
  RUN_TEST(test_session_key);
  RUN_TEST(test_t7_frame_encode);
  RUN_TEST(test_t7_frame_reassemble_fragmented);
  RUN_TEST(test_t7_two_frames_one_notification);
  RUN_TEST(test_keyinfo_decrypt);
  RUN_TEST(test_parse_mppt_heartbeat);
  RUN_TEST(test_parse_bms_heartbeat);
  RUN_TEST(test_parse_pd_heartbeat_truncated);
  RUN_TEST(test_parse_ignores_unknown);
  RUN_TEST(test_sn_prefix_match);
  return UNITY_END();
}

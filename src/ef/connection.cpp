#include "connection.h"

#include <Arduino.h>
#include <string.h>

namespace ef {

// EcoFlow's BLE manufacturer-data company id
static constexpr uint16_t MANUFACTURER_ID = 0xB5B5;

// Handshake constants (ha-ef-ble eflib/connection.py)
static constexpr uint8_t AUTH_DST = 0x35;
static constexpr uint8_t AUTH_CMD_SET = 0x35;
static constexpr uint8_t AUTH_CMD_STATUS = 0x89;
static constexpr uint8_t AUTH_CMD_SUBMIT = 0x86;
static constexpr uint8_t PACKET_VERSION = 2;  // River 2 series is V2

// GATT characteristic pairs the firmware may expose (either profile works)
struct CharPair {
  const char *write_uuid;
  const char *notify_uuid;
};
static const CharPair CHAR_PAIRS[] = {
    // "rfcomm" profile
    {"00000002-0000-1000-8000-00805f9b34fb",
     "00000003-0000-1000-8000-00805f9b34fb"},
    // Nordic UART profile
    {"6e400002-b5a3-f393-e0a9-e50e24dcca9e",
     "6e400003-b5a3-f393-e0a9-e50e24dcca9e"},
};

static constexpr uint32_t HANDSHAKE_STEP_TIMEOUT_MS = 8000;
static constexpr uint32_t BACKOFF_MIN_MS = 5000;
static constexpr uint32_t BACKOFF_MAX_MS = 30000;
// Force a reconnect when an authenticated link goes quiet for this long.
static constexpr uint32_t TELEMETRY_DEAD_MS = 20000;

const char *connStateName(ConnState s) {
  switch (s) {
    case ConnState::SCANNING: return "Scanning";
    case ConnState::CONNECTING: return "Connecting";
    case ConnState::EXCHANGE_PUBKEY:
    case ConnState::REQUEST_KEYINFO: return "Key exchange";
    case ConnState::REQUEST_AUTH_STATUS:
    case ConnState::AUTHENTICATING: return "Authenticating";
    case ConnState::AUTHENTICATED: return "Connected";
    case ConnState::BACKOFF: return "Reconnecting";
    case ConnState::AUTH_REJECTED: return "Auth rejected";
  }
  return "?";
}

// ---- NimBLE callback shims (host task) ----

static Connection *g_conn = nullptr;

namespace {

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice *dev) override {
    if (g_conn) g_conn->onScanResult(dev);
  }
};

class ClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *) override {
    if (g_conn) g_conn->onDisconnect();
  }
};

}  // namespace

static ScanCallbacks g_scan_callbacks;
static ClientCallbacks g_client_callbacks;

static void notifyShim(NimBLERemoteCharacteristic *, uint8_t *data,
                       size_t len, bool) {
  if (g_conn) g_conn->onNotify(data, len);
}

// ---- setup ----

void Connection::begin(const char *user_id, const char *sn_filter) {
  user_id_ = user_id;
  sn_filter_ = sn_filter;
  mutex_ = xSemaphoreCreateMutex();
  g_conn = this;

  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setMTU(517);

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&g_scan_callbacks, false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(80);

  startScan();
}

void Connection::startScan() {
  found_device_ = false;
  NimBLEDevice::getScan()->start(0, nullptr, false);  // 0 = until stopped
  scanning_ = true;
  state_ = ConnState::SCANNING;
}

// ---- host-task callbacks: enqueue only ----

void Connection::onScanResult(NimBLEAdvertisedDevice *dev) {
  // EcoFlow advertisement: manufacturer data for company 0xB5B5 carries
  // proto_version(1) + serial[16] + status + product_type + ... + caps[22]
  std::string mfg = dev->getManufacturerData();
  if (mfg.size() < 19) return;
  uint16_t company = (uint8_t)mfg[0] | ((uint8_t)mfg[1] << 8);
  if (company != MANUFACTURER_ID) return;

  const char *man_data = mfg.data() + 2;  // skip company id
  size_t man_len = mfg.size() - 2;
  if (man_len < 17) return;

  char sn[17] = {0};
  memcpy(sn, man_data + 1, 16);
  for (int i = 15; i >= 0 && (sn[i] == '\0'); i--) sn[i] = '\0';  // zero-pad

  if (!sn_filter_.empty()) {
    if (sn_filter_ != sn) return;
  } else if (!river2::matchesSerialNumber(sn)) {
    return;
  }

  // capability flags: byte 22 of manufacturer payload when present,
  // otherwise assume encrypt_type 7 (matches the reference default)
  int encrypt_type = 7;
  if (man_len > 22) {
    encrypt_type = ((uint8_t)man_data[22] & 0b0111000) >> 3;
  }

  if (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    if (!found_device_) {
      found_addr_ = dev->getAddress();
      found_sn_ = sn;
      found_encrypt_type_ = encrypt_type;
      found_device_ = true;
    }
    xSemaphoreGive(mutex_);
  }
}

void Connection::onNotify(const uint8_t *data, size_t len) {
  if (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    if (rx_queue_.size() < 64) {
      rx_queue_.emplace_back(data, data + len);
    }
    xSemaphoreGive(mutex_);
  }
}

void Connection::onDisconnect() { disconnected_flag_ = true; }

// ---- loop-task state machine ----

void Connection::tick(uint32_t now_ms) {
  if (disconnected_flag_ && state_ != ConnState::BACKOFF &&
      state_ != ConnState::SCANNING && state_ != ConnState::AUTH_REJECTED) {
    disconnected_flag_ = false;
    enterBackoff(now_ms, "BLE link dropped");
    return;
  }
  disconnected_flag_ = false;

  switch (state_) {
    case ConnState::SCANNING: {
      bool found = false;
      if (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
        found = found_device_;
        xSemaphoreGive(mutex_);
      }
      if (found) {
        NimBLEDevice::getScan()->stop();
        scanning_ = false;
        dev_sn_ = found_sn_;
        encrypt_type_ = found_encrypt_type_;
        Serial.printf("[ble] found %s (encrypt_type=%d) at %s\n",
                      dev_sn_.c_str(), encrypt_type_,
                      found_addr_.toString().c_str());
        setState(ConnState::CONNECTING, now_ms);
      }
      break;
    }

    case ConnState::CONNECTING:
      if (connectAndSubscribe()) {
        startHandshake(now_ms);
      } else {
        enterBackoff(now_ms, "connect failed (unit off, out of range, "
                             "or the EcoFlow app holds the connection)");
      }
      break;

    case ConnState::EXCHANGE_PUBKEY:
    case ConnState::REQUEST_KEYINFO:
    case ConnState::REQUEST_AUTH_STATUS:
    case ConnState::AUTHENTICATING:
      processIncoming(now_ms);
      if ((int32_t)(now_ms - state_deadline_ms_) > 0) {
        enterBackoff(now_ms, "handshake step timed out");
      }
      break;

    case ConnState::AUTHENTICATED:
      processIncoming(now_ms);
      if (device_state_.last_telemetry_ms != 0 &&
          now_ms - device_state_.last_telemetry_ms > TELEMETRY_DEAD_MS) {
        enterBackoff(now_ms, "telemetry stopped");
      }
      break;

    case ConnState::BACKOFF:
      if ((int32_t)(now_ms - backoff_until_ms_) > 0) {
        startScan();
      }
      break;

    case ConnState::AUTH_REJECTED:
      // Wrong user id / device bound to another account. Retrying won't fix
      // it, but keep trying slowly in case the user re-pairs via the app.
      if ((int32_t)(now_ms - backoff_until_ms_) > 0) {
        backoff_until_ms_ = now_ms + 60000;
        startScan();
      }
      break;
  }
}

void Connection::enterBackoff(uint32_t now_ms, const char *reason) {
  Serial.printf("[ble] disconnect: %s\n", reason);
  if (scanning_) {
    NimBLEDevice::getScan()->stop();
    scanning_ = false;
  }
  if (client_ != nullptr) {
    if (client_->isConnected()) client_->disconnect();
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
    write_char_ = nullptr;
    notify_char_ = nullptr;
  }
  if (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    rx_queue_.clear();
    xSemaphoreGive(mutex_);
  }
  simple_parser_.reset();
  assembler_.reset();

  backoff_interval_ms_ =
      backoff_interval_ms_ == 0
          ? BACKOFF_MIN_MS
          : min(backoff_interval_ms_ * 2, (uint32_t)BACKOFF_MAX_MS);
  backoff_until_ms_ = now_ms + backoff_interval_ms_;
  state_ = ConnState::BACKOFF;
}

bool Connection::connectAndSubscribe() {
  client_ = NimBLEDevice::createClient();
  client_->setClientCallbacks(&g_client_callbacks, false);
  client_->setConnectTimeout(10);

  if (!client_->connect(found_addr_)) {
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
    return false;
  }

  // Find a known write/notify characteristic pair in any service
  for (NimBLERemoteService *svc : *client_->getServices(true)) {
    for (const CharPair &pair : CHAR_PAIRS) {
      NimBLERemoteCharacteristic *w = svc->getCharacteristic(pair.write_uuid);
      NimBLERemoteCharacteristic *n = svc->getCharacteristic(pair.notify_uuid);
      if (w != nullptr && n != nullptr) {
        write_char_ = w;
        notify_char_ = n;
        break;
      }
    }
    if (write_char_ != nullptr) break;
  }

  if (write_char_ == nullptr || notify_char_ == nullptr ||
      !notify_char_->subscribe(true, notifyShim)) {
    Serial.println("[ble] expected characteristics not found");
    client_->disconnect();
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
    write_char_ = nullptr;
    notify_char_ = nullptr;
    return false;
  }

  return true;
}

void Connection::startHandshake(uint32_t now_ms) {
  assembler_ = FrameAssembler(encrypt_type_, &cipher_);

  if (encrypt_type_ == 0 || encrypt_type_ == 1) {
    // No key exchange: type 1 derives keys from the serial number, type 0 is
    // plaintext. Send the auth wake-up + credentials straight away.
    if (encrypt_type_ == 1) {
      uint8_t key[16], iv[16];
      std::string sn_rev(dev_sn_.rbegin(), dev_sn_.rend());
      md5((const uint8_t *)dev_sn_.data(), dev_sn_.size(), key);
      md5((const uint8_t *)sn_rev.data(), sn_rev.size(), iv);
      cipher_.setKey(key, iv, Aes128Cbc::ZERO);
    }
    sendAuthStatusRequest();
    sendAuthPayload();
    setState(ConnState::AUTHENTICATING, now_ms);
    return;
  }

  // Type 7: ECDH public key exchange first
  if (!ecdh_.generateKeypair()) {
    enterBackoff(now_ms, "ECDH keygen failed");
    return;
  }
  std::vector<uint8_t> payload;
  payload.push_back(0x01);  // exchange-public-key command
  payload.push_back(0x00);
  payload.insert(payload.end(), ecdh_.publicKey(),
                 ecdh_.publicKey() + EcdhSecp160r1::PUBLIC_KEY_SIZE);
  std::vector<uint8_t> frame = simplePacketEncode(payload.data(), payload.size());
  write_char_->writeValue(frame.data(), frame.size(), true);
  setState(ConnState::EXCHANGE_PUBKEY, now_ms);
}

void Connection::processIncoming(uint32_t now_ms) {
  std::vector<std::vector<uint8_t>> chunks;
  if (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    chunks.swap(rx_queue_);
    xSemaphoreGive(mutex_);
  }

  for (const std::vector<uint8_t> &chunk : chunks) {
    if (state_ == ConnState::EXCHANGE_PUBKEY ||
        state_ == ConnState::REQUEST_KEYINFO) {
      handleHandshakeData(chunk, now_ms);
    } else {
      handlePacketData(chunk, now_ms);
    }
  }
}

void Connection::handleHandshakeData(const std::vector<uint8_t> &data,
                                     uint32_t now_ms) {
  std::vector<uint8_t> payload;
  if (!simple_parser_.feed(data.data(), data.size(), payload)) {
    return;  // frame incomplete, wait for the next notification
  }

  if (state_ == ConnState::EXCHANGE_PUBKEY) {
    // payload: [cmd, status, curve_type, pubkey...] — SECP160r1 keys are 40B
    if (payload.size() < 3 + EcdhSecp160r1::PUBLIC_KEY_SIZE) {
      enterBackoff(now_ms, "public key response too short");
      return;
    }
    uint8_t shared[EcdhSecp160r1::SHARED_SECRET_SIZE];
    if (!ecdh_.computeSharedSecret(payload.data() + 3, shared)) {
      enterBackoff(now_ms, "invalid device public key");
      return;
    }
    md5(shared, sizeof(shared), iv_);
    cipher_.setKey(shared, iv_, Aes128Cbc::PKCS7);  // key = shared[:16]

    uint8_t cmd = 0x02;  // request key info
    std::vector<uint8_t> frame = simplePacketEncode(&cmd, 1);
    write_char_->writeValue(frame.data(), frame.size(), true);
    setState(ConnState::REQUEST_KEYINFO, now_ms);
    return;
  }

  // REQUEST_KEYINFO: [0x02, AES(sRand[16] + seed[2])]
  if (payload.empty() || payload[0] != 0x02) {
    enterBackoff(now_ms, "unexpected key info response");
    return;
  }
  std::vector<uint8_t> plain(payload.size());
  size_t n = cipher_.decrypt(payload.data() + 1, payload.size() - 1, plain.data());
  if (n < 18) {
    enterBackoff(now_ms, "key info payload too short");
    return;
  }
  uint8_t session_key[16];
  if (!gen_session_key(plain.data() + 16, plain.data(), session_key)) {
    enterBackoff(now_ms, "session key seed out of range");
    return;
  }
  cipher_.setKey(session_key, iv_, Aes128Cbc::PKCS7);

  sendAuthStatusRequest();
  setState(ConnState::REQUEST_AUTH_STATUS, now_ms);
}

void Connection::handlePacketData(const std::vector<uint8_t> &data,
                                  uint32_t now_ms) {
  std::vector<std::vector<uint8_t>> raw_packets;
  assembler_.feed(data.data(), data.size(), raw_packets);

  for (const std::vector<uint8_t> &raw : raw_packets) {
    Packet packet;
    if (!Packet::fromBytes(raw.data(), raw.size(), packet)) {
      continue;
    }
    handlePacket(packet, now_ms);
  }
}

void Connection::handlePacket(const Packet &packet, uint32_t now_ms) {
  if (state_ == ConnState::REQUEST_AUTH_STATUS) {
    // Reference ignores the status payload — receiving anything moves on
    sendAuthPayload();
    setState(ConnState::AUTHENTICATING, now_ms);
    return;
  }

  if (state_ == ConnState::AUTHENTICATING) {
    bool is_auth_reply = packet.src == AUTH_DST &&
                         packet.cmd_set == AUTH_CMD_SET &&
                         packet.cmd_id == AUTH_CMD_SUBMIT;
    if (is_auth_reply) {
      if (packet.payload.size() == 1 && packet.payload[0] == 0x00) {
        Serial.println("[ble] authenticated");
        backoff_interval_ms_ = 0;
        setState(ConnState::AUTHENTICATED, now_ms);
      } else {
        // 0x01..0x07: refresh-token / already-bound / wrong-key / ... —
        // credentials are wrong for this device, backoff won't cure it
        Serial.printf("[ble] auth rejected, code %s\n",
                      packet.payload.empty()
                          ? "<empty>"
                          : String(packet.payload[0]).c_str());
        if (client_ != nullptr && client_->isConnected()) client_->disconnect();
        backoff_until_ms_ = now_ms + 60000;
        state_ = ConnState::AUTH_REJECTED;
      }
      return;
    }
    // Data before the auth ack also means we're in (matches the reference)
    Serial.println("[ble] authenticated (data flowing)");
    backoff_interval_ms_ = 0;
    setState(ConnState::AUTHENTICATED, now_ms);
    river2::parseTelemetry(packet, device_state_, now_ms);
    return;
  }

  if (state_ == ConnState::AUTHENTICATED) {
    river2::parseTelemetry(packet, device_state_, now_ms);
  }
}

void Connection::sendAuthStatusRequest() {
  sendPacket(Packet(0x21, AUTH_DST, AUTH_CMD_SET, AUTH_CMD_STATUS, {},
                    PACKET_VERSION));
}

void Connection::sendAuthPayload() {
  // MD5(user_id + serial) rendered as 32 uppercase hex chars
  std::string material = user_id_ + dev_sn_;
  uint8_t digest[16];
  md5((const uint8_t *)material.data(), material.size(), digest);

  std::vector<uint8_t> payload(32);
  static const char HEX_CHARS[] = "0123456789ABCDEF";  // HEX is an Arduino macro
  for (int i = 0; i < 16; i++) {
    payload[i * 2] = HEX_CHARS[digest[i] >> 4];
    payload[i * 2 + 1] = HEX_CHARS[digest[i] & 0x0F];
  }
  sendPacket(Packet(0x21, AUTH_DST, AUTH_CMD_SET, AUTH_CMD_SUBMIT,
                    std::move(payload), PACKET_VERSION));
}

bool Connection::sendPacket(const Packet &packet) {
  if (client_ == nullptr || !client_->isConnected() || write_char_ == nullptr) {
    return false;
  }
  std::vector<uint8_t> frame = assembler_.encode(packet);
  return write_char_->writeValue(frame.data(), frame.size(),
                                 assembler_.writeWithResponse());
}

bool Connection::sendAcToggle(bool on) {
  if (state_ != ConnState::AUTHENTICATED) return false;
  return sendPacket(river2::buildAcToggle(on));
}

bool Connection::sendDcToggle(bool on) {
  if (state_ != ConnState::AUTHENTICATED) return false;
  return sendPacket(river2::buildDcToggle(on));
}

uint32_t Connection::telemetryAge(uint32_t now_ms) const {
  if (device_state_.last_telemetry_ms == 0) return UINT32_MAX;
  return now_ms - device_state_.last_telemetry_ms;
}

void Connection::setState(ConnState s, uint32_t now_ms) {
  state_ = s;
  state_deadline_ms_ = now_ms + HANDSHAKE_STEP_TIMEOUT_MS;
}

}  // namespace ef

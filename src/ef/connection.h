// BLE connection manager: scanning, GATT setup, the EcoFlow auth handshake,
// telemetry dispatch and reconnect-with-backoff. Ported from ha-ef-ble
// eflib/connection.py (see PROTOCOL.md for the handshake description).
//
// Threading: NimBLE callbacks (scan results, notifications, disconnects) run
// on the NimBLE host task and only enqueue data behind a mutex. All protocol
// work happens in tick(), called from the Arduino loop task — which is also
// the only place DeviceState is written, so the UI can read it freely from
// the same task.
#pragma once

#include <NimBLEDevice.h>

#include <string>
#include <vector>

#include "crypto.h"
#include "packet.h"
#include "river2.h"

namespace ef {

enum class ConnState {
  SCANNING,
  CONNECTING,
  EXCHANGE_PUBKEY,     // sent our ECDH public key, waiting for the device's
  REQUEST_KEYINFO,     // waiting for encrypted sRand+seed
  REQUEST_AUTH_STATUS, // sent 0x35/0x89, waiting for any reply
  AUTHENTICATING,      // sent 0x35/0x86 (MD5 of user_id+SN), waiting for ack
  AUTHENTICATED,       // telemetry flowing
  BACKOFF,             // disconnected; waiting before the next attempt
  AUTH_REJECTED,       // device refused our credentials — no auto-retry cure
};

const char *connStateName(ConnState s);

class Connection {
 public:
  // user_id: numeric EcoFlow account id (see tools/get_user_id.py).
  // sn_filter: full serial number, or "" to take any River 2 series device.
  void begin(const char *user_id, const char *sn_filter);

  // Drive the state machine. Call every loop iteration.
  void tick(uint32_t now_ms);

  ConnState state() const { return state_; }
  const river2::DeviceState &deviceState() const { return device_state_; }
  const std::string &deviceSn() const { return dev_sn_; }
  // Age of the last telemetry in ms, or UINT32_MAX before the first packet.
  uint32_t telemetryAge(uint32_t now_ms) const;

  // Send an output-toggle command. Returns false when not authenticated.
  bool sendAcToggle(bool on);
  bool sendDcToggle(bool on);

  // --- NimBLE callback entry points (host task; do minimal work) ---
  void onScanResult(NimBLEAdvertisedDevice *dev);
  void onNotify(const uint8_t *data, size_t len);
  void onDisconnect();

 private:
  void startScan();
  void enterBackoff(uint32_t now_ms, const char *reason);
  bool connectAndSubscribe();
  void startHandshake(uint32_t now_ms);
  void processIncoming(uint32_t now_ms);
  void handleHandshakeData(const std::vector<uint8_t> &data, uint32_t now_ms);
  void handlePacketData(const std::vector<uint8_t> &data, uint32_t now_ms);
  void handlePacket(const Packet &packet, uint32_t now_ms);
  void sendAuthStatusRequest();
  void sendAuthPayload();
  bool sendPacket(const Packet &packet);
  void setState(ConnState s, uint32_t now_ms);

  // config
  std::string user_id_;
  std::string sn_filter_;

  // scan result (written by host task under mutex_, consumed in tick)
  bool found_device_ = false;
  NimBLEAddress found_addr_;
  std::string found_sn_;
  int found_encrypt_type_ = 7;

  // notification queue (host task -> loop task)
  SemaphoreHandle_t mutex_ = nullptr;
  std::vector<std::vector<uint8_t>> rx_queue_;
  volatile bool disconnected_flag_ = false;

  // BLE handles (loop task only)
  NimBLEClient *client_ = nullptr;
  NimBLERemoteCharacteristic *write_char_ = nullptr;
  NimBLERemoteCharacteristic *notify_char_ = nullptr;

  // protocol state (loop task only)
  ConnState state_ = ConnState::SCANNING;
  std::string dev_sn_;
  int encrypt_type_ = 7;
  EcdhSecp160r1 ecdh_;
  Aes128Cbc cipher_;
  uint8_t iv_[16];
  SimplePacketParser simple_parser_;
  FrameAssembler assembler_{7, &cipher_};
  river2::DeviceState device_state_;

  // timing
  uint32_t state_deadline_ms_ = 0;    // handshake step timeout
  uint32_t backoff_until_ms_ = 0;
  uint32_t backoff_interval_ms_ = 0;
  bool scanning_ = false;
};

}  // namespace ef

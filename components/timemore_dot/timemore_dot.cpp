#include "timemore_dot.h"
#include "esphome/core/log.h"

namespace esphome {
namespace timemore_dot {

static const char *const TAG = "timemore_dot";

// Captured from a real session against a live Dot scale (see HANDOFF.md,
// "BLE protocol" section). The CRC trailer bytes are hardcoded constants,
// NOT computed -- the reference driver this was ported from
// (gaggimate/esp-arduino-ble-scales, src/scales/dot.cpp) doesn't compute or
// verify a CRC either, it just replays these exact captured bytes.
static const uint8_t TARE_FRAME[] = {0xA5, 0x5A, 0x02, 0x04, 0x00, 0x00, 0x9A, 0x00};
static const uint8_t TARE_FOLLOWUP_FRAME[] = {0xA5, 0x5A, 0x03, 0x0D, 0x00, 0x00, 0x64, 0xD1};

// Reconnect backoff -- avoid hammering a scale that's simply out of range
// or asleep.
static const uint32_t RECONNECT_INTERVAL_MS = 5000;

class TimemoreDotClientCallbacks : public NimBLEClientCallbacks {
 public:
  explicit TimemoreDotClientCallbacks(TimemoreDot *parent) : parent_(parent) {}
  void onConnect(NimBLEClient *client) override { parent_->on_connect(); }
  void onDisconnect(NimBLEClient *client, int reason) override { parent_->on_disconnect(); }

 protected:
  TimemoreDot *parent_;
};

class TimemoreDotScanCallbacks : public NimBLEScanCallbacks {
 public:
  explicit TimemoreDotScanCallbacks(TimemoreDot *parent) : parent_(parent) {}
  void onResult(NimBLEAdvertisedDevice *device) override { parent_->on_scan_result(device); }

 protected:
  TimemoreDot *parent_;
};

void TimemoreDot::setup() {
  NimBLEDevice::init("");

  // "Just works" bonding: the scale has no display/keyboard to confirm a
  // passkey against, and the reference driver doesn't do passkey entry
  // either. Bonding (not just an ad-hoc encrypted link) is required --
  // the scale will not emit weight notifications on an unbonded/insecure
  // link (see HANDOFF.md, "Critical constraint").
  NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/false, /*secure_connections=*/true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  // Owned for the process lifetime -- NimBLE keeps raw pointers to these
  // and there's only ever one TimemoreDot instance (a single ESP32 has one
  // BLE radio).
  client_callbacks_ = new TimemoreDotClientCallbacks(this);
  scan_callbacks_ = new TimemoreDotScanCallbacks(this);

  start_scan_();
}

void TimemoreDot::start_scan_() {
  ESP_LOGD(TAG, "Scanning for a device advertising name prefix '%s'", DEVICE_NAME_PREFIX);
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(scan_callbacks_, false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(100);
  scanning_ = true;
  marked_for_reconnect_ = false;
  scan->start(0 /* duration: scan until we stop it ourselves */, false);
}

void TimemoreDot::on_scan_result(NimBLEAdvertisedDevice *device) {
  if (!device->haveName() || device->getName().rfind(DEVICE_NAME_PREFIX, 0) != 0)
    return;

  ESP_LOGI(TAG, "Found %s (%s), connecting", device->getName().c_str(), device->getAddress().toString().c_str());
  NimBLEDevice::getScan()->stop();
  scanning_ = false;
  connect_(device);
}

void TimemoreDot::connect_(NimBLEAdvertisedDevice *device) {
  target_address_ = device->getAddress().toString();

  if (client_ == nullptr) {
    client_ = NimBLEDevice::createClient();
    client_->setClientCallbacks(client_callbacks_, false);
  }

  if (!client_->connect(device)) {
    ESP_LOGW(TAG, "Connect to %s failed, will retry", target_address_.c_str());
    marked_for_reconnect_ = true;
    return;
  }

  // This is the step the reference driver treats as load-bearing: it checks
  // secureConnection() after connect and aborts (disconnects) if the link
  // isn't secure, because the scale silently withholds weight notifications
  // otherwise rather than returning an error.
  if (!client_->secureConnection()) {
    ESP_LOGW(TAG, "Bonding/secure connection to %s failed, disconnecting and retrying", target_address_.c_str());
    client_->disconnect();
    marked_for_reconnect_ = true;
    return;
  }

  NimBLERemoteService *service = client_->getService(SERVICE_UUID);
  if (service == nullptr) {
    ESP_LOGE(TAG, "Service %s not found on %s", SERVICE_UUID, target_address_.c_str());
    client_->disconnect();
    marked_for_reconnect_ = true;
    return;
  }

  NimBLERemoteCharacteristic *notify_char = service->getCharacteristic(NOTIFY_CHAR_UUID);
  write_char_ = service->getCharacteristic(WRITE_CHAR_UUID);
  if (notify_char == nullptr || write_char_ == nullptr) {
    ESP_LOGE(TAG, "Characteristics %s/%s not found on %s", NOTIFY_CHAR_UUID, WRITE_CHAR_UUID,
              target_address_.c_str());
    client_->disconnect();
    marked_for_reconnect_ = true;
    return;
  }

  notify_char->subscribe(
      true, [this](NimBLERemoteCharacteristic *c, uint8_t *data, size_t length, bool is_notify) {
        this->on_notify(data, length);
      });
}

void TimemoreDot::on_connect() { ESP_LOGI(TAG, "Connected to %s", target_address_.c_str()); }

void TimemoreDot::on_disconnect() {
  ESP_LOGW(TAG, "Disconnected from %s", target_address_.c_str());
  connected_ = false;
  write_char_ = nullptr;
  rx_buffer_.clear();
  if (connected_sensor_ != nullptr)
    connected_sensor_->publish_state(false);
  marked_for_reconnect_ = true;
}

void TimemoreDot::loop() {
  if (marked_for_reconnect_ && !scanning_ && millis() - last_reconnect_attempt_ > RECONNECT_INTERVAL_MS) {
    last_reconnect_attempt_ = millis();
    start_scan_();
  }
}

void TimemoreDot::on_notify(const uint8_t *data, size_t length) {
  // A secured link is necessary but, per the reference driver's observed
  // behavior, not by itself proof notifications are flowing -- so
  // "connected" (for the UI/HA binary_sensor) is defined as "received at
  // least one good frame", not just "BLE link established".
  if (!connected_) {
    connected_ = true;
    if (connected_sensor_ != nullptr)
      connected_sensor_->publish_state(true);
  }

  rx_buffer_.insert(rx_buffer_.end(), data, data + length);

  // Frame: A5 5A [class] [type] [len_hi len_lo] [payload...] [crc_hi crc_lo]
  // total length = payload len + 8. CRC bytes are not verified here, same
  // as the reference driver -- see the CRC comment above TARE_FRAME.
  while (rx_buffer_.size() >= 8) {
    if (rx_buffer_[0] != 0xA5 || rx_buffer_[1] != 0x5A) {
      // Lost sync (e.g. after a dropped/partial notification) -- drop one
      // byte and try again rather than discarding the whole buffer.
      rx_buffer_.erase(rx_buffer_.begin());
      continue;
    }

    uint16_t payload_len = (static_cast<uint16_t>(rx_buffer_[4]) << 8) | rx_buffer_[5];
    size_t frame_len = static_cast<size_t>(payload_len) + 8;
    if (rx_buffer_.size() < frame_len)
      break;  // rest of the frame hasn't arrived yet

    uint8_t frame_class = rx_buffer_[2];
    uint8_t frame_type = rx_buffer_[3];
    handle_frame_(&rx_buffer_[6], payload_len, frame_class, frame_type);

    rx_buffer_.erase(rx_buffer_.begin(), rx_buffer_.begin() + frame_len);
  }
}

void TimemoreDot::handle_frame_(const uint8_t *payload, size_t payload_len, uint8_t frame_class, uint8_t frame_type) {
  if (frame_class == 0x01 && frame_type == 0x01 && payload_len >= 4) {
    // Signed big-endian int32, grams * 10. Bytes [4..8] of the 9-byte
    // payload (flow/secondary metric per the reference driver's comment)
    // are intentionally ignored -- unconfirmed, see HANDOFF.md.
    int32_t raw = (static_cast<int32_t>(payload[0]) << 24) | (static_cast<int32_t>(payload[1]) << 16) |
                  (static_cast<int32_t>(payload[2]) << 8) | static_cast<int32_t>(payload[3]);
    float grams = raw / 10.0f;
    if (weight_sensor_ != nullptr)
      weight_sensor_->publish_state(grams);
  } else if (frame_class == 0x01 && frame_type == 0x05 && payload_len >= 1) {
    if (battery_sensor_ != nullptr)
      battery_sensor_->publish_state(payload[0]);
  } else {
    ESP_LOGV(TAG, "Unhandled frame class=0x%02X type=0x%02X len=%u", frame_class, frame_type,
             static_cast<unsigned>(payload_len));
  }
}

bool TimemoreDot::write_frame_(const uint8_t *data, size_t length) {
  if (write_char_ == nullptr) {
    ESP_LOGW(TAG, "Not connected, dropping write");
    return false;
  }
  return write_char_->writeValue(data, length, false);
}

void TimemoreDot::tare() {
  ESP_LOGD(TAG, "Tare requested");
  write_frame_(TARE_FRAME, sizeof(TARE_FRAME));
  // The scale doesn't actually zero until this second, distinct frame lands
  // as a follow-up -- see HANDOFF.md, "BLE protocol" section.
  write_frame_(TARE_FOLLOWUP_FRAME, sizeof(TARE_FOLLOWUP_FRAME));
}

void TimemoreDot::dump_config() {
  ESP_LOGCONFIG(TAG, "Timemore Dot BLE scale:");
  ESP_LOGCONFIG(TAG, "  Target device name prefix: %s", DEVICE_NAME_PREFIX);
}

}  // namespace timemore_dot
}  // namespace esphome

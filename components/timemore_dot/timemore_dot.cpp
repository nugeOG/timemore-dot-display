#include "timemore_dot.h"
#include "esphome/core/log.h"

#include <esp32-hal-bt.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>

// Root cause of esp_bt_controller_init() failing with ESP_ERR_INVALID_STATE
// (confirmed via esp-idf v5.5's components/bt/controller/esp32/bt.c: that
// error is returned immediately, before the controller status is even
// checked, if the BLE controller's DRAM region was already released) --
// arduino-esp32's initArduino(), which runs before any ESPHome component
// code at all, frees that DRAM back to the heap at boot unless a strong
// definition of bleInUse()/btInUse() overrides its weak default (which
// just returns false). This project's earlier reliance on calling
// btStarted() at runtime (see start_ble_stack_()) turned out to be from an
// older arduino-esp32 core generation's opt-out mechanism and no longer
// works against the core version this ESP-IDF/ESPHome combination pulls
// in -- these are the two mechanisms (old and current) it actually checks,
// as plain strong C symbols so they link regardless of which one a given
// arduino-esp32 core version actually declares weak.
extern "C" bool btInUse(void) { return true; }
extern "C" bool bleInUse(void) { return true; }

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
  void onResult(const NimBLEAdvertisedDevice *device) override { parent_->on_scan_result(device); }

 protected:
  TimemoreDot *parent_;
};

void TimemoreDot::setup() {
  // Deliberately does nothing heavy here -- see start_ble_stack_()'s
  // comment in the header for why. loop() decides when to actually start
  // the BLE stack, once wifi is confirmed connected rather than just past
  // its own setup().
  ESP_LOGI(TAG, "TimemoreDot::setup() entered, deferring BLE start until wifi connects");
}

void TimemoreDot::start_ble_stack_() {
  ESP_LOGI(TAG, "Starting BLE stack");

  // btStarted() is Arduino's own "is BLE in use" check (esp32-hal-bt.c).
  // Calling it here forces that translation unit to link -- without any
  // reference to it, Arduino's initArduino() (which runs before any
  // ESPHome component code) sees no strong definition claiming BLE is
  // wanted and calls esp_bt_controller_mem_release(ESP_BT_MODE_BTDM),
  // freeing the BLE controller's RAM entirely before we ever get here.
  // NimBLEDevice::init() then fails every one of its esp_bt_controller_*
  // calls permanently for the rest of this boot (returns false, no crash --
  // this component's own error handling below is what caught that on the
  // previous real-hardware test). Confirmed via NimBLE-Arduino's own
  // source (src/NimBLEDevice.cpp) that init() checks esp_err_t from
  // nvs_flash_init()/esp_bt_controller_init()/enable()/esp_nimble_hci_init()
  // and only returns false on those specific failures.
  ESP_LOGI(TAG, "btStarted()=%d controller_status=%d (0=IDLE 1=INITED 2=ENABLED)", (int) btStarted(),
           (int) esp_bt_controller_get_status());
  ESP_LOGI(TAG, "free heap=%u internal=%u largest_internal_block=%u", (unsigned) esp_get_free_heap_size(),
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  bool ok = NimBLEDevice::init("");
  ESP_LOGI(TAG, "NimBLEDevice::init() returned %s", ok ? "true" : "false");
  if (!ok) {
    // Previously ignored this return value entirely and fell through to
    // configure security / start scanning against whatever partial state
    // init() left behind -- if init() fails partway through (bad sdkconfig,
    // controller/radio contention with wifi, etc.) that's a very plausible
    // way to crash deep in NimBLE's own code with no log of our own to
    // show for it. Bail out instead; loop() will never get a working scale
    // connection, but at least it won't crash-loop the whole board.
    ESP_LOGE(TAG, "NimBLEDevice::init() failed -- scale connection will not work this boot");
    return;
  }

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
  // interval == window was a 100% BLE scan duty cycle, leaving wifi no gaps
  // to use the shared radio -- the reference driver uses 500/100 (a 20%
  // duty cycle) instead, which is friendlier to wifi/BLE coexistence.
  scan->setInterval(500);
  scan->setWindow(100);

  // scan->start() can fail (observed on real hardware: "Unable to scan -
  // connection in progress", when a just-timed-out connect attempt's
  // client object hadn't finished its own async teardown yet, racing this
  // immediate retry). Previously scanning_ was set true unconditionally
  // here regardless of whether start() actually succeeded -- if it failed,
  // the component was then permanently stuck: loop()'s reconnect check
  // requires !scanning_, nothing else ever clears scanning_ back to false,
  // and on_scan_result()/on_disconnect() (the only other place that
  // touches it) never fire because no scan is actually running. That
  // silently stopped all reconnect attempts for the rest of the boot.
  bool started = scan->start(0 /* duration: scan until we stop it ourselves */, false);
  if (!started) {
    ESP_LOGW(TAG, "scan->start() failed, will retry");
    scanning_ = false;
    mark_for_reconnect_();
    return;
  }
  scanning_ = true;
  marked_for_reconnect_ = false;
}

void TimemoreDot::mark_for_reconnect_() {
  marked_for_reconnect_ = true;
  last_reconnect_attempt_ = millis();
}

void TimemoreDot::on_scan_result(const NimBLEAdvertisedDevice *device) {
  if (!device->haveName() || device->getName().rfind(DEVICE_NAME_PREFIX, 0) != 0)
    return;

  ESP_LOGI(TAG, "Found %s (%s), will connect from loop()", device->getName().c_str(),
           device->getAddress().toString().c_str());
  pending_address_ = device->getAddress();
  have_pending_connect_ = true;
}

void TimemoreDot::connect_(const NimBLEAddress &address) {
  target_address_ = address.toString();

  // A fresh client per attempt, not a reused one -- matches the reference
  // driver and avoids retrying against a client object left in a bad state
  // by the previous attempt's failure.
  bool ok = false;
  for (int attempt = 0; attempt < 3 && !ok; attempt++) {
    if (client_ != nullptr) {
      NimBLEDevice::deleteClient(client_);
      client_ = nullptr;
    }
    client_ = NimBLEDevice::createClient(address);
    client_->setClientCallbacks(client_callbacks_, false);
    ok = client_->connect();
    if (!ok && attempt < 2) {
      ESP_LOGW(TAG, "Connect attempt %d to %s failed, retrying", attempt + 1, target_address_.c_str());
      delay(500);
    }
  }

  if (!ok) {
    ESP_LOGW(TAG, "Connect to %s failed after 3 attempts, will retry later", target_address_.c_str());
    mark_for_reconnect_();
    return;
  }

  // This is the step the reference driver treats as load-bearing: it checks
  // secureConnection() after connect and aborts (disconnects) if the link
  // isn't secure, because the scale silently withholds weight notifications
  // otherwise rather than returning an error.
  if (!client_->secureConnection()) {
    ESP_LOGW(TAG, "Bonding/secure connection to %s failed, disconnecting and retrying", target_address_.c_str());
    client_->disconnect();
    mark_for_reconnect_();
    return;
  }

  NimBLERemoteService *service = client_->getService(SERVICE_UUID);
  if (service == nullptr) {
    ESP_LOGE(TAG, "Service %s not found on %s", SERVICE_UUID, target_address_.c_str());
    client_->disconnect();
    mark_for_reconnect_();
    return;
  }

  NimBLERemoteCharacteristic *notify_char = service->getCharacteristic(NOTIFY_CHAR_UUID);
  write_char_ = service->getCharacteristic(WRITE_CHAR_UUID);
  if (notify_char == nullptr || write_char_ == nullptr) {
    ESP_LOGE(TAG, "Characteristics %s/%s not found on %s", NOTIFY_CHAR_UUID, WRITE_CHAR_UUID,
              target_address_.c_str());
    client_->disconnect();
    mark_for_reconnect_();
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
  mark_for_reconnect_();
}

void TimemoreDot::loop() {
  if (!ble_started_) {
    // wifi::global_wifi_component is always non-null once the wifi
    // component has been set up (guaranteed here since this component's
    // own setup_priority is AFTER_WIFI); is_connected() specifically means
    // actually associated, not just "wifi's own setup() has returned" --
    // the distinction that matters, see the header comment.
    if (wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected()) {
      ble_started_ = true;
      start_ble_stack_();
    }
    return;
  }

  // Checked before the reconnect backoff below: a pending connect target
  // should be acted on immediately, not made to wait out
  // RECONNECT_INTERVAL_MS like a plain retry. See on_scan_result()'s
  // comment for why the connect itself has to happen here (ESPHome's own
  // task) rather than in that NimBLE scan callback.
  if (have_pending_connect_) {
    have_pending_connect_ = false;

    // A real boot hit this: the scan callback can queue one more result
    // (from just before scan->stop() actually took effect) that only gets
    // processed after a connection has already succeeded -- without this
    // check, that stale match would tear down a perfectly healthy
    // connection to "reconnect" to the exact same device.
    if (client_ != nullptr && client_->isConnected()) {
      ESP_LOGD(TAG, "Ignoring stale scan match, already connected");
      return;
    }

    NimBLEDevice::getScan()->stop();
    scanning_ = false;
    connect_(pending_address_);
    return;
  }

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

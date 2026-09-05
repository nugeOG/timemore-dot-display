#pragma once

// ============================================================================
// Custom ESPHome external component for the TIMEMORE Black Mirror Dot BLE
// scale. Owns the NimBLE stack exclusively (no esp32_ble_tracker/ble_client
// in YAML) so it has full control over forcing a secure/bonded connection
// with retry logic -- see HANDOFF.md, "Architecture decision" section, for
// why this couldn't be done with ESPHome's declarative BLE components.
//
// Ported from gaggimate/esp-arduino-ble-scales, src/scales/dot.h / dot.cpp
// (MIT-licensed). That repo is the authoritative source for the protocol --
// re-check it if anything here seems to not match your actual scale.
//
// NOT YET BUILT OR TESTED ON REAL HARDWARE. Written against the NimBLE-
// Arduino ~1.4.x API surface (see __init__.py's pinned library version) --
// verify class/method names (setScanCallbacks, secureConnection, the
// subscribe() callback signature, writeValue's return type) against
// whatever version actually gets pulled in when you build this, since
// NimBLE-Arduino's API has changed across major versions.
// ============================================================================

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"

#include <NimBLEDevice.h>

#include <vector>
#include <string>

namespace esphome {
namespace timemore_dot {

static const char *const SERVICE_UUID = "FFF0";
static const char *const NOTIFY_CHAR_UUID = "FFF1";
static const char *const WRITE_CHAR_UUID = "FFF2";
// Device advertises with a name starting with this prefix (confirmed from
// the reference driver) -- we connect to the first matching advertisement
// seen, there's no support here for picking a specific scale by MAC if you
// have more than one Dot nearby.
static const char *const DEVICE_NAME_PREFIX = "TIMEMORE_Dot";

class TimemoreDot : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // After wifi so an early BLE scan doesn't compete with wifi's own radio
  // setup; NimBLE and wifi share the ESP32's single 2.4GHz radio via
  // time-slicing regardless, this just orders init.
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_weight_sensor(sensor::Sensor *s) { weight_sensor_ = s; }
  void set_battery_sensor(sensor::Sensor *s) { battery_sensor_ = s; }
  void set_connected_sensor(binary_sensor::BinarySensor *s) { connected_sensor_ = s; }

  // Writes the tare command frame, then the handshake/poll follow-up frame
  // -- the scale doesn't actually zero until that second write lands (see
  // HANDOFF.md, "BLE protocol" section).
  void tare();

  // Called from the NimBLE scan/client callbacks defined in the .cpp --
  // public because those callback classes aren't members of TimemoreDot.
  void on_scan_result(NimBLEAdvertisedDevice *device);
  void on_connect();
  void on_disconnect();
  void on_notify(const uint8_t *data, size_t length);

 protected:
  void start_scan_();
  void connect_(NimBLEAdvertisedDevice *device);
  bool write_frame_(const uint8_t *data, size_t length);
  void handle_frame_(const uint8_t *payload, size_t payload_len, uint8_t frame_class, uint8_t frame_type);

  NimBLEClient *client_{nullptr};
  NimBLERemoteCharacteristic *write_char_{nullptr};
  // Owned for the process lifetime; declared as the base callback types so
  // this header doesn't need to know about the concrete callback classes
  // defined in timemore_dot.cpp.
  NimBLEClientCallbacks *client_callbacks_{nullptr};
  NimBLEScanCallbacks *scan_callbacks_{nullptr};
  bool connected_{false};
  bool scanning_{false};
  // Mirrors the reference driver's "markedForReconnection" retry pattern --
  // set on any connect failure or disconnect, cleared once a new scan
  // actually starts. loop() polls this instead of reconnecting inline from
  // a BLE callback, since NimBLE callbacks run on their own task/stack.
  bool marked_for_reconnect_{false};
  uint32_t last_reconnect_attempt_{0};
  std::string target_address_;

  sensor::Sensor *weight_sensor_{nullptr};
  sensor::Sensor *battery_sensor_{nullptr};
  binary_sensor::BinarySensor *connected_sensor_{nullptr};

  // Reassembly buffer -- a single NimBLE notification isn't guaranteed to
  // land as exactly one protocol frame, so we buffer and slice frames out
  // by their length field rather than assuming a 1:1 mapping.
  std::vector<uint8_t> rx_buffer_;
};

// Exposes tare() as a regular ESPHome button entity (platform: timemore_dot
// under button:) so it's callable from Home Assistant / the API, not just
// the on-device touchscreen -- part of the "fan data/control out over the
// network" decision in HANDOFF.md.
class TareButton : public button::Button, public Component {
 public:
  void set_parent(TimemoreDot *parent) { parent_ = parent; }

 protected:
  void press_action() override { parent_->tare(); }
  TimemoreDot *parent_{nullptr};
};

}  // namespace timemore_dot
}  // namespace esphome

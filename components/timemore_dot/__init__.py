import esphome.codegen as cg
from esphome.components.esp32 import (
    add_idf_sdkconfig_option,
    request_bluetooth,
    request_software_coexistence,
)
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@nuge"]
# "wifi" added because timemore_dot.cpp now reads wifi::global_wifi_component
# directly (to defer BLE start until wifi is actually connected, not just
# past its own setup() -- see timemore_dot.h's start_ble_stack_() comment).
DEPENDENCIES = ["esp32", "wifi"]
CONFLICTS_WITH = ["esp32_ble_tracker", "ble_client"]
AUTO_LOAD = ["sensor", "binary_sensor", "button"]
MULTI_CONF = True

timemore_dot_ns = cg.esphome_ns.namespace("timemore_dot")
TimemoreDot = timemore_dot_ns.class_("TimemoreDot", cg.Component)

CONF_TIMEMORE_DOT_ID = "timemore_dot_id"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(TimemoreDot),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # h2zero/NimBLE-Arduino (the Arduino-library variant) doesn't compile
    # under ESPHome's build even with framework: type: arduino -- that
    # framework setting still builds through ESP-IDF's CMake/Kconfig system
    # underneath (confirmed by a real build: it got to actually compiling
    # NimBLE-Arduino's .cpp files before failing on a missing esp_bt.h),
    # and NimBLE-Arduino's own README says as much: "This repo will not
    # compile correctly in ESP-IDF." h2zero/esp-nimble-cpp is the same
    # author's ESP-IDF-native sibling library with the same class/method
    # API (NimBLEDevice, NimBLEClient, NimBLEScan, subscribe(),
    # secureConnection(), etc. all confirmed to match against esp-nimble-cpp
    # 2.5.0's source) -- only real code-level difference found is
    # NimBLEScanCallbacks::onResult taking a `const` pointer in 2.x, fixed
    # in timemore_dot.h/.cpp.
    cg.add_library("h2zero/esp-nimble-cpp", "2.5.0")

    # Found via a GitHub search once the wifi-timing theory was ruled out
    # (a real test with wifi already fully connected still crashed
    # identically): esp-nimble-cpp is fundamentally an ESP-IDF component,
    # not a PlatformIO/Arduino library -- see
    # https://github.com/h2zero/esp-nimble-cpp/issues/407, where the
    # library's own maintainer explains "platformio does not read the
    # kconfig from components [so] there is a mandatory config macro
    # required for the right headers to be included", fixed by adding
    # `-D CONFIG_NIMBLE_CPP_IDF=1` to the build flags. Our build doesn't
    # hit that issue's exact symptom (a missing-header compile error --
    # ours compiles fine), but per
    # https://github.com/h2zero/esp-nimble-cpp/issues/377,
    # cg.add_library() (PlatformIO's lib_deps mechanism, used above) is
    # not really a supported way to pull in an ESP-IDF-native component at
    # all -- it can appear to work under framework: arduino by
    # coincidentally linking against the Arduino core's own bundled NimBLE
    # binary instead of the headers actually being compiled against,
    # which is exactly the kind of silent mismatch that could produce a
    # deep, hard-to-diagnose runtime crash without any compile-time
    # warning. Adding this flag explicitly, in case the missing Kconfig
    # propagation described in #407 is contributing even without the
    # missing-header symptom.
    cg.add_build_flag("-D CONFIG_NIMBLE_CPP_IDF=1")

    # Nothing else in this config requests Bluetooth (deliberately -- no
    # esp32_ble_tracker/ble_client, see HANDOFF.md's architecture-decision
    # section) so nothing else will enable it in sdkconfig either.
    # request_bluetooth() sets CONFIG_BT_ENABLED; ESP-IDF's own Kconfig
    # then defaults the Bluetooth host stack to Bluedroid unless something
    # explicitly picks NimBLE instead, hence the second line -- verified
    # against esp-idf v5.5.5's components/bt/Kconfig `choice BT_HOST` block.
    request_bluetooth()
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_ENABLED", True)

    # A real boot crashed 100% reproducibly inside NimBLE's own host task
    # (nimble_port_run, called from NimBLEDevice::host_task) with the exact
    # same fault address (EXCVADDR 0xa8) as a known esp-nimble-cpp issue
    # caused by esp_bt_controller_init() failing -- see
    # https://github.com/h2zero/esp-nimble-cpp/issues/113. Nothing in this
    # config was telling ESP-IDF that wifi and BT need to coexist on the
    # shared radio, since only esp32_ble's own to_code() normally calls
    # this (and we deliberately don't use esp32_ble).
    #
    # Confirmed by a real rebuild: this alone did NOT fix the crash --
    # identical fault address, identical crash shape, with this in place.
    # Left enabled anyway (correct regardless, and cheap). timemore_dot.cpp
    # also defers NimBLEDevice::init() from setup() to the first loop()
    # iteration after wifi reports actually connected (see
    # start_ble_stack_()'s comment in the header) -- also confirmed NOT
    # the fix by a real test: wifi was fully connected (not just past its
    # own setup()) and the identical crash still happened immediately.
    # That rules out timing/coexistence as the cause entirely; see the
    # add_build_flag() call above for the current hypothesis.
    request_software_coexistence()

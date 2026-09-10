import esphome.codegen as cg
from esphome.components.esp32 import (
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
AUTO_LOAD = ["sensor", "binary_sensor", "button", "text_sensor"]
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

    # Root-caused the boot crash (Guru Meditation LoadProhibited, EXCVADDR
    # 0x98/0xa8, deep in nimble_port_run/host_task) after four failed fix
    # attempts under h2zero/esp-nimble-cpp (coexistence, deferred init, the
    # CONFIG_NIMBLE_CPP_IDF build flag, and CONFIG_BT_NIMBLE_ENABLED via
    # add_idf_sdkconfig_option -- see git history for those, all confirmed
    # NOT to fix it on real hardware): esp-nimble-cpp is a *headers-only*
    # NimBLE wrapper. It requires ESP-IDF's actual NimBLE host stack
    # (nimble_port_init, host task, memory pools, sdkconfig-driven build)
    # to already exist -- but this project uses framework: type: arduino,
    # and the Arduino core's prebuilt static libs (espressif/esp32-arduino-
    # libs) are built with Bluedroid only, no NimBLE host at all. Every
    # sdkconfig-option fix was therefore a no-op: the Arduino framework
    # build doesn't consume sdkconfig.* the way a native ESP-IDF project
    # does, it links against those prebuilt libs. NimBLEDevice::init() was
    # calling into a NimBLE host that was never actually built into the
    # firmware, hence the crash deep inside its own host task.
    #
    # h2zero/NimBLE-Arduino is the fix: unlike esp-nimble-cpp, it bundles
    # the full mynewt-nimble host source itself and compiles it directly
    # into the sketch, so it doesn't depend on the framework's prebuilt
    # libs having NimBLE support at all. Same author, same class/method API
    # (NimBLEDevice, NimBLEClient, NimBLEScan, subscribe(), secureConnection(),
    # NimBLEScanCallbacks::onResult taking a `const` pointer in 2.x) as
    # esp-nimble-cpp, so no other code in this component needed to change.
    #
    # This was tried once earlier in the investigation and set aside after
    # a build got partway through compiling NimBLE-Arduino's own .cpp files
    # before failing on a missing esp_bt.h -- at the time that looked like
    # "doesn't compile under this framework at all" and esp-nimble-cpp
    # seemed like the safer bet. In hindsight that was very likely a
    # transient/fixable include-path issue (esp_bt.h is a generic ESP-IDF
    # BT-controller header that Arduino's prebuilt libs do ship), not proof
    # NimBLE-Arduino is fundamentally incompatible -- if this build hits
    # that same missing-header error again, chase the include path rather
    # than reverting to esp-nimble-cpp, since esp-nimble-cpp is now known
    # to be architecturally wrong for framework: arduino regardless.
    cg.add_library("h2zero/NimBLE-Arduino", "2.3.6")

    # NimBLE-Arduino's own NIMBLE_LOGE() calls (which log the exact esp_err_t
    # from nvs_flash_init()/esp_bt_controller_init()/enable()/esp_nimble_hci_init()
    # -- the only paths inside NimBLEDevice::init() that return false) are
    # compiled out entirely unless CONFIG_NIMBLE_CPP_LOG_LEVEL is defined --
    # see src/NimBLELog.h, which otherwise falls back to Arduino's
    # CORE_DEBUG_LEVEL (0 under ESPHome's build). Without this, init()
    # failing is silent on our end too (nothing to log ourselves -- the
    # library doesn't expose the esp_err_t to the caller, only true/false).
    cg.add_build_flag("-DCONFIG_NIMBLE_CPP_LOG_LEVEL=4")

    # Nothing else in this config requests Bluetooth (deliberately -- no
    # esp32_ble_tracker/ble_client, see HANDOFF.md's architecture-decision
    # section) so nothing else will enable it in sdkconfig either.
    request_bluetooth()

    # Wifi and BT share the ESP32's single 2.4GHz radio; this is correct
    # regardless of which NimBLE library is in use, and cheap, so left
    # enabled even though it was confirmed (during the esp-nimble-cpp
    # investigation) not to be the fix for the crash by itself.
    request_software_coexistence()

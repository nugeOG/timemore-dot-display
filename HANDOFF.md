# Timemore Dot ESP32 Display — project handoff

Goal: an ESP32 + 2.8" touchscreen (240x320, LVGL) acting as a standalone
Bluetooth display/controller for a TIMEMORE Black Mirror Dot coffee scale,
built as an ESPHome project.

This doc exists so implementation work can pick up in Claude Code (or any
fresh session) without re-deriving anything below. Everything here came out
of a planning conversation — none of it has been built or tested on real
hardware yet.

Repo: [github.com/nugeOG/timemore-dot-display](https://github.com/nugeOG/timemore-dot-display)
(private). Built to run via the **Home Assistant ESPHome add-on**. For
step-by-step build/install instructions, see [README.md](README.md) — this
doc is technical background and status, not a setup guide.

## Hardware

- Scale: TIMEMORE Black Mirror Dot (screenless, BLE-only, phone app normally
  provides the display)
- Display: generic "2.8 inch ESP32 LVGL WIFI Bluetooth Touch 240x320" module
  — **exact display driver chip (ILI9341/ST7789/etc.), touch controller
  (resistive XPT2046 vs capacitive), and pinout are not yet confirmed.**
  This is the first thing to nail down before writing display code — it
  varies between near-identical-looking listings.

## BLE protocol — confirmed from source

Found in `gaggimate/esp-arduino-ble-scales`, `src/scales/dot.h` /
`dot.cpp` (MIT-licensed library for the GaggiMate espresso machine
controller project). This is the authoritative source — re-check that repo
if anything below seems to not match your actual scale.

- Device advertises with a name starting with `TIMEMORE_Dot`
- Service UUID: `FFF0`
- Weight/notify characteristic: `FFF1`
- Command/write characteristic: `FFF2`
- Frame format (big-endian): `A5 5A [class] [type] [len_hi len_lo] [payload...] [crc_hi crc_lo]`
  — total frame length = payload len + 8
- **Weight frame**: class `0x01`, type `0x01`, payload length 9. Bytes
  `[6..9]` are a signed big-endian int32, value = grams × 10 (divide by 10).
- **Battery frame**: class `0x01`, type `0x05`, payload length 2. One byte
  is battery percentage 0–100.
- **Tare**: write `A5 5A 02 04 00 00 9A 00` to FFF2, then write
  `A5 5A 03 0D 00 00 64 D1` (a handshake/poll frame) as a follow-up — the
  scale doesn't actually zero until this second write lands.
- CRC trailers on the two command frames above are **hardcoded constants
  captured from a real session**, not computed — the reference driver
  doesn't verify or calculate them either.
- **Critical constraint**: the scale will not emit weight notifications
  until the BLE link is encrypted/bonded. The reference driver explicitly
  checks `secureConnection()` after connect and aborts if it's not secure.
- **Not implemented in the reference driver** (meaning: not confirmed to
  exist at all, or exists but undecoded): timer control, unit switching,
  explicit flow rate. A code comment notes bytes `[10..14]` of the weight
  frame are "additional payload (likely flow / secondary metric)" that the
  driver ignores — worth investigating if you want more than weight/battery
  directly from the scale.
- Physical scale controls (inferred from sibling Black Mirror products —
  not confirmed Dot-specific): two capacitive touch buttons, Power/Tare and
  Timer, working standalone regardless of BLE connection.

## Architecture decision: why not `ble_client:`/ESPHome-native, why not the GaggiMate library directly

- ESPHome's declarative `ble_client:`/`esp32_ble_tracker:` doesn't give
  fine-grained control over forcing a secure/bonded connection with retry
  logic before proceeding — which this scale requires. A stateful
  multi-frame byte parser with resync logic is also awkward in YAML lambdas.
- Importing the GaggiMate scales library wholesale into an ESPHome external
  component is risky: that library manages its own independent BLE
  connection/scanning via NimBLE, and ESPHome's own BLE components also
  want to own the NimBLE stack. Two independent stacks fighting over one
  radio is a known source of crashes.
- **Decision: write a custom ESPHome external component that owns the BLE
  stack exclusively** (no `esp32_ble_tracker:`/`ble_client:` declared in
  YAML at all), porting the connect/handshake/decode logic from
  `dot.cpp` directly against NimBLE's C++ wrapper API (h2zero/esp-nimble-cpp
  -- see below for why that specific library, not NimBLE-Arduino despite
  the similar name). ESPHome still handles Wi-Fi, OTA, display driver, and
  LVGL as normal — only the scale connection is hand-rolled.

## `timemore_dot` component — written, build in progress

The custom ESPHome external component described above now exists at
`components/timemore_dot/`:

```
components/timemore_dot/
├── __init__.py       # ESPHome component registration, pins esp-nimble-cpp 2.5.0
├── sensor.py          # exposes weight + battery_level as sensor: platform
├── binary_sensor.py   # exposes connected as binary_sensor: platform
├── button.py           # exposes tare as button: platform (TareButton)
├── timemore_dot.h
└── timemore_dot.cpp   # ported connect/handshake/decode logic from dot.cpp
```

**Caveat that matters more than the others in this doc**: this code has
not been confirmed working end-to-end on real hardware yet (config
validation and most compilation now pass -- see build status below). It's
a direct port of the connect → bond → subscribe → decode flow from the
reference driver, against h2zero/esp-nimble-cpp's C++ wrapper API
(`setScanCallbacks`, `secureConnection()`, the `subscribe()` lambda
signature, `writeValue()`'s bool return) — verified by reading
esp-nimble-cpp 2.5.0's actual source, not just assumed. If a future
library bump breaks the build on these specific calls, check
esp-nimble-cpp's migration guide, not a sign the overall approach
(connect/bond/decode logic) is wrong.

Reconnection uses the same `marked_for_reconnect_` pattern named in the
original build/test-order note below, polled from `loop()` on a 5s backoff
rather than acted on inline from a BLE callback.

**Build status as of this writing**: actively being test-built via the
Home Assistant ESPHome add-on, running **ESPHome 2026.8.2**. Issues found
and fixed so far, in order:
1. `invert_colors` required on `display.ili9xxx` (no default on this
   ESPHome version).
2. `touchscreen.calibration` needing a nested block, not flat
   `calibration_x_min`/etc. keys.
3. `ota.esphome`'s `encryption:` option doesn't exist on 2026.8.2 (only on
   ESPHome's unreleased `dev` branch) — reverted to `password:`. **Lesson:
   when verifying anything against ESPHome's source, check the tag
   matching the installed version, not `dev`** — they can disagree.
4. `lbl_playpause_text` referenced by two LVGL scripts but never actually
   defined as a widget — a leftover from the original handoff YAML, never
   caught before a real build ran config validation on it. Removed.
5. **The big one**: `h2zero/NimBLE-Arduino` (the originally-chosen library)
   doesn't compile under ESPHome's build at all. Its own README says so
   explicitly ("This repo will not compile correctly in ESP-IDF") --
   ESPHome's `framework: type: arduino` still builds through ESP-IDF's
   CMake/Kconfig system underneath, which isn't what NimBLE-Arduino
   expects. A real build attempt got as far as actually compiling
   NimBLE-Arduino's `.cpp` files before failing on a missing `esp_bt.h`,
   which is what surfaced this. Switched to `h2zero/esp-nimble-cpp` (same
   author, same API, built for exactly this ESP-IDF-based scenario) and
   added the two `esp32.request_bluetooth()` /
   `CONFIG_BT_NIMBLE_ENABLED` sdkconfig calls nothing else in this config
   would otherwise trigger, since BLE is deliberately not requested via
   `esp32_ble_tracker:`/`ble_client:`. See `components/timemore_dot/__init__.py`
   for the full reasoning and source citations. This got compilation past
   the point of even touching BLE code -- next error was unrelated (below).
6. Four LVGL `!lambda` text updates returned a raw ternary of C string
   literals (`cond ? "A" : "B"`), which resolves to `const char*`/
   `const char[N]`, not `std::string` -- but ESPHome's generated code calls
   `.c_str()` on the lambda's result assuming it's std::string-like,
   which only compiles for a real `std::string`. Fixed by wrapping each in
   `std::string(...)`. Affected: the bluetooth icon glyph swap
   (`timemore-dot-display.yaml`), and the play/pause icon + both mode-label
   text and icon updates (`scale-display-lvgl.yaml`). The weight/battery/
   flow labels were already fine since they go through `str_sprintf()`,
   which does return `std::string`.

**Milestone: it compiled, flashed, and booted on the real board.** First
real boot log obtained via OTA logs, screen stayed blank. Two issues found
from that log, both fixed (unverified until the next flash):
7. `[W][lvgl:998]: Failed to allocate 153600 bytes for draw buffer` — this
   board has no PSRAM configured, so LVGL's default 100%-of-screen draw
   buffer (153,600 bytes = 240×320×2) didn't fit in whatever's left of
   internal RAM once wifi/BLE have claimed their share. Set
   `buffer_size: 25%` in `scale-display-lvgl.yaml`'s `lvgl:` block —
   ESPHome's own documented recommendation for PSRAM-less boards. **This
   fixed the allocation warning** (confirmed gone in the next boot log)
   **but turned out to be unrelated to the crash below** — same crash,
   same fault address, still happened with the warning gone. Real
   allocation bug, real fix, just not the one causing the reboot loop.
8. `E (472) gpio: gpio_pullup_en(...): GPIO number error (input-only pad
   has no internal PU)`, logged once per boot right before "Attach Touch
   Interrupt". GPIO36 (the touchscreen `interrupt_pin` guess) is one of
   the ESP32's input-only pins (34-39), which have no internal pull
   resistor hardware at all — requesting one fails. Non-fatal (boot
   continued past it), but fixed anyway: `interrupt_pin` now sets
   `mode: {input: true}` explicitly (no pullup requested), relying on the
   touch panel having its own external pull-up on T_IRQ, which is
   standard for this class of board. If touch doesn't register at all
   once wired up, revisit this assumption first.
9. **The actual crash** (still present after #7): `Guru Meditation Error:
   Core 1 panic'ed (LoadProhibited)`, 100% reproducible every boot,
   `EXCVADDR` a tiny offset (`0x98`/`0xa8`) — and notably, `timemore_dot`'s
   very first log line never printed, meaning it crashes inside
   `NimBLEDevice::init()` before returning, not in any of this component's
   own code. Traced via the backtrace addresses' pattern (not a symbolized
   trace — no ELF available here) to almost certainly the same failure as
   [h2zero/esp-nimble-cpp#113](https://github.com/h2zero/esp-nimble-cpp/issues/113),
   which has the **identical** `EXCVADDR: 0x000000a8` crashing in
   `nimble_port_run`/`NimBLEDevice::host_task`, caused by
   `esp_bt_controller_init()` failing. This component starts BLE right
   after wifi (`AFTER_WIFI` priority) while wifi is still actively
   scanning/connecting on the same 2.4GHz radio, and nothing in this
   config was telling ESP-IDF that wifi and BT need to coexist on that
   shared radio — only `esp32_ble`'s own `to_code()` normally calls
   `esp32.request_software_coexistence()`, and this component deliberately
   doesn't use `esp32_ble`. Added that call to
   `components/timemore_dot/__init__.py`. **Tried, did not fix it** —
   rebuilt and reflashed with the coexistence request in place; identical
   crash, identical `EXCVADDR` values, same backtrace shape. The
   esp-nimble-cpp#113 fault-address match was a real, specific lead, but
   turned out to be either the wrong cause or an incomplete fix for it.
10. Since guessing further wasn't converging (no ELF available here to
    symbolize the actual backtrace), switched to instrumenting instead:
    `TimemoreDot::setup()` now logs on entry, and separately logs and
    bails out (rather than silently continuing) if `NimBLEDevice::init()`
    returns `false` — a real bug regardless of whether it's the crash
    cause, since the old code ignored that return value and would
    configure security / start scanning against whatever partial state a
    failed `init()` left behind. **Whichever of these log lines does or
    doesn't appear in the next boot log will localize the crash for
    real** — if even the entry log is missing, the crash isn't in this
    component at all and every theory above is wrong; if entry logs but
    `init()` never returns, the crash is inside NimBLE's own init
    sequence as suspected, just not fixed by coexistence alone.
11. **Neither log line appeared** in the next boot log either — same
    conclusion as before, crash not reached this component at all, or so
    it seemed. To get a clean answer, ran an actual bisection instead of
    reasoning from log silence: commented out `timemore_dot:` and its
    three `platform: timemore_dot` entries entirely
    (`timemore-dot-display.yaml`) and rebuilt. **This booted clean — no
    crash, `setup() finished successfully!`, wifi connected, touch
    events registering.** This flips the earlier conclusion: the crash
    IS tied to `timemore_dot` after all. Almost certainly, the
    coexistence-fix and instrumented-logging builds were never actually
    testing the code they claimed to -- this whole investigation kept
    tripping on the same `refresh: 1h` git-cache staleness documented
    elsewhere in this doc (rebuilding without first bumping to
    `refresh: always` silently re-tests old component code). By the time
    this was understood, focus shifted to the display/touch work (see
    "Screen orientation" below); `timemore_dot` stayed disabled while
    that happened.
12. **Re-enabled** now that the display/touch investigation is done:
    `timemore_dot:`, its `external_components:` entry, and the
    sensor/binary_sensor/button platform entries are back in
    `timemore-dot-display.yaml`, and `do_tare` calls `button.press:
    tare_button` again in `scale-display-lvgl.yaml`. The coexistence fix
    from step 8 is still in place but was never actually proven to work
    (or not) given the caching confusion above — this next build is the
    first real test of it since it was written. `refresh: always` is
    already the default in this file, so no manual bump should be needed
    this time, but double-check the local `timemore-dot-display.yaml` on
    your Home Assistant is the current version (re-download if unsure).
13. **Same test, real result this time (caching wasn't the issue here):
    coexistence alone does not fix the crash.** Identical fault address,
    identical crash shape, confirmed against a build with fresh display
    fixes visibly present in the log (`MADCTL 0x40`, `320 x 240`) — so
    this wasn't stale code. `TimemoreDot::setup()`'s own diagnostic log
    line still never showed up, but that evidence is now considered
    unreliable rather than conclusive: these logs stream over the
    network (ESPHome API), not a raw serial line, and a crash within
    milliseconds of a log call can prevent the buffered message from ever
    being transmitted. The crash backtrace is only reachable through
    `nimble_port_run`/`host_task`, which only exists if
    `NimBLEDevice::init()` was actually called — so the balance of
    evidence still points to the crash being inside NimBLE's own init
    sequence, just not fixed by coexistence alone.
14. Implemented the documented fallback: `NimBLEDevice::init()` (and
    everything after it, now `start_ble_stack_()`) no longer runs from
    `setup()`. `setup()` does nothing but log. `loop()` polls
    `wifi::global_wifi_component->is_connected()` — actually associated,
    not just "wifi's own `setup()` returned" — and calls
    `start_ble_stack_()` exactly once, on the first `loop()` iteration
    after that becomes true. Added `wifi` to `DEPENDENCIES` in
    `__init__.py` since the component now references
    `wifi::global_wifi_component` directly.
15. **Real test result: timing/coexistence conclusively ruled out.** The
    boot log showed the device reach `[I][wifi:1609]: Connected` (fully
    associated, IP assigned, not just "setup() returned") and THEN
    `"Starting BLE stack"` logged successfully — and the identical crash
    (same `EXCVADDR` 0x98/0xa8, same crash shape) still happened
    immediately after. This settles it: the crash isn't a wifi-radio-
    timing race at all. It's a deterministic failure specifically tied to
    calling into NimBLE's init path on this exact board/library/ESP-IDF
    combination, regardless of when that call happens.
16. Since the failure is deterministic and library-specific, searched
    GitHub for anything matching this exact library/build combination
    rather than guessing further. Found
    [h2zero/esp-nimble-cpp#407](https://github.com/h2zero/esp-nimble-cpp/issues/407):
    the library's own maintainer explains PlatformIO "does not read the
    kconfig from components," so a Kconfig value like our
    `CONFIG_BT_NIMBLE_ENABLED=y` may never actually reach esp-nimble-cpp's
    own header-selection logic as a real C++ preprocessor define — fixed
    there by adding `-D CONFIG_NIMBLE_CPP_IDF=1` as an explicit build
    flag. More significantly,
    [h2zero/esp-nimble-cpp#377](https://github.com/h2zero/esp-nimble-cpp/issues/377)
    reveals `cg.add_library()` (PlatformIO's `lib_deps` mechanism, what
    `components/timemore_dot/__init__.py` has used all along) isn't
    really a supported way to pull in esp-nimble-cpp at all — it's
    fundamentally an ESP-IDF component (needs `idf_component.yml`), and a
    maintainer states outright "You can not add an espidf component via
    `lib_deps`." It can appear to work under `framework: arduino` by
    coincidentally linking against Arduino core's own bundled NimBLE
    binary instead of what the headers were actually compiled against —
    exactly the kind of silent ABI mismatch that would compile clean and
    crash deep at runtime with no warning, matching our symptom exactly.
    Added the `-D CONFIG_NIMBLE_CPP_IDF=1` build flag as the next thing to
    try — **not yet confirmed on real hardware.** If this doesn't resolve
    it, the properly-supported next step is switching away from
    `cg.add_library()` entirely to `esp32.add_idf_component()` (ESPHome's
    API for adding a real ESP-IDF component via git), which was considered
    earlier and set aside for `cg.add_library()`'s simplicity -- given
    what's now known, that assumption needs revisiting. *(Superseded — see
    #17: the `esp32.add_idf_component()` route is no longer the next step.)*
17. **`CONFIG_NIMBLE_CPP_IDF` confirmed NOT a fix — and the actual root
    cause found.** A real fresh rebuild + boot log with the build flag in
    place produced the *identical* crash: same `EXCVADDR` (0x98 on Core 1 /
    0xa8 on Core 0), same backtrace, across many crash-loop iterations —
    all reporting the same ELF SHA256 (`c11514df0`), which confirms this
    was a genuine rebuild of new firmware and not a stale cached binary.
    The real problem: `h2zero/esp-nimble-cpp` is a **headers-only C++
    wrapper**. It requires ESP-IDF's actual NimBLE *host stack*
    (`nimble_port_init`, the host task, its memory pools) to already be
    compiled into the firmware. This project builds with
    `framework: type: arduino`, and the Arduino core's prebuilt static libs
    (`espressif/esp32-arduino-libs`) are built with **Bluedroid only — no
    NimBLE host at all**. That also explains why *every* sdkconfig-based fix
    attempted was a no-op: `CONFIG_BT_NIMBLE_ENABLED` via
    `add_idf_sdkconfig_option` (#13/#14) and the `CONFIG_NIMBLE_CPP_IDF`
    build flag (#16) both assume a native ESP-IDF build that compiles from
    sdkconfig; the Arduino framework build doesn't consume `sdkconfig.*` at
    all, it just links the prebuilt libs. So `NimBLEDevice::init()` was
    calling into a NimBLE host that was never compiled into the firmware —
    hence a crash deep inside its own host task.
    **Fix applied**: `components/timemore_dot/__init__.py` now uses
    `cg.add_library("h2zero/NimBLE-Arduino", "2.3.6")` instead of
    `cg.add_library("h2zero/esp-nimble-cpp", "2.5.0")`. NimBLE-Arduino
    bundles the full mynewt-nimble host source itself and compiles it
    directly into the sketch, so it does **not** depend on the framework's
    prebuilt libs having NimBLE support. Same author, same class/method API
    as esp-nimble-cpp (compatibility already confirmed earlier in this
    investigation), so no other component code changed. The now-dead
    `cg.add_build_flag("-D CONFIG_NIMBLE_CPP_IDF=1")` and the no-op
    `add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_ENABLED", True)` were both
    removed.
    *Note on #5*: NimBLE-Arduino was tried once early on and set aside after
    a build got partway through compiling its own `.cpp` files before
    failing on a missing `esp_bt.h` — which at the time looked like a
    fundamental incompatibility and prompted the move to esp-nimble-cpp. In
    hindsight, since esp-nimble-cpp is now understood to be architecturally
    wrong for `framework: arduino` regardless, that `esp_bt.h` failure was
    very likely a transient/fixable include-path issue, not proof that
    NimBLE-Arduino can't work here.
    **NOT YET CONFIRMED on real hardware.** The next real build/boot log
    needs to be checked for (a) whether it now compiles cleanly — if the
    missing `esp_bt.h` recurs, that needs its own investigation rather than
    a revert to esp-nimble-cpp — and (b) whether the boot crash is actually
    resolved.
18. **Confirmed working end-to-end on real hardware — the BLE connectivity
    problem is RESOLVED.** After the host-task-deadlock fix (deferring
    `connect()` out of the scan callback into `loop()`, connecting by
    address with a 3-attempt retry, and relaxing the scan duty cycle to
    500/100), a real boot log showed the entire BLE flow succeeding in
    sequence: scan found the scale → connected → MTU negotiated (247) →
    `secureConnection()` succeeded (bonded) → service `0xFFF0` and both
    characteristics `0xFFF1`/`0xFFF2` discovered → the notify
    subscription's CCCD write completed (`status=0`) → live weight
    notifications began arriving from the scale (`Got Notification for
    characteristic uuid: 0xfff1`, repeating). Every failure mode this
    investigation chased — the boot crash, the silent `init()` failure, the
    stuck reconnect logic, and the `connect()` timeout/deadlock — is now
    resolved; the scale connects, bonds, and streams live data reliably.
    **One more real bug found and fixed in the same test**: a scan result
    that had been queued just before `scan->stop()` took effect still
    reached `loop()` (via the `have_pending_connect_` flag) a few seconds
    *after* the connection had already succeeded — which would have torn
    down a perfectly healthy connection in order to spuriously "reconnect"
    to the same device. Fixed by having `loop()` check
    `client_->isConnected()` before acting on a pending scan match.

Connection and bonding are now confirmed on real hardware (see #18), so
what's left to verify is everything downstream of the byte stream. Verify
in this order (riskiest first) rather than assuming live notifications
mean the decode is right:
1. Weight frame decode — compare the grams shown on the display (and the
   HA weight entity) against the scale's own reading / a known reference
   weight. Live notifications arriving is not proof the `×10` signed
   big-endian int32 at bytes `[6..9]` is being read correctly.
2. Battery frame decode — does the battery entity show a sane, plausible
   percentage that tracks the scale's real charge level?
3. Tare — does pressing the on-screen tare button (or the HA `tare_button`
   entity) actually zero the scale, including the second handshake write
   the scale needs before it commits?
4. Reconnection — power-cycle the scale or pull it out of range while the
   ESP32 keeps running; does it reconnect on its own within a few
   reconnect-backoff cycles?
5. Auto-timer start/stop detection (see "Auto-timer logic" below) — still
   pending/deferred; it's computed entirely on-device from the weight
   stream, so it can't be evaluated until the decode above is trusted.

## Screen orientation — corrected to native landscape

A real boot photo showed the physical board is landscape-shaped (cable at
top, wider than tall), but the firmware was rendering a 240×320 portrait
layout, so content appeared sideways. This went through a few iterations:

1. First attempt: `display.rotation` (later moved to `lvgl.rotation`,
   since ESPHome rejects `rotation:` on `display:` when LVGL is in use)
   tried `90`, then `270` per direct feedback from the real screen — both
   still wrong, and worse, a real photo showed a persistent static/noise
   band covering part of the screen, meaning the drawable area never
   covered the full physical panel.
2. Root cause found once the board's silkscreen ("TPM408-2.8") was
   actually read and looked up (see open item #1): `ili9xxx` never
   declares hardware-rotation support, so LVGL's `rotation:` was always a
   *software* transform applied on top of the driver's default 240×320
   (portrait) dimensions — which never matched this panel's true native
   320×240 (landscape) shape in the first place. Rotating the wrong-sized
   buffer explains the never-covered static region.
3. First fix attempt: kept `model: ILI9341` and manually added
   `dimensions: {width: 320, height: 240}` + `color_order: RGB` (was
   defaulting to BGR — also wrong per the same TFT_eSPI reference) +
   `transform: swap_xy: true` (the display-level, MADCTL-based hardware
   equivalent of rotation). **Made it worse, not better** — a real photo
   showed the status row near the origin rendering fine, then
   progressively more corrupted/melted-looking toward the middle and
   bottom, with the bottom region still completely untouched (static).
   Consistent with an addressing/stride mismatch from combining
   `dimensions:` and `swap_xy` in a way this chip doesn't expect them
   used together, even though each individually is documented/valid.
4. Real fix: `ili9xxx` ships a model built for exactly this situation --
   `ILI9XXXILI9342`, same command set as ILI9341
   (`INITCMD_ILI9341`) but constructed with `width=320, height=240`
   directly baked in, no `swap_xy`/`dimensions` override needed. Switched
   `model: ILI9341` → `model: ILI9342`, dropped the manual `dimensions:`/
   `transform:` from step 3 entirely, kept `color_order: RGB`. Not yet
   confirmed on real hardware — this is a much more likely-correct
   approach than composing the primitives by hand (it's what the model
   class exists for), but "likely correct" isn't "confirmed."
5. Added a `touchscreen.transform: swap_xy: true` guess separately, since
   raw touch coordinates come from the panel's fixed physical wiring
   independent of the display driver's own transform, and likely need
   realigning to match.
6. Rewrote `scale-display-lvgl.yaml`'s entire widget layout for a native
   320×240 canvas (not a rotated copy of the old portrait layout): status
   row across the top, weight+flow in a left column, timer+mode in a
   right column, all four buttons along the bottom.

**Milestone: `model: ILI9342` confirmed correct by a real photo** — full
screen, right-side up, layout matching the design exactly (status row top,
weight/flow left, timer/mode right, four buttons along the bottom). The
display side of this investigation is done.

Touch alignment took two real-hardware tests to nail down, both
confirmed (not guesses): `swap_xy: true` alone left touches **vertically
mirrored** (top registered as bottom) — fixed with `mirror_y: true`. That
alone then left touches **horizontally mirrored** too — pressing "start"
registered as "reset" and "mode" registered as "tare" (an exact left/right
swap of the four-button row) — fixed with `mirror_x: true`. All three
transform flags (`swap_xy`, `mirror_x`, `mirror_y`) are now set; this
should be the complete, correct axis mapping.

**Calibrated.** ESPHome has no interactive calibration tool — its
documented procedure is to log raw touch coordinates via an `on_touch:`
lambda while tapping the four corners, then hand-set `calibration:` from
what's observed. Added that lambda temporarily, had all four corners
tapped, read the settled (not first-contact-noisy) `x_raw`/`y_raw` values
off the logs:
- Bottom-right: x_raw≈187, y_raw≈310
- Top-right: x_raw≈217, y_raw≈3755
- Top-left: x_raw≈3781, y_raw≈3792
- Bottom-left: x_raw≈3797, y_raw≈302

Both corners sharing an edge agreed within ~30 raw units, a reasonable
sign of consistency. Set `calibration: {x_min: 187, x_max: 3797,
y_min: 302, y_max: 3792}` and removed the temporary logging hook. This
closes out the entire display/touch orientation investigation — display
renders correctly, all four buttons register correctly, and touch is now
calibrated to real measured values rather than a full-range placeholder.

**Physical 180-degree flip (USB port moved to the other side).** Added an
explicit `display.transform: {mirror_x: false, mirror_y: true}` — with no
transform block at all, `model: ILI9342` rendered correctly but with
`mirror_x` baked in as its own default (confirmed via a real boot log:
"Mirror_x: YES, Mirror_y: NO" with nothing set here), so a 180-degree spin
means inverting both axes relative to that default. Touch's
`transform.mirror_x`/`mirror_y` flipped from `true`/`true` to
`false`/`false` to match (raw wiring didn't move, but which corner is
which did); `swap_xy` unaffected, since a 180-degree rotation doesn't
change which axis is which. **Not yet confirmed on real hardware** — next
boot needs to verify the physical orientation actually matches and all
four touch corners still land correctly.

## Auto-timer logic (not scale-dependent — computed entirely on-device)

Since timer state likely isn't transmitted over BLE at all, the plan is to
replicate typical coffee-scale auto-timer behavior purely from the weight
stream:

- **Auto-start**: weight increasing faster than a small noise threshold
  (e.g. >0.5–1g within a short window) = pour started
- **Auto-stop, pour-over mode**: weight plateaus (near-zero delta) for
  ~1–2s after having been actively increasing
- **Auto-stop, espresso mode**: weight drops sharply toward zero (cup
  removed)
- **Tare suppression window**: after a manual tare command, suppress
  auto-stop logic for ~1.5s so the intentional zero isn't mistaken for
  "cup removed"
- **Flow rate**: compute directly as `(weight_now - weight_prev) /
  (time_now - time_prev)`, smoothed over a short rolling window — don't
  rely on the unconfirmed bytes in the weight frame
- Manual Start/Pause and (long-press) Reset buttons remain available
  regardless of auto-detect

## Multi-device / relay considerations

**Decision (confirmed): single-connection.** BLE peripherals (this class of
device very much included) typically only accept one central connection at
a time — still untested for the Dot specifically, but assumed until proven
otherwise. If you do get a chance to test by connecting the custom
firmware and the official Timemore app simultaneously, update this note
with the actual result.

Given that, the ESP32 is treated as the sole BLE client, and data fans out
over the network instead of trying to share the BLE connection — this is
implemented in `timemore-dot-display.yaml`:
- `api:` (with an encryption key) is the primary path — Home Assistant
  auto-discovers it and gets the weight/battery/connected/tare entities
  defined in that file's `sensor:`/`binary_sensor:`/`button:` sections.
- An `mqtt:` block is included commented-out for any non-Home-Assistant
  consumer that wants the same data.

A true BLE relay (ESP32 as both central to the scale and peripheral
  re-advertising its own service) is possible but nontrivial, and wouldn't
  let the *official* Timemore app connect to the relay — only a custom
  client you write yourself.

## UI design — finalized

Screen is 240x320 portrait, dark theme, large touch targets. Final layout
(see `scale-display-lvgl.yaml` in this handoff bundle for the actual
ESPHome config):

- **Status row**: time + date (left), wifi signal (standard fan/arc glyph,
  not custom bars), Bluetooth connection icon, battery % + icon (right)
- **Weight**: large numerals, dominant element
- **Flow rate**: small secondary line below weight, computed on-device
- **Timer**: `mm:ss`, with a small "auto" badge, plus a mode label
  (Pour-over / Espresso)
- **Four buttons**, bottom row, large circular touch targets:
  - **Tare** — confirmed real BLE command
  - **Start/Pause** — primary action, larger button
  - **Reset** — **requires a 1.5s long-press**, with a visible progress
    ring around the button as feedback. Implemented manually via
    `on_press`/`on_release` + a 30ms interval driving an `arc` widget,
    *not* via LVGL's built-in long-press event — there's an open,
    unresolved community question about configuring long-press timing for
    touchscreen presses specifically (well-documented for rotary
    encoder/keypad inputs, not clearly for touch), so a fully manual timer
    avoids depending on unclear behavior for something that needs to be an
    exact duration.
  - **Mode** — toggles which auto-stop condition applies

## Open items / unverified — check these first in Claude Code

1. ~~Exact display/touch driver and pinout~~ **Mostly confirmed**: a real
   boot photo showed the board's silkscreen reads "TPM408-2.8" — a
   specific "Cheap Yellow Display" variant with its own community
   troubleshooting repo
   ([cosynuss999-max/Setup-for-TPM408-2.8-variant](https://github.com/cosynuss999-max/Setup-for-TPM408-2.8-variant)).
   Its TFT_eSPI config uses the *exact same* SPI/CS/DC/backlight pins
   already in `timemore-dot-display.yaml`, confirming the board-family
   guess was right — but it declares `TFT_WIDTH 320` / `TFT_HEIGHT 240`
   (native landscape) and `TFT_RGB_ORDER TFT_RGB`, neither of which
   matched this project's original config (which assumed 240x320 portrait
   + BGR). Fixed via `model: ILI9342` (a landscape-native ILI9341-chipset
   model ESPHome ships specifically for this) + `color_order: RGB` on the
   `display:` block — see "Screen orientation" below for the full trail,
   including a first attempt that made things worse. Still not confirmed
   by an actual clean render; that TFT_eSPI source didn't cover touch
   wiring at all, so the XPT2046 pins remain a separate, still-unconfirmed
   guess.
2. ~~Icon font~~ **Resolved**: fonts are now pulled at build time via
   `gfonts://` (Inter for text, Google's Material Symbols Outlined for
   icons — see `scale-display-lvgl.yaml`'s `font:` block) rather than
   bundled TTF files, and the glyph codepoints are real ones looked up from
   `google/material-design-icons`' codepoints file, not placeholders. Not
   yet verified by actually rendering them, though — the semantic picks
   (`exposure_zero` for tare, `coffee`/`local_cafe` for pour-over/espresso
   mode) are reasonable guesses at which glyph looks right, not confirmed
   against the real rendered icon. **This needs internet access at build
   time** to fetch fonts from Google's CDN — fine for the Home Assistant
   ESPHome add-on, but worth knowing if you ever build offline.
3. ~~`lvgl.arc.update` action name~~ **Resolved**: config validation now
   passes clean on ESPHome 2026.8.2 (confirmed by an actual build reaching
   the C++ compile stage), so this action name is correct as written.
4. ~~`timemore_dot` external component — BLE connectivity~~ **Resolved**:
   builds, flashes, boots, connects, bonds, and streams live weight
   notifications on real hardware (see build-status #18). Uses
   `h2zero/NimBLE-Arduino`, with `connect()` deferred out of the scan
   callback into `loop()` to avoid deadlocking on NimBLE's own host task.
   What's still unverified is everything *downstream* of the byte stream —
   weight decode, battery decode, tare, reconnection, and the auto-timer
   detection logic (see the numbered verify-in-this-order list above).
5. ~~Touchscreen calibration~~ **Resolved**: `calibration:` in
   `timemore-dot-display.yaml` now uses real measured values from tapping
   all four corners (see "Screen orientation" below for the full trail),
   not the original full-raw-range placeholder.
6. Repo created: [github.com/nugeOG/timemore-dot-display](https://github.com/nugeOG/timemore-dot-display)
   (private).

## Files included in this handoff

- `HANDOFF.md` — this file
- `README.md` — step-by-step build/install instructions via the Home
  Assistant ESPHome add-on, written for a first-time/inexperienced user
- `THIRD_PARTY_NOTICES.md` — credit and license text for the projects this
  one builds on (most importantly, the BLE protocol was ported from
  someone else's reverse-engineering work, not derived independently here)
- `timemore-dot-display.yaml` — the top-level ESPHome device config: wifi,
  `api:`/`mqtt:` network fan-out, display/touch driver (placeholder pins,
  see open item #1), the `timemore_dot` component, and the sensor/
  binary_sensor/button entities wired into the LVGL labels.
- `scale-display-lvgl.yaml` — the LVGL screen config (status row, weight,
  timer, buttons, reset-hold logic), included by the file above as a
  `packages:` entry. Contains inline comments on the same open items
  listed above.
- `components/timemore_dot/` — the custom BLE component (see above):
  `__init__.py`, `sensor.py`, `binary_sensor.py`, `button.py`,
  `timemore_dot.h`, `timemore_dot.cpp`.
- `secrets.yaml.example` — template for the `secrets.yaml` the ESPHome
  add-on expects (wifi credentials, API encryption key, OTA password).
- `.gitignore` — excludes `secrets.yaml`, the `.esphome/` build cache, and
  `*.bin` build artifacts.

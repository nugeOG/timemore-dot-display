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
  `dot.cpp` directly against NimBLE-Arduino. ESPHome still handles Wi-Fi,
  OTA, display driver, and LVGL as normal — only the scale connection is
  hand-rolled.

## `timemore_dot` component — written, not yet built or tested

The custom ESPHome external component described above now exists at
`components/timemore_dot/`:

```
components/timemore_dot/
├── __init__.py       # ESPHome component registration, pins NimBLE-Arduino 1.4.1
├── sensor.py          # exposes weight + battery_level as sensor: platform
├── binary_sensor.py   # exposes connected as binary_sensor: platform
├── button.py           # exposes tare as button: platform (TareButton)
├── timemore_dot.h
└── timemore_dot.cpp   # ported connect/handshake/decode logic from dot.cpp
```

**Caveat that matters more than the others in this doc**: this code has
never been compiled or run. It's a direct port of the connect → bond →
subscribe → decode flow from the reference driver, using what I believe is
the NimBLE-Arduino ~1.4.x API (`setScanCallbacks`, `secureConnection()`,
the `subscribe()` lambda signature, `writeValue()`'s bool return). NimBLE-
Arduino's API has shifted across major versions — if the build fails on
these specific calls, that's the first thing to check, not a sign the
overall approach (connect/bond/decode logic) is wrong.

Reconnection uses the same `marked_for_reconnect_` pattern named in the
original build/test-order note below, polled from `loop()` on a 5s backoff
rather than acted on inline from a BLE callback.

The code for all of the following now exists, but none of it has been
exercised on hardware — when you actually build, verify in this order
(test the riskiest part first) rather than assuming a clean build means
it all works:
1. Bonding — does `secureConnection()` actually succeed on this board's
   NimBLE stack at all? Everything downstream depends on this. Watch the
   logs (`logger:` is enabled) for "Bonding/secure connection ... failed".
2. Frame decode — do the weight/battery entities in Home Assistant show
   sane, updating values once bonded?
3. Tare — does pressing the on-screen tare button (or the HA `tare_button`
   entity) actually zero the scale?
4. Reconnection — pull the scale out of range or power-cycle it; does the
   scale entity reconnect on its own within a few reconnect-backoff cycles?

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

1. **Exact display/touch driver and pinout** for the specific board
   purchased — still not confirmed. `timemore-dot-display.yaml` currently
   guesses the pinout for the commonly-sold "Cheap Yellow Display"
   (ESP32-2432S028R, ILI9341 + resistive XPT2046) because the original
   listing description matches that board's usual marketing copy closely —
   but this is a guess based on how these boards are typically sold, not a
   confirmed match to the actual unit. Confirm against the board's
   silkscreen/schematic (or just attempt a build and see what doesn't
   initialize) before trusting any pin number in that file.
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
3. **`lvgl.arc.update` action name** — used in the config to update the
   reset-hold progress ring. Matches the general `lvgl.<widget>.update`
   pattern seen in other ESPHome LVGL actions, but not fully confirmed for
   the plain `arc` widget (as opposed to `meter` arc indicators, which
   definitely changed between LVGL 8 and 9). Verify against current
   ESPHome docs / by attempting a build.
4. **`timemore_dot` external component** — written (`components/timemore_dot/`)
   but **never compiled or run** — see the caveat under "`timemore_dot`
   component" above. The NimBLE-Arduino ~1.4.x API surface it's written
   against (`setScanCallbacks`, `secureConnection()`, the `subscribe()`
   callback signature) is the most likely thing to need adjusting on a
   real build.
5. **Touchscreen calibration** — `calibration_x_min/max`/`y_min/max` in
   `timemore-dot-display.yaml` are uncalibrated placeholders (full raw
   ADC range). Run the touchscreen through ESPHome's calibration process
   once it's flashed and paste the real values in.
6. Repo created: [github.com/nugeOG/timemore-dot-display](https://github.com/nugeOG/timemore-dot-display)
   (private).

## Files included in this handoff

- `HANDOFF.md` — this file
- `README.md` — step-by-step build/install instructions via the Home
  Assistant ESPHome add-on, written for a first-time/inexperienced user
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

# Timemore Dot ESP32 Display — project handoff

Goal: an ESP32 + 2.8" touchscreen (240x320, LVGL) acting as a standalone
Bluetooth display/controller for a TIMEMORE Black Mirror Dot coffee scale,
built as an ESPHome project.

This doc exists so implementation work can pick up in Claude Code (or any
fresh session) without re-deriving anything below. Everything here came out
of a planning conversation — none of it has been built or tested on real
hardware yet.

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

## Component not yet written

The actual `timemore_dot` external component (`__init__.py`, `sensor.py`,
`.h`, `.cpp`) implementing the above has **not been written yet** — this
was the planned next step before the conversation moved to the LVGL screen
and repo setup. Suggested structure:

```
components/timemore_dot/
├── __init__.py       # ESPHome component registration
├── sensor.py          # exposes weight + battery_level as sensor: platform
├── timemore_dot.h
└── timemore_dot.cpp   # ported connect/handshake/decode logic from dot.cpp
```

Build/test order (test the riskiest part first):
1. Bare component, log-only — connect, force secure pairing, log decoded
   weight/battery frames. Confirms bonding actually works on this board's
   NimBLE stack before anything else is built.
2. Wire into ESPHome `sensor:` entities (weight, battery) — also makes them
   visible in Home Assistant if useful for debugging.
3. Add tare as a `button:` calling into the component.
4. Reconnection handling — mirror the `markedForReconnection` retry pattern
   from the reference source.

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

- BLE peripherals (this class of device very much included) typically only
  accept one central connection at a time — untested for the Dot
  specifically, but assume it until proven otherwise. Test by connecting
  the custom firmware and the official Timemore app simultaneously.
- If single-connection is confirmed: treat the ESP32 as the sole BLE
  client, and fan data out over the network (ESPHome API / Home Assistant,
  or MQTT) for any other device that wants to see it, rather than trying to
  share the BLE connection itself.
- A true BLE relay (ESP32 as both central to the scale and peripheral
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
   purchased — not yet identified.
2. **Icon font** — the LVGL config uses placeholder codepoints
   (`\U0000E000` etc.) for all icons (wifi, bluetooth, battery, tare, play,
   pause, refresh, coffee, cup). Need to pick and bundle a real icon font
   subset, or switch to PNG image widgets.
3. **`lvgl.arc.update` action name** — used in the config to update the
   reset-hold progress ring. Matches the general `lvgl.<widget>.update`
   pattern seen in other ESPHome LVGL actions, but not fully confirmed for
   the plain `arc` widget (as opposed to `meter` arc indicators, which
   definitely changed between LVGL 8 and 9). Verify against current
   ESPHome docs / by attempting a build.
4. **`timemore_dot` external component** — not written yet (see above).
5. Repo `timemore-dot-esp32-display` was being set up on GitHub but not yet
   confirmed created — no GitHub connector was available to do this
   automatically; needs to be created manually (github.com/new or
   `gh repo create`) if not already done.

## Files included in this handoff

- `HANDOFF.md` — this file
- `scale-display-lvgl.yaml` — the LVGL screen config (status row, weight,
  timer, buttons, reset-hold logic). Contains inline comments on the same
  open items listed above, plus a commented-out section showing how to
  wire live sensors into the labels.

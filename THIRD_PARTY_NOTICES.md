# Third-party notices and acknowledgements

This project builds on work from other open-source projects. Thanks to
the people below — this wouldn't have been possible without their prior
work being public and readable.

## BLE protocol reverse-engineering — gaggimate/esp-arduino-ble-scales

The entire BLE protocol for the TIMEMORE Black Mirror Dot — the service/
characteristic UUIDs, the frame format, the weight/battery decoding, the
tare command bytes, and the requirement to force a secure/bonded
connection before the scale will emit notifications — comes from reading
[`gaggimate/esp-arduino-ble-scales`](https://github.com/gaggimate/esp-arduino-ble-scales),
specifically `src/scales/dot.h` / `dot.cpp`. None of that protocol
knowledge was independently reverse-engineered here; it was ported
directly from that project's driver into
[`components/timemore_dot/timemore_dot.h`](components/timemore_dot/timemore_dot.h)
and
[`components/timemore_dot/timemore_dot.cpp`](components/timemore_dot/timemore_dot.cpp).
If anything about the protocol behaves unexpectedly on real hardware,
that repo is the authoritative source to re-check against, not this one.

`esp-arduino-ble-scales` is part of the [GaggiMate](https://github.com/gaggimate)
espresso machine controller project and is MIT-licensed:

```
MIT License

Copyright (c) 2023 kstam

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## BLE stack — NimBLE-Arduino

The `timemore_dot` component is written against
[h2zero/NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino), pulled
in automatically at build time (see `components/timemore_dot/__init__.py`).
Not code copied into this repo, but the component wouldn't exist without
it — credited here for that reason. Apache License 2.0.

## Fonts — Inter and Material Symbols

Text and icon glyphs are fetched at build time via ESPHome's `gfonts://`
source (see `scale-display-lvgl.yaml`'s `font:` block), not bundled in
this repo, but named here since they're a real design dependency:

- **[Inter](https://rsms.me/inter/)** by Rasmus Andersson — SIL Open Font
  License 1.1.
- **[Material Symbols](https://fonts.google.com/icons)** by Google — used
  for the wifi/bluetooth/battery/tare/play/pause/reset/mode icons — Apache
  License 2.0.

## Everything else

The ESPHome external component structure (`__init__.py`/`sensor.py`/
`binary_sensor.py`/`button.py` + `.h`/`.cpp`), the LVGL screen layout, the
auto-timer design, and the top-level device config are original to this
project, written for it rather than sourced from elsewhere.

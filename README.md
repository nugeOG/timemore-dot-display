# Timemore Dot Display — setup guide

Step-by-step instructions for building and installing this project using
the **ESPHome add-on** in Home Assistant (in some older Home Assistant
versions this add-on is labeled "ESPHome Builder" — it's the same thing).

For the technical background, protocol details, and the list of things
that still need verifying on real hardware, see [HANDOFF.md](HANDOFF.md).
This guide is just "how do I actually get this onto my device."

## Before you start, you'll need

- **Home Assistant OS or Home Assistant Supervised.** Add-ons (including
  the ESPHome add-on) only run on these installation types — if you're on
  Home Assistant Container or Core, this guide doesn't apply to you; you'd
  need to run ESPHome a different way (not covered here).
- **The ESP32 touchscreen board**, plus a USB cable that can transfer data
  (some cheap USB cables are power-only and won't work — if the board
  never shows up as a device in step 5, try a different cable first).
- **A computer with Chrome or Microsoft Edge installed** — the very first
  install has to happen over USB, and that only works through those two
  browsers. It doesn't need to be the same machine Home Assistant itself
  runs on — it just needs to be whichever computer you plug the board
  into and have the browser tab open on.
- **This repo's files, downloaded to your computer.** If you're reading
  this from the GitHub repo, use the green "Code" button → "Download ZIP",
  then unzip it somewhere you can find (e.g. your Desktop).

## Step 1 — Install the ESPHome add-on

1. In Home Assistant, go to **Settings → Add-ons → Add-on Store**.
2. Search for **ESPHome**, click it, then click **Install**. This can take
   a few minutes.
3. Once installed, turn on **Start on boot** and **Show in sidebar**, then
   click **Start**.
4. You should now see an **ESPHome** icon in your Home Assistant sidebar.

## Step 2 — Copy the project files onto your Home Assistant

The ESPHome add-on looks for device configs in a folder on your Home
Assistant called `config/esphome/`. The easiest way to get files into that
folder from your computer is a network file share:

1. Go to **Settings → Add-ons → Add-on Store**, search for **Samba share**,
   install it.
2. Open the Samba share add-on's **Configuration** tab and set a username
   and password (anything you'll remember), then **Save** and **Start**
   the add-on.
3. On your Mac: open **Finder → Go → Connect to Server…** (or press
   `⌘K`), and enter:
   ```
   smb://homeassistant.local
   ```
   If that address doesn't connect, use your Home Assistant's IP address
   instead, e.g. `smb://192.168.1.50`. Click **Connect**, and when
   prompted, enter the username/password you set in step 2.
4. Double-click the **config** share to open it. If there's no `esphome`
   folder inside, create one (name it exactly `esphome`, lowercase).
5. From the unzipped project folder on your computer, copy **everything
   inside it** into that `esphome` folder — `timemore-dot-display.yaml`,
   `scale-display-lvgl.yaml`, `secrets.yaml.example`, and the whole
   `components` folder (with all the files inside it). Make sure
   `components` stays a folder, not flattened — the path
   `esphome/components/timemore_dot/timemore_dot.h` should exist after
   copying.

## Step 3 — Create your secrets file

This project needs your wifi password and a couple of other private
values, kept out of the main config file in a separate `secrets.yaml`.

1. In the `esphome` folder (the one you just copied files into), find
   `secrets.yaml.example`. Duplicate it and rename the copy to
   `secrets.yaml`.
   - **If a `secrets.yaml` already exists there** (from a different
     ESPHome device you've set up before), don't replace it — instead
     open it and add the lines from `secrets.yaml.example` to the bottom
     of your existing file.
2. Open `secrets.yaml` in any text editor and fill in:
   - `wifi_ssid` / `wifi_password` — your home wifi network's name and
     password, exactly as they are (this is case-sensitive).
   - `api_encryption_key` — a random key so Home Assistant can talk to
     the device securely. Generate one by opening **Terminal** on your
     Mac (Applications → Utilities → Terminal) and running:
     ```bash
     python3 -c "import os,base64;print(base64.b64encode(os.urandom(32)).decode())"
     ```
     Copy the string it prints and paste it in as the value.
   - `ota_password` — any password you choose, used for future wireless
     updates.
   - Leave the commented-out `mqtt_*` lines alone unless you specifically
     want MQTT — they're not needed for Home Assistant.
3. Save the file.

## Step 4 — Find your device in the ESPHome dashboard

1. Click **ESPHome** in the Home Assistant sidebar.
2. You should see a card for **Timemore Dot Display**. If it's not there,
   click the add-on's refresh icon, or restart the ESPHome add-on from
   **Settings → Add-ons → ESPHome → Restart**.
3. If the dashboard shows a red error instead of the device card, open it
   to read the error — it's usually a typo in `secrets.yaml`, or the
   `components` folder not having copied over completely (see Step 2).

## Step 5 — First install, over USB

The very first time, the firmware has to be loaded onto the board over a
USB cable — after that, all future updates can happen wirelessly.

1. Plug the ESP32 board into **the computer you're using right now**
   (the one with your Chrome/Edge browser open) via USB.
2. In the ESPHome dashboard, click **Install** on the Timemore Dot Display
   card.
3. Choose **Plug into this computer**.
   - If you don't see this option, you're either not using Chrome/Edge,
     or you're viewing Home Assistant from a phone/tablet rather than
     this computer — open the same Home Assistant URL in Chrome on the
     computer the board is physically plugged into.
4. A browser popup will ask you to pick a serial port. Choose the one
   that appeared when you plugged the board in (it'll often show as
   something like "USB Serial" or a chip name such as CP2102 or CH340).
   - **If no port shows up at all**: your Mac may be missing a driver for
     that board's USB-to-serial chip. Look up the chip name printed on
     the board (often CP2102 or CH340G) and install the matching macOS
     driver, then unplug/replug the board and try again.
5. Click **Connect**. The add-on will now compile the firmware (this can
   take several minutes the first time) and then flash it to the board —
   watch the on-screen log to follow along. A successful flash ends with
   the device rebooting and printing new log lines on its own.
6. Once flashing finishes, leave the board plugged in for a minute or two
   while it connects to your wifi network using the credentials from
   `secrets.yaml`.

## Step 6 — Add the device to Home Assistant

1. Go to **Settings → Devices & Services**. Within a minute or so you
   should see a notification that a new device was discovered, or find it
   under the **Discovered** section of that page.
2. Click **Configure** (or **Add**) next to it.
3. When asked for an **encryption key**, paste in the exact
   `api_encryption_key` value from your `secrets.yaml`.
4. Finish the setup. You should now have these entities: **Weight**,
   **Battery**, **Wifi Signal**, **Scale Connected**, and a **Tare**
   button — all under a device named "Timemore Dot Display".

## Step 7 — Future updates are wireless

Any time you change the YAML files and want to update the board again,
you no longer need the USB cable — back in the ESPHome dashboard, click
**Install** and choose **Wirelessly** instead. This only works if the
board is already on your wifi network.

## Before you expect this to fully work

This project hasn't been run on real hardware yet, so a few things need
checking once you have a physical board in hand — full details are in
[HANDOFF.md](HANDOFF.md), but in short:

- The exact touchscreen/display pins in `timemore-dot-display.yaml` are a
  best guess for a commonly-sold board — if the screen stays blank, this
  is the first thing to check.
- Touch won't be accurately positioned until you run the touchscreen
  calibration process and update the placeholder values in the same file.
- The Bluetooth connection to the actual scale (pairing, weight readings,
  tare) has never been tested — watch the ESPHome logs (in the dashboard,
  click the device → **Logs**) for lines starting with `[timemore_dot]`
  to see what's happening when it tries to connect.

## Troubleshooting

- **Board doesn't show up as a USB port option** — try a different cable
  (it needs to support data, not just power), and check whether you need
  a USB-to-serial driver for your Mac (see Step 5).
- **Wifi never connects after flashing** — double-check `wifi_ssid` and
  `wifi_password` in `secrets.yaml` for typos, then plug the board back
  into USB and run **Install → Plug into this computer** again (a board
  that can't reach wifi at all can't receive an update wirelessly).
- **ESPHome dashboard shows a build error mentioning
  `components/timemore_dot`** — the `components` folder likely didn't
  copy over completely in Step 2. Reconnect via Samba and confirm all six
  files are present inside `esphome/components/timemore_dot/`.
- **Device appears in Home Assistant but entities say "Unavailable"** —
  most likely the board can't connect to the scale over Bluetooth yet.
  Check the device's logs in the ESPHome dashboard for `[timemore_dot]`
  lines, and see HANDOFF.md's BLE protocol notes.

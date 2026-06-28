# Kinetic Sculpture firmware

ESP32-C3 firmware for the counter-rotating moiré sculpture (TMC2209 stepper).
Single sketch, three build variants, web UI + wireless updates.

## Files

- `kinetic_sculpture.ino` - the firmware. This is the file you flash.
- `page.h` - the web app (HTML/CSS/JS) as a PROGMEM string. Must sit in the same
  folder as the .ino. It is a separate header on purpose: the Arduino toolchain's
  ctags prototype generator cannot parse a C++11 raw string inside a .ino and
  fails the build with "function does not name a type"; keeping the page in an
  #included header avoids that. Do not paste the page back into the .ino.
- `build.yml` - GitHub Actions workflow that compiles the WiFi build and
  publishes `firmware.bin` to a rolling release, so boards can self-update.

The sketch is preset to the WiFi build (`#define INPUT_TOF_WIFI` active at the
top), which gives the web UI, hand-gesture (ToF) control, and OTA.

## Flash a new board (one-time, over USB)

1. Arduino IDE: install the ESP32 boards package (Boards Manager -> "esp32").
2. Install these libraries (Library Manager): FastAccelStepper, TMCStepper,
   WebSockets (by Markus Sattler), ArduinoJson. The rest ship with the ESP32 core.
3. Open `kinetic_sculpture.ino`. Keep `page.h` in the same folder (the IDE shows
   it as a second tab). Leave `#define INPUT_TOF_WIFI` as the active build flag.
4. Tools menu:
   - Board: "ESP32C3 Dev Module"
   - Partition Scheme: "Minimal SPIFFS (1.9MB APP with OTA...)"  <- must be an OTA scheme
   - Port: the board's COM port
5. Upload over USB. This first flash has to be USB; everything after can be wireless.

Repeat for each board. Each one comes up with a unique name automatically
(see below), so there is nothing per-board to hand-edit before flashing.

## First boot and joining WiFi

- On first boot the board starts its own access point named
  `KineticSculpture-XXXX` (XXXX = unique per board) at `192.168.4.1`.
- Join that AP, the control page opens automatically.
- Go to Setup -> Join wifi, enter your network, Save. The board reboots onto
  your WiFi and is reachable at `sculpture-XXXX.local`.

Because the AP name and hostname both carry a per-board suffix derived from the
chip ID, multiple boards never collide. You can rename any board in Setup if you
want friendlier names (e.g. `sculpture-lobby`).

## Wireless updates (push to GitHub, update from phone)

One-time repo setup:

1. Create a public GitHub repo.
2. Put `kinetic_sculpture.ino` and `page.h` at the repo root, and `build.yml` at
   `.github/workflows/build.yml`. Upload the files (do not copy-paste them, they
   are large and paste can corrupt the embedded web page).
3. Settings -> Actions -> General -> Workflow permissions -> Read and write.
4. Push. The Actions tab builds and publishes a `latest` release with
   `firmware.bin`.

Then on each board: Setup -> Firmware -> paste the release URL once -> install:

```
https://github.com/<you>/<repo>/releases/download/latest/firmware.bin
```

After that, the update loop for every board is: edit the sketch (bump
`FW_VERSION`), push, wait for the green check, then tap Download & install on
each board. All boards pull the same binary, so one push updates the fleet.

Notes:
- GitHub pull needs the board on an internet-connected network (STA mode), not AP.
- Keep boards powered during an install; a power cut mid-flash needs a USB reflash.
- The repo must be public for boards to download the asset without a token.

## Build variants (if you ever need them)

At the top of the sketch, exactly one of these is active:

- `INPUT_TOF_WIFI` - ToF gestures + web UI + OTA (default, recommended)
- `INPUT_TOF`      - ToF gestures only, no networking
- `INPUT_POT_BTN`  - potentiometer + buttons, no networking

The GitHub workflow forces `INPUT_TOF_WIFI` at build time regardless of what is
committed, so the published binary is always the WiFi build.

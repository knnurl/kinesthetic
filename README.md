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
  `Kinesthetic-XXXX` (XXXX = unique per board) at `192.168.4.1`.
- Join that AP, the control page opens automatically.
- Go to Setup -> Join wifi, enter your network, Save. The board reboots onto
  your WiFi and is reachable at `sculpture-XXXX.local`.

Because the AP name and hostname both carry a per-board suffix derived from the
chip ID, multiple boards never collide. You can rename any board in Setup if you
want friendlier names (e.g. `sculpture-lobby`).

## Using it

The control page has tabs across the top. The essentials:

- **Control** - the speed/direction slider (bidirectional, centre is stop) and the
  movement mode grid. Enable the motor with the big button.
- **Motion** - tune the feel of each mode (cycle durations, ramp/dwell shaping) or
  pick a preset (Calm, Balanced, Lively, Hypnotic). Also the mode queue, a timed
  playlist that steps through modes and loops.
- **Light** - LED ring modes (if fitted): Solid, Glow (breathes with real speed),
  Chase (spins with the discs), Rainbow.
- **Fleet** - control several sculptures from one page, plus the swarm designer.
- **Setup / Help** - networking, appearance, password, firmware, and a gesture and
  mode reference.

### Movement modes

Manual (you drive), Breathe, Sweep, Wander, Tide, Pendulum, Heartbeat, Stutter.
In every mode except Manual the slider's distance from centre sets the top speed
the movement builds to, and which side sets its direction. Tap the `?` in the mode
grid for a full description of each.

Hand gestures (ToF sensor): move your hand to control speed, hold still then
withdraw to change mode (2-5s), enable/disable (5-15s), or reset networking (15s+).

### Fleet: control many sculptures from one page

Open any sculpture's page, go to Fleet, and add the others by address
(`sculpture-XXXX.local` or IP). Peers sharing this board's interface password
unlock automatically. The roster is stored on the boards themselves, so opening
any device's page shows the whole fleet. "Mirror my controls" sends every change
you make to all of them; "Enable all" / "Stop all" and "Send mode + speed to all"
are one-shot broadcasts.

### Swarm: many sculptures as one wall

Imagine a grid of these on a wall moving as a single organism. The swarm turns
motion into a *field function* of each device's position and a shared clock, so
the wall runs travelling waves, ripples, and flocking instead of each unit doing
its own thing.

How it works: one device is the **conductor**. It broadcasts a small UDP clock
beacon on the LAN carrying the pattern and its parameters; every other device
syncs its clock to the conductor and runs the same maths locally at its own stored
wall position. The result is phase-locked with almost no network traffic, and it
keeps running even if you close the control page.

To set it up (all boards on the same WiFi):

1. Fleet tab -> add the other sculptures.
2. In the **Swarm** card, tap **Link devices** once (shares a swarm key), then drag
   each dot on the layout to where that sculpture physically sits, or tap
   **Arrange in a row**.
3. Mark one sculpture as conductor (the **C** button in the list, or "this
   sculpture is the conductor").
4. Pick a pattern (Unison, Wave, Ripple, Cascade, Flock), set amplitude and the
   pattern sliders, and tap **Engage swarm**. The live preview animates the exact
   motion the wall will run.

Once engaged, the Control tab's speed slider drives the whole wall's amplitude in
one move (toggle in the Swarm card, on by default). With the ToF sensor, a hand
wave at any one sculpture radiates a ripple across all of them.

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

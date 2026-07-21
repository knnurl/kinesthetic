/*
 * KINETIC SCULPTURE FIRMWARE  ESP32-C3 Super Mini + TMC2209 (UART)
 *
 * ONE .ino file. A single #define build flag selects the input mode.
 * Define EXACTLY ONE of:
 *     INPUT_POT_BTN     pot + buttons only
 *     INPUT_TOF         ToF10120 gesture sensor only, no WiFi
 *     INPUT_TOF_WIFI    ToF10120 gesture sensor + WiFi web app
 *
 * LIBRARIES (Arduino Library Manager):
 *     FastAccelStepper   (gin66)
 *     TMCStepper         (teemuatlut)
 *     Wire               (built-in, ToF versions)
 *     WiFi               (built-in, ToF+WiFi only)
 *     WebSocketsServer   (Markus Sattler / links2004, ToF+WiFi only)
 *     ArduinoJson v7     (Benoit Blanchon, ToF+WiFi only)
 *
 * STYLE NOTE: shared motion core and TMC2209 init are NEVER inside #ifdef.
 * Version specific code is wrapped in clearly labelled #ifdef blocks.
 * No em dashes anywhere. Assumptions are tagged with // ASSUMPTION:
 */

// ============================================================
//  BUILD FLAG  (pick exactly one)
// ============================================================
// #define INPUT_TOF
// #define INPUT_POT_BTN
#define INPUT_TOF_WIFI

// Optional WS2812 ring lighting. Comment out to build without LEDs; the LED
// tab in the web app hides itself automatically when the firmware lacks it.
#define ENABLE_LEDS

#ifdef ENABLE_LEDS
#define LEDS_JSON "true"
#else
#define LEDS_JSON "false"
#endif

#if (defined(INPUT_POT_BTN) + defined(INPUT_TOF) + defined(INPUT_TOF_WIFI)) != 1
#error "Define exactly one input mode: INPUT_POT_BTN, INPUT_TOF, or INPUT_TOF_WIFI"
#endif

// Convenience: any build that uses the ToF sensor
#if defined(INPUT_TOF) || defined(INPUT_TOF_WIFI)
#define USES_TOF
#endif

// ============================================================
//  INCLUDES
// ============================================================
#include <Arduino.h>
#include "FastAccelStepper.h"
#include <TMCStepper.h>
#include <Preferences.h>   // NVS persistence of mode + ceiling

#ifdef ENABLE_LEDS
#include <Adafruit_NeoPixel.h>
#endif

#ifdef USES_TOF
#include <Wire.h>
#endif

#ifdef INPUT_TOF_WIFI
#include <WiFi.h>
#include <WiFiUdp.h>       // swarm clock beacon + gesture pings (built-in)
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <HTTPUpdate.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#endif

// ============================================================
//  PINS  (fixed across all versions)
// ============================================================
// Shared motion pins
const int UART_TX = 4;     // -> 1k -> TMC2209 UART pin
const int UART_RX = 10;    // -> TMC2209 UART pin direct
const int DIR_PIN = 5;
const int STEP_PIN = 6;
const int EN_PIN = 7;      // TMC2209 ENN, active LOW

#ifdef INPUT_POT_BTN
const int EN_BTN = 1;      // INPUT_PULLUP, hold to enable motor
const int MODE_BTN = 2;    // INPUT_PULLUP, press to cycle modes forward
const int POT_PIN = 3;     // 10k pot wiper
#endif

#ifdef USES_TOF
// ON PCB >> 3 | 1 | GND 
const int SDA_PIN = 1;     // ToF10120 I2C data
// GPIO2 unused on ToF versions
const int SCL_PIN = 3;     // ToF10120 I2C clock
#endif

#ifdef ENABLE_LEDS
// HARDWARE NOTE: the ESP32-C3 has only two RMT TX channels and FastAccelStepper
// claims one for step generation, leaving exactly one for WS2812 output. Two
// separate LED data pins therefore cannot work on this chip alongside the
// stepper. Chain ring B's DIN to ring A's DOUT and drive the whole chain from
// GPIO20; the rings remain independently addressable as segments. GPIO21 is
// intentionally unused.
const int LED_PIN = 20;
#define LED_COUNT_A 24     // ring A pixel count (first in the chain)
#define LED_COUNT_B 24     // ring B pixel count (chained after A)
#define LED_COUNT   (LED_COUNT_A + LED_COUNT_B)
#endif

// ============================================================
//  CONSTANTS
// ============================================================
#define FREQ_MIN   8        // steps/sec, below this the motor is treated as stopped.
                            // Was 100, which silently discarded the whole 0-100 st/s
                            // band while telemetry still displayed it. TMC2209 in
                            // StealthChop with intpol handles single-digit rates fine.
#define FREQ_MAX   1800     // steps/sec maximum. Was 4000; observed usable top
                            // speed is 45% of that, so the range is clamped and
                            // every ceiling-relative mapping rescales with it.
#define MANUAL_MIN_MOVE (FREQ_MIN + 2) // steps/sec at the slider's lowest non-zero
                            // step, so even 1% produces real motion (no dead low
                            // end). Tied to FREQ_MIN so it can never fall back
                            // under the stop threshold if that changes again;
                            // it was a stale hardcoded 200 from the FREQ_MIN=100
                            // era, which made 1% far faster than it needed to be.
#define SMOOTH_TIME_UP   0.5f   // sec, responsiveness when speeding up (lower = snappier)
#define SMOOTH_TIME_DOWN 1.1f   // sec, when slowing down (higher = gentler wind down)
#define MAX_ACCEL  10000    // steps/sec^2, hard ceiling on rate of change
#define DEAD_BAND  180      // ADC counts (pot version only)

// Minimum speed ceiling for the autonomous modes (BREATHE, SWEEP, WANDER) so the
// sculpture always keeps visibly moving and cannot be starved to a standstill by
// the ToF, pot, or app. MANUAL is exempt and keeps its full range including stop.
// Kept above FREQ_MIN / 0.15 so BREATHE's trough (0.15 x ceiling) never stalls.
#define MODE_MIN_SPEED 120  // steps/sec, default floor for the auto modes. Keeps
                            // their quiet phases visibly alive; every mode trough
                            // (tide's 8% is the lowest) still clears FREQ_MIN.

// Envelope shaping for the autonomous modes. Higher values make the speed
// profile linger longer at the low end (BREATHE) and around the zero crossing
// at each reversal (SWEEP). 1.0 = the original unshaped curve.
#define BREATHE_SHAPE 4.0f  // power on sin: sharper peak, broader low dwell
#define SWEEP_SHAPE   2.2f  // power on ramp: slow crawl away from each reversal

// Cycle timing for the autonomous modes (longer = more languid).
#define BREATHE_PERIOD_MS 20000   // one full breath in/out
#define SWEEP_HALF_MS     10000   // one ramp 0 -> peak (a full sweep is 2x this)
#define WANDER_RATE       0.0005f // time scale for the noise drift (lower = slower)
#define TIDE_PERIOD_MS    480000  // one full tide swell (8 min); direction alternates per cycle
#define PEND_PERIOD_MS    12000   // one full pendulum swing there and back
#define BEAT_PERIOD_MS    4000    // one heartbeat (lub-dub + rest)
#define STUT_PERIOD_MS    900     // base dwell per stutter step (varies 0.5x-1.5x)

#define STEPS_PER_REV 3200  // 200 full steps x 16 microsteps  (informational)
const float E_CONST = 2.718281828f;

// Compile-time invariants: misconfiguration fails the build, not the sculpture.
static_assert(FREQ_MIN < FREQ_MAX, "FREQ_MIN must be below FREQ_MAX");
static_assert(MODE_MIN_SPEED <= FREQ_MAX, "MODE_MIN_SPEED must be within range");
static_assert(MODE_MIN_SPEED * 15 / 100 >= FREQ_MIN,
              "BREATHE trough (0.15 x ceiling) must clear the FREQ_MIN stall threshold");

// ============================================================
//  TMC2209
// ============================================================
#define DRIVER_ADDRESS 0b00     // MS1=GND MS2=GND
#define R_SENSE        0.11f
TMC2209Stepper driver(&Serial1, R_SENSE, DRIVER_ADDRESS);
bool uartOk = false;

// ============================================================
//  FastAccelStepper
// ============================================================
FastAccelStepperEngine engine = FastAccelStepperEngine();
FastAccelStepper *stepper = NULL;

// ============================================================
//  SHARED STATE
// ============================================================
enum Mode : uint8_t { MANUAL = 0, BREATHE = 1, SWEEP = 2, WANDER = 3 };
uint8_t mode = MANUAL;

int  currFreq = 0;          // signed steps/sec, smoothed tracker output
int  appliedFreq = 0;       // signed steps/sec actually commanded to the stepper
                            // (0 whenever the FREQ_MIN cutoff holds it still), so
                            // telemetry never shows motion that is not happening
int  targetFreq = 0;        // signed steps/sec requested this tick
bool motorEnabled = false;  // tracked driver enable state

float trackPos = 0.0f;      // smoothed signed frequency (tracker output)
float trackVel = 0.0f;      // tracker velocity state
uint32_t softStartAt = 0;   // millis of the enable edge; 0 = not soft-starting
uint32_t enableGraceUntil = 0; // gentler speed ramp until this time after enable
uint8_t  softStage = 0;

int  maxSpeedCeiling = 0;         // ToF/pot/app set this ceiling for the auto modes.
                                  // Defaults to 0 (slider at rest) so an out-of-box
                                  // unit cannot launch at full speed on first enable;
                                  // auto modes still animate via the min-speed floor.
int  manualSpeed = 0;             // signed, frozen between MANUAL speed gestures
int  lastSliderPct = 0;           // exact percent last sent by the app; echoed back
                                  // verbatim in telemetry so integer map() rounding
                                  // cannot nudge other clients' sliders by 1%
int  lastManualDir = 1;           // +1 or -1, inherited by BREATHE

// Health / fault flags, surfaced to Serial, telemetry, and the response logic.
bool     faultTmcComm  = false;   // TMC2209 UART read-back not responding (motor still
                                  // runs open-loop on STEP/DIR; this only means no telemetry)
bool     faultOvertemp = false;   // TMC2209 overtemperature prewarning active
bool     faultTof      = false;   // a PRESENT ToF sensor died; never set when none is fitted
bool     tofPresent    = false;   // a ToF ever answered, so its silence is a real fault
bool     tofControl    = false;   // false = GUI/swarm owns motion; true = the hand (ToF) has
                                  // seized this sculpture (a 3-8s hold latches it either way)
int      tofDir        = 1;       // +1/-1 direction for hand speed control; a double tap flips it
uint16_t sgLoad        = 0;       // StallGuard load (informational)
float    speedDerate   = 1.0f;    // health back-off multiplier on all motion
bool     otaActive     = false;   // true while an OTA update is being written

Preferences prefs;                // NVS store for mode + ceiling

// Runtime motion config (defaults from the #defines above). Tunable live from
// the web UI in the WiFi build; other builds just use the stored or default
// values. Read by the mode generators and the motion tracker every tick.
struct MotionCfg {
  uint32_t breatheMs   = BREATHE_PERIOD_MS;
  uint32_t sweepHalfMs = SWEEP_HALF_MS;
  float    wanderRate  = WANDER_RATE;
  uint32_t tideMs      = TIDE_PERIOD_MS;
  uint32_t pendMs      = PEND_PERIOD_MS;
  uint32_t beatMs      = BEAT_PERIOD_MS;
  uint32_t stutMs      = STUT_PERIOD_MS;
  float    smoothUp    = SMOOTH_TIME_UP;
  float    smoothDown  = SMOOTH_TIME_DOWN;
  float    breatheShape= BREATHE_SHAPE;
  float    sweepShape  = SWEEP_SHAPE;
  int      minSpeed    = MODE_MIN_SPEED;  // runtime floor for auto modes; 0 = allowed to stop
} cfg;

// Mode queue: an optional playlist that auto-advances the mode on a timer.
#define QUEUE_MAX 8
struct QStep { uint8_t mode; uint16_t secs; };
QStep    queueSteps[QUEUE_MAX];
uint8_t  queueLen = 0;
bool     queueEnabled = false;
uint8_t  queueIdx = 0;
uint32_t queueStepStart = 0;
bool     queueOffPending = false;   // set when firmware cancels the queue; the
                                    // WiFi build broadcasts it once to sync the UI

// ---------- Swarm (multi-device choreography) ----------
// Motion becomes a field function: target = amplitude * pattern(x, y, sharedTime).
// One device (the conductor) broadcasts a UDP clock beacon carrying the pattern
// parameters; followers sync their clock to it and run the same math at their own
// stored wall position, so many sculptures move as one installation with zero
// per-tick network traffic. The pattern math is shared core; only the network
// layer is WiFi-build specific. In the other builds SWARM simply never engages
// because no beacon ever arrives.
#define SWARM_UDP_PORT      47269
#define SWARM_MAGIC         0x4B535731UL  // 'KSW1'
#define SWARM_TIMEOUT_MS    3000    // no beacon for this long: follower holds at 0
#define SWARM_BEACON_MS     500     // conductor beacon cadence (2 Hz)
#define SWARM_HELLO_MS      4000    // presence announce cadence (network discovery)
#define SWARM_PEERS_MAX     24      // discovered devices remembered
#define SWARM_PEER_TTL_MS   15000   // drop a peer not heard from in this long
#define SWARM_PATTERNS      6       // unison, wave, ripple, cascade, flock, mirror
#define SWARM_MIRROR        5       // pattern id that samples the sensor depth field
#define SWARM_PING_MAX      4       // concurrent gesture ripples remembered
#define SWARM_PING_PERIOD_S 4.0f    // gesture ripple ring period
#define SWARM_PING_SPACING  0.4f    // gesture ripple ring spacing (wall units)
#define SWARM_PING_DECAY_S  8.0f    // gesture ripple lifetime

// ---------- Sensor modulation (VL53L5CX node -> swarm) ----------
// A dedicated sensor node broadcasts SwarmCue (presence + blob centroid +
// motion) and, for the MIRROR pattern, SwarmField (an 8x8 depth image). These
// MODULATE the swarm; they never write swarmAmp (the beacon owns that), so the
// two cannot fight. All of it is optional: on sensor silence the wall eases back
// to its designed choreography. Shared state so modeSwarm reads it; only the
// receive path is WiFi specific. See SENSOR_PLAN.md.
#define SENSE_TIMEOUT_MS 2000       // no cue/field for this long: relax to ambient
#define SENSE_IDLE_GAIN  0.35f      // wall energy when the sensor sees nobody
#define SENSE_FIELD_W    8
#define SENSE_FIELD_H    8

bool     senseEnabled  = true;              // "respond to sensor" toggle, NVS sw_sense
float    senseGain     = 1.0f;              // eased presence multiplier, applied in modeSwarm
float    senseTarget   = 1.0f;              // raw target senseGain eases toward
bool     senseCueFresh = false;             // a cue arrived within SENSE_TIMEOUT_MS
float    senseCx = 0.5f, senseCy = 0.5f;    // primary blob centroid, wall coords (X, Y)
float    senseVx = 0.0f, senseVy = 0.0f;    // centroid velocity, wall units/sec (reserved)
float    senseDepth = 0.0f;                 // primary blob distance, normalised 0..1 (1 = closest)
float    sensePresence = 0.0f;              // how strongly a hand is present, 0..1
uint8_t  senseBlobs = 0, sensePrevBlobs = 0;
uint32_t senseLastCue = 0;                  // millis() of last accepted cue
uint8_t  senseField[SENSE_FIELD_W * SENSE_FIELD_H] = { 0 };  // 0 empty, 1..255 near
uint32_t senseLastField = 0;                // millis() of last accepted field frame

// Routing matrix: each sensor axis (X, Y, Depth) maps to one destination with a
// range + invert. Presets in the web UI are just default fills of this. The
// conductor broadcasts it (SwarmSenseCfg) so the whole wall shares one routing.
enum SenseDst : uint8_t {
  SD_NONE = 0, SD_AMP, SD_FOCUSX, SD_FOCUSY, SD_SPOT, SD_PERIOD, SD_WAVELEN, SD_DIR, SD_COUNT
};
struct SenseCfg {                 // floats first so the wire/NVS layout stays 4-aligned
  float   lo[3], hi[3];           // output range per axis X, Y, Z
  float   spotR;                  // spotlight radius, wall units
  uint8_t mode;                   // preset id (informational; 6 = custom)
  uint8_t inv;                    // invert bits: b0 X, b1 Y, b2 Z
  uint8_t dst[3];                 // destination per axis X, Y, Z
};
SenseCfg scfg = { { 0, 0, 0 }, { 1, 1, 1 }, 0.3f, 0, 0, { SD_NONE, SD_NONE, SD_NONE } };

// Derived control values, eased once per tick in senseTask, read in modeSwarm.
float    mFocusX = 0.5f, mFocusY = 0.5f;    // eased focus point (spotlight / ripple source)
float    mSpot = 0.0f;                      // eased spotlight strength 0..1 (0 = layer off)
bool     mFocusActive = false;              // focus is being driven by the sensor this tick
bool     mPov[3] = { false, false, false }; // pattern param p0/p1/p2 override active
float    mPval[3] = { 0, 0, 0 };            // override values

float    swarmX = 0.5f, swarmY = 0.5f;  // wall position 0..1, NVS sw_x / sw_y
bool     swarmConductor = false;        // NVS sw_cond
uint32_t swarmKey = 0;                  // shared packet key, NVS sw_key
bool     swarmActive = false;           // engaged (runtime only, never persisted)
bool     swarmMuted  = false;           // local user override: ignore active beacons
uint8_t  swarmPatternId = 1;
float    swarmAmp = 0.5f;
float    swarmP[4] = { 12.0f, 0.8f, 0.0f, 0.0f };
bool     swarmCeilAuto = false;         // ceiling was auto-seeded from amplitude and
                                        // keeps following it until a human sets a cap
int32_t  swarmClkOff = 0;               // sharedMillis() = millis() + offset
uint32_t swarmLastBeacon = 0;           // millis() of last accepted beacon
uint8_t  swarmReleaseBeacons = 0;       // conductor: beacons still to send after release
struct SwarmPingSlot { uint32_t t; float x, y; bool used; };
SwarmPingSlot swarmPings[SWARM_PING_MAX] = {};
uint32_t sharedMillis() { return millis() + (uint32_t)swarmClkOff; }
#ifdef INPUT_TOF_WIFI
void swarmGesturePing();   // defined with the network layer below
#endif

#define CROSSFADE_MS 1000          // mode change envelope blend duration

// Set to 0 to fully de-energise on disable (true power off, lower heat, no
// holding torque). Safe here because the sculpture geometry is balanced, so
// there is no gravity load to backdrive when current cuts. Set to 1 to keep
// the driver energised at hold current if an unbalanced piece needs holding.
#define HOLD_TORQUE_ON_DISABLE 0

// Motor current (TMC2209 over UART) and enable soft-start. Energizing a
// de-energized stepper at full current yanks the rotor to the nearest detent;
// instead the enable edge powers the coils at SOFT_START_MA and steps up to
// RUN_CURRENT_MA over SOFT_START_MS while motion is held at zero, so the rotor
// settles gently. ENABLE_GRACE_MS then stretches the speed ramp so an enable
// with the slider already up builds speed deliberately instead of leaping.
#define RUN_CURRENT_MA   900
#define HOLD_MULT        0.3f
#define SOFT_START_MA    150
#define SOFT_START_MS    450
#define ENABLE_GRACE_MS  1200

// ============================================================
//  MOTION CORE  (shared, never inside #ifdef)
//  Signed frequency. DIR flips only when speed passes through zero.
//  A critically damped second order tracker eases toward the target, so motion
//  is jerk limited at both ends of a move and never jumps or overshoots.
// ============================================================

// Unity-style SmoothDamp: no overshoot, jerk limited. smoothTime sets the feel
// (caller passes up or down constant), maxAccel is a hard ceiling on rate of
// change. Frame rate independent via dt.
float smoothDamp(float current, float target, float &vel,
                 float smoothTime, float maxAccel, float dt) {
  if (smoothTime < 0.0001f) smoothTime = 0.0001f;
  float omega = 2.0f / smoothTime;
  float x = omega * dt;
  float ex = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
  float change = current - target;
  float originalTo = target;
  float maxChange = maxAccel * smoothTime;          // clamp implied velocity
  if (change > maxChange) change = maxChange;
  else if (change < -maxChange) change = -maxChange;
  target = current - change;
  float temp = (vel + omega * change) * dt;
  vel = (vel - omega * temp) * ex;
  float out = target + (change + temp) * ex;
  if ((originalTo - current > 0.0f) == (out > originalTo)) {   // clamp overshoot
    out = originalTo;
    vel = (out - originalTo) / dt;
  }
  return out;
}

void applyMotion() {
  // --- Enable soft-start: settle the rotor before moving it ---
  static bool prevEnabled = false;
  uint32_t nowMs = millis();
  if (motorEnabled && !prevEnabled) {
    softStartAt = nowMs ? nowMs : 1;          // 0 means idle, avoid the collision
    softStage = 0;
    enableGraceUntil = nowMs + SOFT_START_MS + ENABLE_GRACE_MS;
#if !HOLD_TORQUE_ON_DISABLE
    if (uartOk) driver.rms_current(SOFT_START_MA, 1.0f);  // weak first grab
#endif
    if (stepper) { stepper->enableOutputs(); stepper->stopMove(); }
  }
  prevEnabled = motorEnabled;

  bool softStarting = softStartAt && (nowMs - softStartAt < SOFT_START_MS);
  if (softStarting) {
#if !HOLD_TORQUE_ON_DISABLE
    // Step the coil current up in five stages across the window.
    uint8_t stage = (uint8_t)((nowMs - softStartAt) * 5 / SOFT_START_MS);
    if (stage != softStage && uartOk) {
      softStage = stage;
      driver.rms_current(SOFT_START_MA + (RUN_CURRENT_MA - SOFT_START_MA) * stage / 5, HOLD_MULT);
    }
#endif
  } else if (softStartAt) {
    softStartAt = 0;
#if !HOLD_TORQUE_ON_DISABLE
    if (uartOk) driver.rms_current(RUN_CURRENT_MA, HOLD_MULT);  // land exactly on spec
#endif
  }

  if (!motorEnabled) targetFreq = 0;   // disabled means ease down to a stop
  if (softStarting)  targetFreq = 0;   // hold still while the field comes up

  // Real elapsed time keeps the tracker independent of loop rate.
  static uint32_t lastUs = 0;
  uint32_t nowUs = micros();
  float dt = (lastUs == 0) ? 0.01f : (nowUs - lastUs) * 1e-6f;
  lastUs = nowUs;
  if (dt < 0.001f) dt = 0.001f; else if (dt > 0.05f) dt = 0.05f;

  // Slowing down (commanded magnitude below current) uses the gentler time
  // constant; speeding up uses the snappier one. This makes wind down from full
  // speed ease off smoothly without making acceleration feel sluggish.
  float st = (fabsf((float)targetFreq) < fabsf(trackPos)) ? cfg.smoothDown : cfg.smoothUp;
  if (nowMs < enableGraceUntil) st *= 2.5f;   // deliberate, unhurried first spin-up
  trackPos = smoothDamp(trackPos, (float)targetFreq, trackVel, st, MAX_ACCEL, dt);
  currFreq = (int)lroundf(trackPos);

  appliedFreq = 0;            // set below only when the stepper is actually driven
  if (!stepper) return;

  // Once fully stopped and disabled, either hold at hold current or cut current.
  // The motor has already eased to zero before reaching here, so there is no
  // lurch on the rotation. The only abrupt risk was the instant current cut on
  // an unbalanced load, which holding torque removes.
  if (!motorEnabled && currFreq == 0) {
    trackVel = 0.0f;
#if HOLD_TORQUE_ON_DISABLE
    stepper->enableOutputs();   // stay energised; TMC drops to hold current when idle
    stepper->stopMove();
#else
    stepper->disableOutputs();  // fully de-energise
    stepper->stopMove();
#endif
    return;
  }
  if (motorEnabled) stepper->enableOutputs();

  int mag = abs(currFreq);
  if (mag < FREQ_MIN) {
    stepper->stopMove();          // at or near zero, hold still (DIR safe to change next)
    return;
  }

  stepper->setSpeedInHz((uint32_t)mag);
  if (currFreq > 0) stepper->runForward();
  else              stepper->runBackward();
  appliedFreq = currFreq;
}

// ============================================================
//  MODE ENVELOPES  (shared)
//  Each generator returns a SIGNED target frequency for this tick, given the
//  current time, the (already floored) speed ceiling, and whether the mode was
//  just entered. Adding a mode is: write a generator, add one table row, and
//  add a button to the web app. The cycling and telemetry adapt automatically.
// ============================================================
typedef int (*ModeFn)(uint32_t now, int cap, bool entered);

int modeManual(uint32_t now, int cap, bool entered) {
  (void)now; (void)cap; (void)entered;
  // Speed and direction come straight from the input layer.
  // Held (frozen) between speed gestures in the ToF versions.
  return manualSpeed;
}

int modeBreathe(uint32_t now, int cap, bool entered) {
  (void)entered;
  // Sinusoidal envelope, never fully stops. Direction inherited from MANUAL.
  // breatheShape sharpens the peak and broadens the low dwell near the trough.
  const float period = (float)cfg.breatheMs;
  float ph = (now % cfg.breatheMs) / period;  // 0..1
  float s = sinf(ph * PI);                     // 0..1 over the breath
  float env = 0.15f + 0.85f * powf(s, cfg.breatheShape);   // 0.15..1.0
  int f = (int)(cap * env);
  return lastManualDir >= 0 ? f : -f;
}

int modeSweep(uint32_t now, int cap, bool entered) {
  // Ramp 0 -> ceiling -> 0, flip direction, repeat. Flip occurs at zero.
  // sweepShape makes the ramp crawl slowly away from each reversal, so the
  // profile lingers near zero at the turn; the peak still reaches the ceiling.
  const uint32_t T = cfg.sweepHalfMs;             // one half cycle (0 -> peak)
  static uint32_t t0 = 0; static bool up = true; static int dir = 1;
  if (entered) { t0 = now; up = true; dir = (lastManualDir >= 0) ? 1 : -1; }
  uint32_t e = now - t0;
  if (e >= T) { t0 = now; e = 0; up = !up; if (up) dir = -dir; }
  float n = up ? (float)e / T : (float)(T - e) / T;   // 0..1, 0 at the reversal
  int f = (int)(powf(n, cfg.sweepShape) * cap);
  return dir * f;
}

int modeWander(uint32_t now, int cap, bool entered) {
  (void)entered;
  // Organic noise driven drift. Reverses through zero, amplitude floored.
  // ASSUMPTION: pseudo-Perlin from two slow incommensurate sines gives a smooth
  // nonrepeating value in [-1,1] with no full cycle under ~8s, without a table.
  float t = now * cfg.wanderRate;
  float v = 0.6f * sinf(t * 0.45f) + 0.4f * sinf(t * 0.19f + 1.3f);
  if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
  return (int)(cap * v);
}

int modeTide(uint32_t now, int cap, bool entered) {
  (void)entered;
  // Minutes-long swell: rise to the ceiling and subside, then repeat with the
  // direction reversed, like a tide turning. sin^2 gives a soft trough and a
  // broad peak. The 0.08 floor keeps it barely, visibly, alive at the turn;
  // the tracker eases the sign flip through zero so the reversal is seamless.
  float ph = (now % cfg.tideMs) / (float)cfg.tideMs;   // 0..1
  float s = sinf(ph * PI);
  float env = 0.08f + 0.92f * s * s;
  int f = (int)(cap * env);
  int dir = ((now / cfg.tideMs) & 1) ? -1 : 1;         // alternate per cycle
  return (lastManualDir >= 0 ? dir : -dir) * f;
}

int modePendulum(uint32_t now, int cap, bool entered) {
  // Pure signed sine on velocity: the disc rocks to and fro, fastest at the
  // centre of each swing and lingering at the extremes. The complement of
  // SWEEP, which is slowest at its reversals. Phase restarts on entry so the
  // first swing always begins from rest.
  static uint32_t t0 = 0;
  if (entered) t0 = now;
  float ph = ((now - t0) % cfg.pendMs) / (float)cfg.pendMs;   // 0..1
  int f = (int)(cap * sinf(ph * TWO_PI));
  return lastManualDir >= 0 ? f : -f;
}

// Raised-cosine bump centred at c with half-width w, evaluated at phase ph.
static float hbPulse(float ph, float c, float w) {
  float d = fabsf(ph - c);
  if (d > w) return 0.0f;
  return 0.5f * (1.0f + cosf(PI * d / w));
}

int modeHeartbeat(uint32_t now, int cap, bool entered) {
  (void)entered;
  // Lub-dub: a strong surge, a softer echo, then a long low rest. Asymmetry
  // is what makes it read as alive. Floor keeps the rest phase visibly moving.
  float ph = (now % cfg.beatMs) / (float)cfg.beatMs;   // 0..1
  float env = hbPulse(ph, 0.10f, 0.07f) + 0.75f * hbPulse(ph, 0.27f, 0.06f);
  if (env > 1.0f) env = 1.0f;
  env = 0.12f + 0.88f * env;
  int f = (int)(cap * env);
  return lastManualDir >= 0 ? f : -f;
}

int modeStutter(uint32_t now, int cap, bool entered) {
  // Clockwork: jump between a few discrete speed levels, holding each for a
  // random dwell, occasionally flipping direction. The tracker still eases
  // between levels, so it reads as mechanical steps rather than violent jerks.
  // A hash of the step index picks level and dwell deterministically, so it is
  // reproducible per boot yet never looks periodic.
  static uint32_t stepEnd = 0;
  static uint32_t idx = 0;
  static int level = 0;      // 0..LEVELS-1
  static int dir = 1;
  const int LEVELS = 5;
  if (entered) { stepEnd = 0; idx = 0; dir = (lastManualDir >= 0) ? 1 : -1; }
  if (now >= stepEnd) {
    idx++;
    uint32_t h = idx * 2654435761u;             // Knuth multiplicative hash
    level = 1 + (int)((h >> 8) % LEVELS);        // 1..LEVELS, never a dead stop
    uint32_t dwell = cfg.stutMs / 2 + (h % cfg.stutMs);   // 0.5x..1.5x base
    stepEnd = now + dwell;
    if (((h >> 20) & 7) == 0) dir = -dir;        // ~1 in 8 steps reverses
  }
  int f = (int)((float)cap * level / LEVELS);
  return dir * f;
}

// ---------- Swarm pattern field ----------
// Pure functions of (x, y, sharedTime, params): every device computes an
// identical value for identical inputs. No device-local state, no randomness,
// no iteration-order dependence. KEEP IN SYNC WITH swarmPattern() in page.h;
// the browser preview runs this exact math, so the canvas IS the wall.
// sn() works in cycles with the fraction taken in double precision, so phase
// stays accurate no matter how large sharedMillis grows (a float mantissa
// would start visibly jittering after a few hours of uptime).
static float sn(double ph) { ph -= floor(ph); return sinf((float)(ph * 6.283185307179586)); }

float swarmPattern(uint8_t id, float x, float y, double t, const float *p) {
  float p0 = p[0] < 0.5f ? 0.5f : p[0];    // period never zero
  float p1 = p[1] < 0.05f ? 0.05f : p[1];  // wavelength / spacing / width never zero
  switch (id) {
    case 0:                                                       // UNISON
      return sn(t / p0);
    case 1:                                                       // WAVE
      // Travelling wave: a crest sweeps the wall along direction p2 (radians).
      return sn(t / p0 - (x * cosf(p[2]) + y * sinf(p[2])) / p1);
    case 2: {                                                     // RIPPLE
      // Expanding rings from source (p2, p3), phase lags with distance.
      float dx = x - p[2], dy = y - p[3];
      return sn(t / p0 - sqrtf(dx * dx + dy * dy) / p1);
    }
    case 3: {                                                     // CASCADE
      // Devices fire in sequence down the line: a raised-cosine pulse of
      // width p1 travels through each device's x rank once per cycle.
      double phd = t / p0; phd -= floor(phd);
      float d = (float)phd - x; d -= floorf(d);                   // 0..1
      if (d > 0.5f) d -= 1.0f;                                    // -0.5..0.5
      if (fabsf(d) > p1) return 0.0f;
      return 0.5f * (1.0f + cosf(PI * d / p1));
    }
    case 4: {                                                     // FLOCK
      // Correlated organic drift: three incommensurate sines (the modeWander
      // trick) sampled at each device's position. Neighbours move alike but
      // not identically; p1 scales how fast likeness falls off with distance.
      float s = p1;
      return 0.50f * sn(t / p0            + s * (0.81f * x + 0.27f * y))
           + 0.35f * sn(t / (p0 * 0.618f) + s * (0.37f * x + 1.26f * y))
           + 0.15f * sn(t / (p0 * 0.382f) + s * (1.50f * x + 0.49f * y));
    }
  }
  return 0.0f;
}

// Gesture ripples (interactive layer): expanding rings from recent hand pings,
// summed on top of the base pattern. An empty buffer costs only the loop test.
float swarmOverlay(uint32_t nowShared) {
  float sum = 0.0f;
  for (uint8_t i = 0; i < SWARM_PING_MAX; i++) {
    if (!swarmPings[i].used) continue;
    float age = (int32_t)(nowShared - swarmPings[i].t) / 1000.0f;
    if (age < 0.0f || age > SWARM_PING_DECAY_S) { swarmPings[i].used = false; continue; }
    float dx = swarmX - swarmPings[i].x, dy = swarmY - swarmPings[i].y;
    float dd = sqrtf(dx * dx + dy * dy);
    sum += expf(-age / SWARM_PING_DECAY_S) *
           sinf(TWO_PI * (age / SWARM_PING_PERIOD_S - dd / SWARM_PING_SPACING));
  }
  return sum;
}

// Run the routing matrix once per control tick (never inside the pattern, which
// can be evaluated twice during a mode crossfade). Each sensor axis maps through
// its range to a control value; amplitude falls back to presence, then to 1.0 on
// sensor silence so the wall runs its designed choreography exactly as if no
// sensor existed.
void senseTask() {
  bool fresh = senseEnabled && senseLastCue && (millis() - senseLastCue < SENSE_TIMEOUT_MS);
  senseCueFresh = fresh;
  float src[3] = { senseCx, senseCy, senseDepth };    // X, Y, Depth in 0..1
  float ampTarget = -1.0f;                            // <0 => not routed
  float focusTX = mFocusX, focusTY = mFocusY, spotT = 0.0f;
  bool  focusRouted = false;
  bool  pov[3] = { false, false, false };
  float pval[3] = { 0, 0, 0 };
  if (fresh) {
    for (int a = 0; a < 3; a++) {
      uint8_t dst = scfg.dst[a];
      if (dst == SD_NONE) continue;
      float vv = src[a]; if (scfg.inv & (1 << a)) vv = 1.0f - vv;
      float out = scfg.lo[a] + (scfg.hi[a] - scfg.lo[a]) * vv;
      switch (dst) {
        case SD_AMP:     ampTarget = out; break;
        case SD_FOCUSX:  focusTX = out; focusRouted = true; break;
        case SD_FOCUSY:  focusTY = out; focusRouted = true; break;
        case SD_SPOT:    spotT = out; break;
        case SD_PERIOD:  pov[0] = true; pval[0] = out; break;
        case SD_WAVELEN: pov[1] = true; pval[1] = out; break;
        case SD_DIR:     pov[2] = true; pval[2] = out; break;
      }
    }
  }
  // Amplitude: explicit route wins, else presence (Wake), else ambient.
  if (!fresh)              senseTarget = 1.0f;
  else if (ampTarget >= 0) senseTarget = ampTarget;
  else                     senseTarget = SENSE_IDLE_GAIN + (1.0f - SENSE_IDLE_GAIN) * sensePresence;
  senseGain += (senseTarget - senseGain) * 0.02f;     // ~0.5s time constant at 100Hz
  mFocusX += (focusTX - mFocusX) * 0.15f;
  mFocusY += (focusTY - mFocusY) * 0.15f;
  mSpot   += ((fresh ? spotT : 0.0f) - mSpot) * 0.08f;
  mFocusActive = fresh && focusRouted;
  for (int i = 0; i < 3; i++) { mPov[i] = pov[i]; mPval[i] = pval[i]; }
}

// Bilinear sample of the 8x8 depth field at wall position (x, y): 0 (empty) to
// 1 (near). MIRROR turns that intensity into local motion, so devices under
// the viewer move hardest, all pulsing on one shared breath so the wall reads
// as a single silhouette rather than 64 independent flickers.
float senseFieldBilinear(float x, float y) {
  float fx = x * (SENSE_FIELD_W - 1), fy = y * (SENSE_FIELD_H - 1);
  int x0 = (int)fx, y0 = (int)fy;
  if (x0 < 0) x0 = 0; if (x0 > SENSE_FIELD_W - 2) x0 = SENSE_FIELD_W - 2;
  if (y0 < 0) y0 = 0; if (y0 > SENSE_FIELD_H - 2) y0 = SENSE_FIELD_H - 2;
  float tx = fx - x0, ty = fy - y0;
  #define SF(a, b) (senseField[(b) * SENSE_FIELD_W + (a)] / 255.0f)
  float a = SF(x0, y0) * (1 - tx) + SF(x0 + 1, y0) * tx;
  float b = SF(x0, y0 + 1) * (1 - tx) + SF(x0 + 1, y0 + 1) * tx;
  #undef SF
  return a * (1 - ty) + b * ty;
}

float senseFieldSample(float x, float y) {
  if (!senseLastField || millis() - senseLastField > SENSE_TIMEOUT_MS)
    return 0.15f * sn(sharedMillis() / 6000.0);       // field lost: idle breathing
  return senseFieldBilinear(x, y) * sn(sharedMillis() / 1500.0);
}

int modeSwarm(uint32_t now, int cap, bool entered) {
  (void)now; (void)entered;
  // A follower that has lost (or never had) the conductor holds still; the
  // motion tracker turns this hard zero into a gentle ramp down.
  if (!swarmConductor &&
      (swarmLastBeacon == 0 || millis() - swarmLastBeacon > SWARM_TIMEOUT_MS)) return 0;
  uint32_t st = sharedMillis();
  float v;
  if (swarmPatternId == SWARM_MIRROR) {
    v = senseFieldSample(swarmX, swarmY);             // the wall mirrors the room
  } else {
    // Routed sensor controls override pattern params this tick; focus aims the
    // RIPPLE source. Untouched params fall through, so the designed choreography
    // is the default the moment the hand leaves.
    float pp[4] = { swarmP[0], swarmP[1], swarmP[2], swarmP[3] };
    if (senseCueFresh) {
      if (mPov[0]) pp[0] = mPval[0];
      if (mPov[1]) pp[1] = mPval[1];
      if (mPov[2]) pp[2] = mPval[2];
      if (swarmPatternId == 2 && mFocusActive) { pp[2] = mFocusX; pp[3] = mFocusY; }
    }
    v = swarmPattern(swarmPatternId, swarmX, swarmY, (double)st / 1000.0, pp);
  }
  // Spotlight layer: the device under the focus runs hard, neighbours fall off on
  // a gaussian, the rest idle slow. Composes over any base pattern.
  if (senseCueFresh && mSpot > 0.001f) {
    float dx = swarmX - mFocusX, dy = swarmY - mFocusY;
    float r = scfg.spotR > 0.02f ? scfg.spotR : 0.02f;
    float g = 0.12f + 0.88f * expf(-(dx * dx + dy * dy) / (r * r)) * mSpot;
    v *= g;
  }
  v += swarmOverlay(st);
  if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
  float g = senseEnabled ? senseGain : 1.0f;
  return (int)(v * swarmAmp * g * cap);
}

struct ModeDef {
  const char *name;
  ModeFn      fn;
  bool        floorSpeed;   // apply MODE_MIN_SPEED floor (MANUAL is exempt)
};

const ModeDef MODES[] = {
  { "MANUAL",    modeManual,    false },
  { "BREATHE",   modeBreathe,   true  },
  { "SWEEP",     modeSweep,     true  },
  { "WANDER",    modeWander,    true  },
  { "TIDE",      modeTide,      true  },
  { "PENDULUM",  modePendulum,  true  },
  { "HEARTBEAT", modeHeartbeat, true  },
  { "STUTTER",   modeStutter,   true  },
  { "SWARM",     modeSwarm,     true  },   // engaged from the Fleet tab, not the mode grid
};
const uint8_t MODE_COUNT = sizeof(MODES) / sizeof(MODES[0]);
#define MODE_SWARM (MODE_COUNT - 1)  // swarm rides the normal mode pipeline as the last entry

// Ceiling for a given mode, floored to the runtime minimum unless the mode is
// exempt. cfg.minSpeed = 0 lets auto modes be starved to a standstill, which is
// now a deliberate user choice rather than a failure state.
int modeCeiling(uint8_t m) {
  int cap = maxSpeedCeiling;
  if (MODES[m].floorSpeed && cap < cfg.minSpeed) cap = cfg.minSpeed;
  return cap;
}

int modeTarget() {
  uint32_t now = millis();

  // Track mode transitions: entry fires stateful resets, and starts a crossfade
  // from the outgoing mode so the character blends rather than swapping abruptly.
  static uint8_t prevMode = 255;
  static uint8_t fadeFrom = 255;
  static uint32_t fadeStart = 0;
  bool entered = (mode != prevMode);
  if (entered) { fadeFrom = prevMode; fadeStart = now; }
  prevMode = mode;

  if (mode >= MODE_COUNT) mode = 0;   // safety against an out of range index

  int target = MODES[mode].fn(now, modeCeiling(mode), entered);

  // Blend the outgoing envelope out over CROSSFADE_MS. The outgoing generator
  // keeps running (with entered = false) so its shape stays continuous.
  if (fadeFrom != 255 && fadeFrom < MODE_COUNT && fadeFrom != mode) {
    uint32_t e = now - fadeStart;
    if (e < CROSSFADE_MS) {
      int fromTarget = MODES[fadeFrom].fn(now, modeCeiling(fadeFrom), false);
      float a = (float)e / (float)CROSSFADE_MS;   // 0 -> outgoing, 1 -> incoming
      target = (int)(fromTarget * (1.0f - a) + target * a);
    } else {
      fadeFrom = 255;   // fade complete
    }
  }
  return target;
}

// Logarithmic helper: normalised 0..1 maps to 0..fullScale on a natural-log curve.
int logSpeed(float normalised, int fullScale) {
  if (normalised < 0) normalised = 0;
  if (normalised > 1) normalised = 1;
  float f = logf(1.0f + normalised * (E_CONST - 1.0f));  // 0 -> 0, 1 -> 1
  return (int)(fullScale * f);
}

// ============================================================
//  TMC2209 INIT  (shared, never inside #ifdef)
// ============================================================
void applyDriverConfig() {
  driver.en_spreadCycle(false);   // StealthChop2
  driver.TPWMTHRS(0);
  driver.pwm_autoscale(true);
  driver.pwm_autograd(true);
  driver.pwm_freq(1);             // 35.1kHz, above audible
  driver.microsteps(16);
  driver.intpol(true);            // interpolate to 256 internally
  driver.rms_current(RUN_CURRENT_MA, HOLD_MULT);
  driver.iholddelay(6);
}

void initTMC() {
  Serial1.begin(115200, SERIAL_8N1, UART_RX, UART_TX);
  driver.begin();
  applyDriverConfig();
  // Liveness via the IOIN VERSION field (0x21 on a TMC2209): a known constant
  // is a far more reliable read-back check than GCONF, which can legitimately
  // read as small values. Retry a few times through boot noise.
  uint8_t ver = 0;
  for (int i = 0; i < 3 && ver != 0x21; i++) { ver = driver.version(); delay(2); }
  uartOk = (ver == 0x21);
  faultTmcComm = !uartOk;
  Serial.printf("[tmc] version=0x%02X %s\n", ver, uartOk ? "UART OK" : "UART read-back FAULT (motor still runs open-loop)");
  // Do not hang on fault. Error flag is set for callers/telemetry.
}

// ============================================================
//  HEALTH POLL  (shared, slow cadence)
//  Reads TMC2209 status over UART and sets a defined response:
//    overtemp prewarning -> derate speed; sustained -> disable + latch.
//    UART comm loss -> flag (kept running; nothing trusted from driver).
//  StallGuard load is read for telemetry only (meaningful in spreadCycle).
//  All reads are debounced: a single corrupted UART frame must not derate the
//  motor or raise a fault. Faults need consecutive confirming reads; any good
//  read clears them immediately.
// ============================================================
#define OTPW_CONFIRM 3    // consecutive overtemp reads before derating (~1.5s at 2Hz)
#define OTPW_DISABLE 20   // consecutive overtemp reads before disable+latch (~10s)
#define COMM_CONFIRM 5    // consecutive bad GCONF reads before flagging comm loss (~2.5s at 2Hz)

void pollHealth() {
  // Round-robin: exactly ONE TMC UART transaction per call. Each read is a
  // blocking request+reply on Serial1 and glitched reads wait out a timeout;
  // stacking 3-4 of them back to back stalled the loop for tens of ms every
  // poll, starving ws.loop() and http on this single core chip. One register
  // per 500ms tick caps the worst case stall at a single transaction.
  static uint8_t phase = 0;
  static bool commBad = false;
  static uint8_t commBadCount = 0;
  static bool otpw = false;
  static uint8_t otpwCount = 0;

  switch (phase) {
    case 0: {                      // VERSION: comm read-back liveness
      commBad = (driver.version() != 0x21);   // 0x21 = TMC2209 IOIN VERSION field
      if (commBad) { if (commBadCount < 255) commBadCount++; }
      else commBadCount = 0;
      faultTmcComm = (commBadCount >= COMM_CONFIRM);
      break;
    }
    case 1: {                      // overtemp, only if comm looked sane
      otpw = (!commBad) && driver.otpw();
      if (otpw) { if (otpwCount < 255) otpwCount++; }
      else otpwCount = 0;
      break;
    }
    case 2: {                      // StallGuard load for telemetry
      if (!faultTmcComm) sgLoad = driver.SG_RESULT();
      break;
    }
  }
  phase = (phase + 1) % 3;

  if (otpwCount >= OTPW_CONFIRM) {
    faultOvertemp = true;
    speedDerate = 0.5f;                            // back off while genuinely warm
    if (otpwCount >= OTPW_DISABLE) motorEnabled = false;   // sustained: disable + latch
  } else {
    faultOvertemp = false;
    speedDerate = 1.0f;
  }

  static bool prevAny = false;
  bool any = faultTmcComm || faultOvertemp || faultTof;
  if (any != prevAny) {
    Serial.printf("[health] tmc_comm=%d overtemp=%d tof=%d derate=%.2f\n",
                  faultTmcComm, faultOvertemp, faultTof, speedDerate);
    prevAny = any;
  }
}

// ============================================================
//  WS2812 LED RINGS  (optional, ENABLE_LEDS)
//  One chained strip on LED_PIN, addressed as two segments (ring A then ring
//  B). Reactive modes read appliedFreq so the light truthfully follows what
//  the motor is physically doing, including the FREQ_MIN standstill.
// ============================================================
#ifdef ENABLE_LEDS
Adafruit_NeoPixel pixels(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

enum LedMode : uint8_t { LED_OFF = 0, LED_SOLID, LED_GLOW, LED_CHASE, LED_RAINBOW };
struct LedCfg {
  uint8_t  mode = LED_GLOW;
  uint16_t hue  = 30;    // 0..359
  uint8_t  bri  = 60;    // 0..100
  uint8_t  rate = 50;    // 1..100, animation speed for CHASE/RAINBOW
} led;
bool ledDirty = false;   // set on any change; NVS save debounced in persistTask

void loadLed() {
  led.mode = prefs.getUChar ("l_md",  LED_GLOW);
  if (led.mode > LED_RAINBOW) led.mode = LED_GLOW;
  led.hue  = prefs.getUShort("l_hue", 30);
  if (led.hue > 359) led.hue = 30;
  led.bri  = prefs.getUChar ("l_bri", 60);
  if (led.bri > 100) led.bri = 60;
  led.rate = prefs.getUChar ("l_rt",  50);
  if (led.rate < 1 || led.rate > 100) led.rate = 50;
}

void saveLed() {
  prefs.putUChar ("l_md",  led.mode);
  prefs.putUShort("l_hue", led.hue);
  prefs.putUChar ("l_bri", led.bri);
  prefs.putUChar ("l_rt",  led.rate);
  Serial.println("[nvs] saved led config");
}

// Draw a comet (head + fading tail) on one segment, wrapping within it.
void ledComet(int base, int count, float pos, uint32_t headColor) {
  int head = ((int)pos % count + count) % count;
  for (int t = 0; t < 6 && t < count; t++) {
    int i = (head - t % count + count) % count;
    uint8_t r = (headColor >> 16) & 0xFF, g = (headColor >> 8) & 0xFF, b = headColor & 0xFF;
    pixels.setPixelColor(base + i, pixels.Color(r >> t, g >> t, b >> t));
  }
}

void ledTask() {
  static float posA = 0, posB = 0, wheel = 0;
  static bool offDone = false;
  static uint32_t lastMs = 0;
  uint32_t now = millis();
  float dt = (lastMs == 0) ? 0.033f : (now - lastMs) * 0.001f;
  lastMs = now;
  if (dt > 0.2f) dt = 0.2f;

  if (led.mode == LED_OFF) {
    if (!offDone) { pixels.clear(); pixels.show(); offDone = true; }
    return;
  }
  offDone = false;

  uint8_t v = (uint16_t)led.bri * 255 / 100;
  uint32_t base = pixels.gamma32(pixels.ColorHSV((uint32_t)led.hue * 182, 255, v));

  switch (led.mode) {
    case LED_SOLID:
      pixels.fill(base, 0, LED_COUNT);
      break;

    case LED_GLOW: {
      // Brightness breathes with the sculpture's real speed. 8% floor keeps a
      // faint presence at standstill instead of going fully dark.
      float n = fabsf((float)appliedFreq) / (float)FREQ_MAX;
      if (n > 1.0f) n = 1.0f;
      uint8_t gv = (uint8_t)(v * (0.08f + 0.92f * n));
      pixels.fill(pixels.gamma32(pixels.ColorHSV((uint32_t)led.hue * 182, 255, gv)), 0, LED_COUNT);
      break;
    }

    case LED_CHASE: {
      // Comets spin with the disc: speed and direction follow appliedFreq,
      // segment B runs opposite to A to echo the counter-rotation.
      float rev = (appliedFreq / (float)FREQ_MAX) * (led.rate / 50.0f) * 1.5f; // rev/s at full speed
      posA = fmodf(posA + rev * LED_COUNT_A * dt, (float)LED_COUNT_A);
      posB = fmodf(posB - rev * LED_COUNT_B * dt, (float)LED_COUNT_B);
      // fmodf keeps the accumulators bounded; unwrapped they grow ~3M/day and
      // float precision starves the animation after a few days of uptime.
      pixels.clear();
      ledComet(0,           LED_COUNT_A, posA, base);
      ledComet(LED_COUNT_A, LED_COUNT_B, posB, base);
      break;
    }

    case LED_RAINBOW: {
      // Ambient hue wheel, independent of motion; rings drift opposite ways.
      wheel += (led.rate * 1.2f) * dt;                    // degrees/s
      if (wheel >= 360.0f) wheel -= 360.0f;
      for (int i = 0; i < LED_COUNT_A; i++) {
        float h = wheel + i * 360.0f / LED_COUNT_A;
        pixels.setPixelColor(i, pixels.gamma32(pixels.ColorHSV((uint32_t)((uint16_t)h % 360) * 182, 255, v)));
      }
      for (int i = 0; i < LED_COUNT_B; i++) {
        float h = -wheel + i * 360.0f / LED_COUNT_B;
        while (h < 0) h += 360.0f;
        pixels.setPixelColor(LED_COUNT_A + i, pixels.gamma32(pixels.ColorHSV((uint32_t)((uint16_t)h % 360) * 182, 255, v)));
      }
      break;
    }
  }
  pixels.show();
}
#endif  // ENABLE_LEDS

// ============================================================
//  NVS PERSISTENCE  (shared)
// NVS schema version. Bump when a key changes meaning or needs active repair.
// New keys with safe defaults do NOT need a bump: every getX() call in this
// file supplies a default, so missing keys self-heal. The migration ladder is
// for the cases defaults cannot fix. Falls through so any old version walks
// every step up to current.
#define CFG_VER 1

void migrateConfig() {
  uint32_t v = prefs.getUInt("cfg_ver", 0);
  if (v == CFG_VER) return;
  switch (v) {
    case 0: {
      // v0 boards derived their identity from the MAC's OUI bytes, so a whole
      // batch shared one suffix (e.g. -8C58) in stored AP names and hostnames.
      // Rewrite any stored name carrying the OLD suffix to the corrected one.
      char oldUid[8], newUid[8];
      snprintf(oldUid, sizeof(oldUid), "%04X", (uint16_t)(ESP.getEfuseMac() & 0xFFFF));
      snprintf(newUid, sizeof(newUid), "%04X", (uint16_t)(ESP.getEfuseMac() >> 32));
      const char *keys[] = { "ap_ssid", "host" };
      for (auto k : keys) {
        String val = prefs.getString(k, "");
        int p = val.indexOf(oldUid);
        if (p >= 0) {
          val = val.substring(0, p) + newUid + val.substring(p + 4);
          prefs.putString(k, val);
          Serial.printf("[nvs] migrate v0: %s -> %s\n", k, val.c_str());
        }
      }
      // fall through
    }
    default: break;
  }
  prefs.putUInt("cfg_ver", CFG_VER);
  Serial.printf("[nvs] config schema %u -> %u\n", v, CFG_VER);
}

//  Restores mode + ceiling on boot, saves them debounced so a power cycle
//  resumes gracefully. The motor still boots disabled regardless.
// ============================================================
void loadSettings() {
  prefs.begin("sculpt", false);
  migrateConfig();          // upgrade NVS written by older firmware first
  mode = prefs.getUChar("mode", MANUAL);
  if (mode >= MODE_COUNT) mode = MANUAL;
  if (mode == MODE_SWARM) mode = MANUAL;   // swarm engagement is never persisted
  maxSpeedCeiling = 0;   // always boot with the slider at rest (speed 0), regardless of NVS
  // Swarm identity persists; swarm engagement does not.
  swarmX = prefs.getFloat("sw_x", 0.5f);
  swarmY = prefs.getFloat("sw_y", 0.5f);
  swarmConductor = prefs.getBool("sw_cond", false);
  swarmKey = prefs.getUInt("sw_key", 0);
  senseEnabled = prefs.getBool("sw_sense", true);
  if (prefs.getBytesLength("sw_scfg") == sizeof(scfg))
    prefs.getBytes("sw_scfg", &scfg, sizeof(scfg));   // else keep the compiled default
  Serial.printf("[nvs] restored mode=%u ceil=%d\n", mode, maxSpeedCeiling);
  Serial.printf("[swarm] pos=%.2f,%.2f role=%s\n", swarmX, swarmY,
                swarmConductor ? "conductor" : "follower");
}

// ----- Motion config + mode queue persistence -----
void parseQueue(const String &s) {
  queueLen = 0;
  int i = 0;
  while (i < (int)s.length() && queueLen < QUEUE_MAX) {
    int comma = s.indexOf(',', i);
    if (comma < 0) comma = s.length();
    String tok = s.substring(i, comma);          // "mode:secs"
    int colon = tok.indexOf(':');
    if (colon > 0) {
      int m = tok.substring(0, colon).toInt();
      int sec = tok.substring(colon + 1).toInt();
      if (m < 0) m = 0; if (m >= MODE_COUNT - 1) m = MODE_COUNT - 2;   // SWARM is not queueable
      if (sec < 1) sec = 1; if (sec > 3600) sec = 3600;
      queueSteps[queueLen].mode = m;
      queueSteps[queueLen].secs = sec;
      queueLen++;
    }
    i = comma + 1;
  }
}

void loadMotion() {
  cfg.breatheMs    = prefs.getUInt ("m_bms", BREATHE_PERIOD_MS);
  cfg.sweepHalfMs  = prefs.getUInt ("m_sms", SWEEP_HALF_MS);
  cfg.wanderRate   = prefs.getFloat("m_wr",  WANDER_RATE);
  cfg.tideMs       = prefs.getUInt ("m_tms", TIDE_PERIOD_MS);
  cfg.pendMs       = prefs.getUInt ("m_pms", PEND_PERIOD_MS);
  cfg.beatMs       = prefs.getUInt ("m_hms", BEAT_PERIOD_MS);
  cfg.stutMs       = prefs.getUInt ("m_sts", STUT_PERIOD_MS);
  cfg.minSpeed     = prefs.getInt  ("m_flo", MODE_MIN_SPEED);
  if (cfg.minSpeed < 0) cfg.minSpeed = 0;
  if (cfg.minSpeed > FREQ_MAX) cfg.minSpeed = FREQ_MAX;
  cfg.smoothUp     = prefs.getFloat("m_up",  SMOOTH_TIME_UP);
  cfg.smoothDown   = prefs.getFloat("m_dn",  SMOOTH_TIME_DOWN);
  cfg.breatheShape = prefs.getFloat("m_bsh", BREATHE_SHAPE);
  cfg.sweepShape   = prefs.getFloat("m_ssh", SWEEP_SHAPE);
  queueEnabled     = prefs.getBool ("q_en",  false);
  parseQueue(prefs.getString("q_str", ""));
  queueStepStart = 0;   // (re)start the queue on next tick
}

void saveMotion() {
  prefs.putUInt ("m_bms", cfg.breatheMs);
  prefs.putUInt ("m_sms", cfg.sweepHalfMs);
  prefs.putFloat("m_wr",  cfg.wanderRate);
  prefs.putUInt ("m_tms", cfg.tideMs);
  prefs.putUInt ("m_pms", cfg.pendMs);
  prefs.putUInt ("m_hms", cfg.beatMs);
  prefs.putUInt ("m_sts", cfg.stutMs);
  prefs.putInt  ("m_flo", cfg.minSpeed);
  prefs.putFloat("m_up",  cfg.smoothUp);
  prefs.putFloat("m_dn",  cfg.smoothDown);
  prefs.putFloat("m_bsh", cfg.breatheShape);
  prefs.putFloat("m_ssh", cfg.sweepShape);
  prefs.putBool ("q_en",  queueEnabled);
  String qs = "";
  for (uint8_t i = 0; i < queueLen; i++) {
    qs += String(queueSteps[i].mode) + ":" + String(queueSteps[i].secs);
    if (i + 1 < queueLen) qs += ",";
  }
  prefs.putString("q_str", qs);
  Serial.println("[nvs] saved motion config");
}

// Advance the mode through the queue while it is enabled (shared, all builds).
void queueTask() {
  if (!queueEnabled || queueLen == 0) return;
  uint32_t now = millis();
  if (queueStepStart == 0) { queueIdx = 0; mode = queueSteps[0].mode; queueStepStart = now; return; }
  uint32_t dwell = (uint32_t)queueSteps[queueIdx].secs * 1000UL;
  if (now - queueStepStart >= dwell) {
    queueIdx = (queueIdx + 1) % queueLen;
    mode = queueSteps[queueIdx].mode;
    queueStepStart = now;
  }
}

// Queue playback and direct mode selection are mutually exclusive. Any direct
// choice (app button, ToF gesture, mode button) stops the queue; the stored
// playlist is kept, only the enable flag drops, and it is persisted immediately
// so a reboot does not resurrect playback the user just cancelled.
void cancelQueue(const char *why) {
  if (!queueEnabled) return;
  queueEnabled = false;
  queueStepStart = 0;
  prefs.putBool("q_en", false);
  queueOffPending = true;   // WiFi build broadcasts this to sync the UI checkbox
  Serial.printf("[queue] stopped (%s)\n", why);
}

void userSelectMode(uint8_t m) {
  cancelQueue("mode selected directly");
  if (m >= MODE_COUNT) m = 0;
  // A direct local choice always wins on this device. Leaving SWARM mutes this
  // device against active beacons until the swarm is released and re-engaged;
  // on the conductor it releases the whole swarm.
  if (mode == MODE_SWARM && m != MODE_SWARM) {
    swarmMuted = true;
    swarmActive = false;
    if (swarmConductor) swarmReleaseBeacons = 6;   // WiFi build: tell the followers
  }
  mode = m;
}

// Engage or release swarm on this device. Engage seeds the local speed ceiling
// from the choreography amplitude when the slider is at rest, because the
// ceiling deliberately boots to 0 and nobody wants to walk a wall of ten
// sculptures raising every slider by hand. The local slider stays a hard cap:
// pulling it to 0 mid-swarm silences this device only.
// An auto-managed ceiling runs at full scale, because amplitude is applied ONCE
// in modeSwarm (v * swarmAmp * cap). Deriving the ceiling from amplitude too
// would scale the wall quadratically and, worse, inconsistently: a device with
// a hand-set cap scales linearly while an auto device scaled quadratically, so
// the same amplitude change moved them by different amounts. Full-scale cap +
// the single swarmAmp term makes every device respond to amplitude identically
// (as a fraction of its own cap). A human touching the local slider reclaims
// the cap and turns auto-follow off, keeping the slider a per-device safety cap.
void swarmApplyAutoCeil() {
  if (!swarmCeilAuto) return;
  maxSpeedCeiling = FREQ_MAX;
}

void swarmSetActive(bool on) {
  if (on) {
    cancelQueue("swarm");
    swarmMuted = false;
    if (maxSpeedCeiling == 0) swarmCeilAuto = true;
    swarmApplyAutoCeil();
    mode = MODE_SWARM;
    swarmActive = true;
  } else {
    swarmActive = false;
    if (mode == MODE_SWARM) { mode = MANUAL; manualSpeed = 0; }
  }
}

void persistTask() {
  static bool init = false;
  static uint8_t savMode = 255, obsMode = 255;
  static int savCeil = -1, obsCeil = -1;
  static uint32_t quiet = 0;
  uint32_t now = millis();

  if (!init) { savMode = obsMode = mode; savCeil = obsCeil = maxSpeedCeiling; quiet = now; init = true; return; }

  // A 50 step deadband ignores pot ADC jitter so a steady pot does not block saves.
  bool changed = (mode != obsMode) || (abs(maxSpeedCeiling - obsCeil) > 50);
  if (changed) { obsMode = mode; obsCeil = maxSpeedCeiling; quiet = now; return; }

  if ((mode != savMode || abs(maxSpeedCeiling - savCeil) > 50) && (now - quiet > 3000)) {
    prefs.putUChar("mode", mode);
    prefs.putInt("ceil", maxSpeedCeiling);
    savMode = mode; savCeil = maxSpeedCeiling;
    Serial.printf("[nvs] saved mode=%u ceil=%d\n", mode, maxSpeedCeiling);
  }

#ifdef ENABLE_LEDS
  // LED sliders stream live values; write NVS only after 3s of quiet so a
  // drag does not hammer flash.
  static bool ledPend = false;
  static uint32_t ledQuiet = 0;
  if (ledDirty) { ledDirty = false; ledPend = true; ledQuiet = now; }
  if (ledPend && now - ledQuiet > 3000) { ledPend = false; saveLed(); }
#endif
}


// ============================================================
//  POT + BUTTON INPUT
// ============================================================
#ifdef INPUT_POT_BTN
const uint32_t DEBOUNCE_MS = 40;
bool safeStart = false;   // pot must pass through centre once before motion

int potSignedFreq() {
  int delta = analogRead(POT_PIN) - 2048;
  if (abs(delta) < DEAD_BAND) {
    if (!safeStart) { safeStart = true; Serial.println("[safe] pot centred, motion unlocked"); }
    return 0;
  }
  if (!safeStart) return 0;
  int f = map(abs(delta), DEAD_BAND, 2047, FREQ_MIN, FREQ_MAX);
  return (delta > 0) ? f : -f;
}

int potCeiling() {
  // For non-MANUAL modes the pot sets the ceiling from its magnitude.
  if (!safeStart) return 0;
  int delta = abs(analogRead(POT_PIN) - 2048);
  if (delta < DEAD_BAND) return 0;
  return map(delta, DEAD_BAND, 2047, FREQ_MIN, FREQ_MAX);
}

void pollPotButtons() {
  // EN_BTN: hold to enable. ENN active LOW handled by FastAccelStepper enable pin.
  motorEnabled = (digitalRead(EN_BTN) == LOW);

  // MODE_BTN: press to advance mode, debounced.
  static bool last = HIGH; static uint32_t t0 = 0;
  int r = digitalRead(MODE_BTN);
  if (last == HIGH && r == LOW && millis() - t0 > DEBOUNCE_MS) {
    t0 = millis();
    userSelectMode((mode + 1) % (MODE_COUNT - 1));   // cycle the real modes; SWARM is not in the rotation
    Serial.printf("[mode] -> %u\n", mode);
  }
  last = r;
}

void inputUpdate() {
  pollPotButtons();
  if (mode == MANUAL) {
    int s = potSignedFreq();
    manualSpeed = s;
    if (s > 0) lastManualDir = 1; else if (s < 0) lastManualDir = -1;
  } else {
    maxSpeedCeiling = potCeiling();
  }
}
#endif  // INPUT_POT_BTN

// ============================================================
//  ToF10120 SENSOR + GESTURE STATE MACHINE
// ============================================================
#ifdef USES_TOF
// ToF10120 I2C address. The datasheet quotes 0xA4 (8-bit write address); on
// Arduino Wire (7-bit) that is 0xA4 >> 1 = 0x52 (decimal 82). (0x29 is a
// different part, the VL53L5CX, and is not used here.)
uint8_t tofAddr = 0x52;

// Window geometry (millimetres). Speed is now a single unidirectional ramp
// (far = fast), not a bidirectional dead band; direction is a separate flip.
#define WIN_START      200   // usable hand band floor (readings clamp up to here)
#define WIN_END       1000   // usable hand band ceiling / full-speed distance
#define SPEED_MIN_DIST 240   // at/below this the motor is disabled (24cm)
#define SPEED_REENABLE 270   // must lift back above this to re-enable (hysteresis)
#define GESTURE_EXIT  1050   // hand treated as leaving the window beyond here
#define SPEED_EXIT    1100   // sustained beyond here freezes speed / ends control
#define REENTRY_HYST    50   // must drop 50mm below exit to re-enter
#define STILL_TOL       50   // +-5cm: within this a hold counts as "still"; more = speed
#define NO_HAND_DIST  2000   // sentinel: invalid or beyond usable band = no hand (far)
#define EMA_ALPHA     0.4f   // distance smoothing, 1.0 = no smoothing, lower = calmer

// Still-hold classification, measured from beam entry to exit (withdraw):
//   < HOLD_TAP            a brief tap (two within DOUBLETAP_MS flip direction)
//   HOLD_TAP..MODE        next mode          (owner only)
//   MODE..HANDOFF         seize / release hand control
//   HANDOFF..RESET        ignored (buffer, so a long hold does not reset by accident)
//   > RESET               network reset to default AP (WiFi build)
#define HOLD_TAP_MS     1000
#define HOLD_MODE_MS    3000
#define HOLD_HANDOFF_MS 8000
#define HOLD_RESET_MS  15000
#define DOUBLETAP_MS     900   // max gap between the two brief taps of a direction flip

// Window geometry must stay ordered or the gesture math silently misbehaves.
static_assert(WIN_START < SPEED_MIN_DIST,   "band floor must be below the disable boundary");
static_assert(SPEED_MIN_DIST < SPEED_REENABLE, "disable boundary must be below its re-enable point");
static_assert(SPEED_REENABLE < WIN_END,     "re-enable point must sit inside the band");
static_assert(WIN_END < GESTURE_EXIT,       "gesture exit must be beyond the band ceiling");
static_assert(GESTURE_EXIT < SPEED_EXIT,    "speed exit must be beyond gesture exit");
static_assert(SPEED_EXIT < NO_HAND_DIST,    "no hand sentinel must sit beyond speed exit");
static_assert(HOLD_TAP_MS < HOLD_MODE_MS && HOLD_MODE_MS < HOLD_HANDOFF_MS && HOLD_HANDOFF_MS < HOLD_RESET_MS,
              "hold ladder must be strictly increasing");

// Gesture FSM
enum GState : uint8_t { G_IDLE = 0, G_ACQUIRING, G_EVALUATING, G_SPEED };
GState gstate = G_IDLE;

int  filtDist = NO_HAND_DIST;  // latest filtered reading (mm)
int  med[3] = {NO_HAND_DIST, NO_HAND_DIST, NO_HAND_DIST};
uint8_t medIdx = 0;
float emaDist = NO_HAND_DIST;  // exponential moving average of the median
bool  emaInit = false;

int  entryDist = 0;
uint32_t tEnter = 0;           // millis the hand entered the window
uint32_t tStableStart = 0;     // millis the reading became stable in ACQUIRING
int  acqRef = 0;               // reference for stability check
uint32_t tExitSpeed = 0;       // millis speed control last exited (for re-entry)
bool waitingReentry = false;

// ASSUMPTION: ToF10120 read protocol is write register 0x00 then read 2 bytes,
// big-endian millimetres. This matches the common ToF10120 distance register.
int readToFRaw() {
  Wire.beginTransmission(tofAddr);
  Wire.write((uint8_t)0x00);          // point at the distance register
  if (Wire.endTransmission() != 0) return -1;
  // The TOF10120 needs a moment to latch the reading before it can be read back
  // (datasheet: at least 30us). Skipping this makes the read NACK, so a fitted
  // sensor looks absent and never sees a hand. Match the reference driver's 1ms
  // (delay() also yields to WiFi rather than busy-spinning).
  delay(1);
  if (Wire.requestFrom(tofAddr, (uint8_t)2) != 2) return -1;
  int hi = Wire.read();
  int lo = Wire.read();
  return (hi << 8) | lo;
}

bool initToF() {
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  // A stuck bus otherwise blocks ~50ms (core default) per op, twice per read,
  // at 30Hz: instant loop starvation. A full read takes <1ms at 100kHz, so 5ms
  // is generous.
  Wire.setTimeOut(5);
  delay(50);
  int d = readToFRaw();
  bool ok = (d >= 0);
  Serial.printf("[tof] init %s addr=0x%02X (d=%d)\n", ok ? "OK" : "FAULT", tofAddr, d);
  return ok;
}

int median3(int a, int b, int c) {
  if (a > b) { int t = a; a = b; b = t; }
  if (b > c) { int t = b; b = c; c = t; }
  if (a > b) { int t = a; a = b; b = t; }
  return b;
}

// Read one sample. Invalid or out of range is mapped to a far sentinel (NOT
// discarded, which previously froze filtDist and stalled gesture exit). A
// 3 sample median rejects single spikes and an EMA calms residual jitter.
// Cadence is owned by the scheduler, so there is no internal time gate here.
bool sampleToF() {
  // Dead sensor backoff: once faulted, probe at 1Hz instead of hammering a
  // dead bus at 30Hz. Recovers automatically when reads succeed again.
  static uint32_t tNextProbe = 0;
  if (faultTof || !tofPresent) {   // dead OR never-fitted: probe slowly, do not hammer I2C
    uint32_t now = millis();
    if (now < tNextProbe) return false;
    tNextProbe = now + 1000;
  }
  int d = readToFRaw();
  static uint16_t failRun = 0;
  if (d < 0) { if (failRun < 0xFFFF) failRun++; }   // raw read failed
  else { failRun = 0; tofPresent = true; }          // a good read: sensor is fitted (hot-plug ok)
  faultTof = tofPresent && (failRun > 60);           // only a PRESENT sensor going silent is a fault
  if (d < 0 || d > NO_HAND_DIST) d = NO_HAND_DIST;   // invalid / far beyond reach = no hand
  else if (d < WIN_START) d = WIN_START;             // clamp very near readings up to 200mm
  med[medIdx] = d;                                    // (the far/speed cap at 1000 is in the ramp)
  medIdx = (medIdx + 1) % 3;
  int m = median3(med[0], med[1], med[2]);
  if (!emaInit) { emaDist = m; emaInit = true; }
  else          { emaDist += EMA_ALPHA * (m - emaDist); }
  filtDist = (int)(emaDist + 0.5f);
  return true;
}

// Distance -> unsigned speed magnitude on a single ramp: SPEED_MIN_DIST..WIN_END
// maps to 0..FREQ_MAX, far = fast. Below the disable boundary returns 0; the
// caller (or applySpeedControl) handles the actual disable + hysteresis.
int speedMagFromDist(int d) {
  if (d <= SPEED_MIN_DIST) return 0;
  if (d > WIN_END) d = WIN_END;                                       // cap far at full speed
  float norm = (float)(d - SPEED_MIN_DIST) / (float)(WIN_END - SPEED_MIN_DIST); // 0..1
  return logSpeed(norm, FREQ_MAX);
}

// Signed manual speed = magnitude * the current hand-control direction.
int manualSpeedFromDist(int d) { return tofDir * speedMagFromDist(d); }

// Ceiling for the auto modes (BREATHE/SWEEP/WANDER); direction is applied by the
// mode generators through lastManualDir, so this stays unsigned.
int ceilingFromDist(int d) { return speedMagFromDist(d); }

// A 3-8s still-hold hands motion control between the web GUI/swarm and the
// physical hand, and latches until held again. On seizing, the sculpture leaves
// the swarm and ignores remote drive commands so the person at it always wins;
// releasing lets it rejoin / obey the GUI again.
void fireControlToggle() {
  tofControl = !tofControl;
  if (tofControl) {
    if (mode == MODE_SWARM) swarmSetActive(false);
    swarmMuted = true;                 // beacons will not re-engage while the hand owns it
    cancelQueue("hand control");
  } else {
    swarmMuted = false;                // released: rejoin the swarm / obey the GUI
  }
  Serial.printf("[gesture] control -> %s\n", tofControl ? "HAND (ToF)" : "GUI/swarm");
}

// A double tap (two brief taps within DOUBLETAP_MS) flips the hand-control
// direction. Only meaningful while the hand owns control.
void fireDirFlip() {
  if (!tofControl) return;
  tofDir = -tofDir;
  lastManualDir = tofDir;              // so the auto modes reverse too
  Serial.printf("[gesture] direction -> %s\n", tofDir > 0 ? "FWD" : "REV");
}

uint32_t tLastTap = 0;
void noteTap(uint32_t now) {
  if (tLastTap && now - tLastTap < DOUBLETAP_MS) { fireDirFlip(); tLastTap = 0; }
  else tLastTap = now;
}

// The mode hold only drives while the hand owns control. Handoff (3-8s) and the
// >15s network reset are always live (that is how you seize control in the first
// place, and the reset is the recovery lifeline).
void fireModeChange() {
  if (!tofControl) return;
  userSelectMode((mode + 1) % (MODE_COUNT - 1));   // cycle the real modes; SWARM is not in the rotation
  Serial.printf("[gesture] mode change -> %u\n", mode);
}

void fireNetworkReset() {
#ifdef INPUT_TOF_WIFI
  // Clear all stored network settings and reboot to the default KineticSculpture
  // AP. prefs is the shared NVS store; removing absent keys is harmless.
  Serial.println("[net] gesture reset: reverting to default AP, rebooting");
  prefs.remove("sta_ssid"); prefs.remove("sta_pass"); prefs.remove("use_sta");
  prefs.remove("ap_ssid");  prefs.remove("ap_pass");  prefs.remove("ap_ip");
  prefs.remove("host");
  prefs.remove("use_static"); prefs.remove("sta_ip"); prefs.remove("sta_gw"); prefs.remove("sta_mask");
  prefs.remove("ui_pass");   // owner escape hatch for a forgotten interface password
  delay(200);
  ESP.restart();
#else
  Serial.println("[gesture] 15s hold (no network layer in this build)");
#endif
}

void applySpeedControl() {
  // Live distance -> speed while the hand owns control. Otherwise the GUI/swarm
  // owns it; in SWARM the hand is a ripple source instead (swarmGesturePing).
  if (!tofControl) return;
  if (mode == MODE_SWARM) return;   // defensive: a grab already broke it out of swarm
  int d = filtDist;
  // Close = stop: at/below the disable boundary, cut the motor (hysteresis keeps
  // it off until the hand lifts back above the re-enable point).
  if (d <= SPEED_MIN_DIST) {
    motorEnabled = false;
    if (mode == MANUAL) manualSpeed = 0;
    return;
  }
  // In the speed band: enable first if the motor was off, then map distance.
  if (!motorEnabled && d >= SPEED_REENABLE) motorEnabled = true;
  if (mode == MANUAL) {
    int s = manualSpeedFromDist(d);
    manualSpeed = s;
    if (s > 0) lastManualDir = 1; else if (s < 0) lastManualDir = -1;
  } else {
    maxSpeedCeiling = ceilingFromDist(d);
  }
}

void gestureTick() {
  if (!sampleToF()) return;
  uint32_t now = millis();
  int d = filtDist;

  switch (gstate) {

    case G_IDLE:
      if (d <= GESTURE_EXIT) {
        // Re-entry shortcut: resume speed control if we exited it recently.
        if (waitingReentry && (now - tExitSpeed) <= 1000 && d <= (SPEED_EXIT - REENTRY_HYST)) {
          gstate = G_SPEED;
          waitingReentry = false;
          break;
        }
        waitingReentry = false;
        entryDist = d;
        acqRef = d;
        tEnter = now;
        tStableStart = now;
        gstate = G_ACQUIRING;
      }
      break;

    case G_ACQUIRING:
      if (d > GESTURE_EXIT) { gstate = G_IDLE; break; }
      if (abs(d - acqRef) > 15) {        // not stable, restart the 200ms window
        acqRef = d;
        tStableStart = now;
      } else if (now - tStableStart >= 200) {
        entryDist = d;                    // lock entry distance
        gstate = G_EVALUATING;
      }
      break;

    case G_EVALUATING:
      // Motor ignores the hand here. A move beyond +-5cm commits to speed
      // control; staying still and withdrawing is classified as a hold gesture.
      if (abs(d - entryDist) > STILL_TOL) {
        gstate = G_SPEED;                 // movement confirmed: speed control now
#ifdef INPUT_TOF_WIFI
        swarmGesturePing();               // in swarm (not owning): this hand is a ripple source
#endif
        break;                            // enable + mapping are handled in applySpeedControl
      }
      if (d > GESTURE_EXIT) {             // withdrew while still: classify by dwell
        uint32_t held = now - tEnter;
        if (held < HOLD_TAP_MS)            noteTap(now);          // brief tap -> direction flip on double
        else if (held < HOLD_MODE_MS)      fireModeChange();      // 1-3s -> next mode (owner only)
        else if (held < HOLD_HANDOFF_MS)   fireControlToggle();   // 3-8s -> seize / release control
        else if (held >= HOLD_RESET_MS)    fireNetworkReset();    // >15s -> revert to default AP
        // 8-15s: intentional dead buffer, do nothing (avoids accidental reset)
        gstate = G_IDLE;
      }
      break;

    case G_SPEED: {
      static uint32_t tAboveExit = 0;
      if (d > SPEED_EXIT) {
        if (tAboveExit == 0) tAboveExit = now;
        if (now - tAboveExit >= 200) {    // sustained exit, freeze last speed
          tAboveExit = 0;
          tExitSpeed = now;
          waitingReentry = true;
          gstate = G_IDLE;                // speed/ceiling stays frozen at last value
          break;
        }
      } else {
        tAboveExit = 0;
        applySpeedControl();
      }
      break;
    }
  }
}

const char* gestureName() {
  switch (gstate) {
    case G_IDLE:       return "idle";
    case G_ACQUIRING:  return "acquiring";
    case G_EVALUATING: return "evaluating";
    case G_SPEED:      return "speed_control";
  }
  return "idle";
}
#endif  // USES_TOF

// ============================================================
//  WIFI + WEB APP  (ToF+WiFi version only)
// ============================================================
#ifdef INPUT_TOF_WIFI
WebServer        http(80);
WebSocketsServer ws(81);
DNSServer        dns;

// Network defaults (used when nothing is stored, and after a gesture reset).
#define DEF_AP_SSID "Kinesthetic"
#define DEF_AP_PASS "kinetic123"
#define DEF_HOST    "sculpture"
#define OTA_PASS    "kinetic"    // required by the IDE when uploading over WiFi
#define FW_VERSION  "2.4.0"      // shown in the UI; bump on each release
// Pre-filled into the OTA box so a fresh board can self-update with one tap. The
// CI workflow publishes firmware.bin to this rolling "latest" release on push.
#define DEF_FW_URL  "https://github.com/knnurl/kinesthetic/releases/download/latest/firmware.bin"

// Loaded network settings + live status.
String    apSsid, apPass, staSsid, staPass, hostName;
IPAddress apIP, staIP, staGw, staMask;
bool      useSta = false, useStatic = false, captiveActive = false;
String    netMode = "AP", netIp = "0.0.0.0";
String    fwUrl = "";              // GitHub (or other) .bin URL for pull updates
String    fleetList = "";          // shared fleet roster (comma separated hosts), NVS
                                   // fl_list; lets any device's page rebuild the whole
                                   // fleet view instead of relying on one phone's storage
void performOtaPull(const String &url);   // defined below, called from the WS handler

// The web app hardcodes one button per mode. If MODE_COUNT changes, the buttons
// in PAGE must change too. This catches the mismatch at build time.
// The web app grid has one chip per real mode; SWARM (the last table entry) is
// engaged from the Fleet tab instead and deliberately has no chip.
static_assert(MODE_COUNT == 9, "web app PAGE has 8 mode chips + SWARM; update page.h to match MODE_COUNT");

// The web app (HTML/CSS/JS) lives in page.h as a PROGMEM raw string literal.
// It must stay in a header. The Arduino IDE / arduino-cli ctags prototype
// generator cannot parse a C++11 raw string in the .ino: it misreads the
// embedded JavaScript as C and fails the build (function does not name a
// type). ctags does not scan #included files, so the literal is safe here.
#include "page.h"


// ---------- Web interface authentication ----------
// Optional password gate. Empty ui_pass (factory state) = open access, exactly
// the old behavior. When set: WS clients must send {cmd:"auth",pw} before any
// command is honored or telemetry is sent to them; HTTP JSON endpoints require
// the session token handed out on successful auth. The page itself is always
// served (it is not a secret; the lock screen lives in it). The physical
// network-reset gesture clears the password: that is the owner escape hatch.
String uiPass;                 // stored password, empty = no gate
String uiToken;                // per-boot session token for HTTP fetches
bool wsAuthed[WEBSOCKETS_SERVER_CLIENT_MAX] = { false };

bool authRequired() { return uiPass.length() > 0; }

// Send to authorized clients only (all clients when no password is set).
void wsSendAll(const String &msg) {
  String m = msg;   // sendTXT takes a mutable reference in this library
  for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++)
    if (wsAuthed[i] && ws.clientIsConnected(i)) ws.sendTXT(i, m);
}
void wsSendAll(const char *msg) { wsSendAll(String(msg)); }

bool httpAuthed() { return !authRequired() || http.arg("t") == uiToken; }

// ---------- Swarm network layer (UDP clock beacon + gesture pings) ----------
// Binary little-endian packets on a broadcast socket. The shared key is cheap
// insurance against a stray device on the LAN, not cryptography; commands that
// change swarm state ride the authenticated WebSocket.
WiFiUDP swarmUdp;
struct __attribute__((packed)) SwarmHdr { uint32_t magic; uint32_t key; };
struct __attribute__((packed)) SwarmBeacon {
  SwarmHdr hdr;
  uint8_t  type;        // 1 = beacon
  uint8_t  pattern;
  uint8_t  active;      // 1 = swarm engaged, 0 = released
  uint8_t  seq;         // debug only
  uint32_t t;           // conductor sharedTime in ms (its millis)
  float    amplitude;
  float    p0, p1, p2, p3;
};
struct __attribute__((packed)) SwarmPing {
  SwarmHdr hdr;
  uint8_t  type;        // 2 = ping
  uint8_t  pad[3];
  uint32_t t;           // sender sharedTime at the gesture
  float    x, y;        // sender wall position
};
// Sensor node -> wall. KEEP IN SYNC WITH sensor_node/sensor_node.ino.
struct __attribute__((packed)) SwarmCue {
  SwarmHdr hdr;
  uint8_t  type;        // 3 = cue
  uint8_t  presence;    // 0..255 how strongly someone is present
  uint8_t  blobs;       // near-region count 0..4
  uint8_t  flags;       // bit0: field frames also broadcast
  float    cx, cy;      // primary blob centroid, wall coords 0..1
  float    vx, vy;      // centroid velocity, wall units/sec
  float    depth;       // primary blob distance-from-wall, normalised 0..1 (1 = closest)
};
struct __attribute__((packed)) SwarmField {
  SwarmHdr hdr;
  uint8_t  type;        // 4 = field
  uint8_t  w, h;        // grid dimensions
  uint8_t  seq;
  uint8_t  cells[SENSE_FIELD_W * SENSE_FIELD_H];   // 0 empty, 1..255 near, row-major
};
// Sensor routing config, conductor -> followers. Wall-only (the sensor node
// never needs it). pad[3] puts the embedded SenseCfg (floats first) at offset 12
// so its floats stay 4-aligned when the packet is cast in place.
struct __attribute__((packed)) SwarmSenseCfg {
  SwarmHdr hdr;
  uint8_t  type;        // 5 = sense config
  uint8_t  pad[3];
  SenseCfg cfg;
};
// Presence announce for network discovery: every sculpture broadcasts this so
// any device's page can list the whole fleet without typing addresses. Accepted
// regardless of swarm key (you discover devices before linking them).
struct __attribute__((packed)) SwarmHello {
  SwarmHdr hdr;
  uint8_t  type;        // 6 = hello
  uint8_t  role;        // bit0: conductor
  uint8_t  pad[2];
  char     host[32];    // mDNS hostname (reach it at host.local)
};
struct SwarmPeer { uint32_t ip; uint32_t seen; uint8_t role; char host[32]; };
SwarmPeer swarmPeers[SWARM_PEERS_MAX] = {};
uint8_t swarmSeq = 0;

void sendSwarmBeacon() {
  SwarmBeacon b;
  b.hdr.magic = SWARM_MAGIC; b.hdr.key = swarmKey;
  b.type = 1; b.pattern = swarmPatternId;
  b.active = swarmActive ? 1 : 0; b.seq = swarmSeq++;
  b.t = millis();               // the conductor's clock offset is 0 by definition
  b.amplitude = swarmAmp;
  b.p0 = swarmP[0]; b.p1 = swarmP[1]; b.p2 = swarmP[2]; b.p3 = swarmP[3];
  swarmUdp.beginPacket(IPAddress(255, 255, 255, 255), SWARM_UDP_PORT);
  swarmUdp.write((const uint8_t *)&b, sizeof(b));
  swarmUdp.endPacket();
}

// Conductor fans the sensor routing config out to the whole wall.
void sendSwarmSenseCfg() {
  SwarmSenseCfg m;
  m.hdr.magic = SWARM_MAGIC; m.hdr.key = swarmKey;
  m.type = 5; m.pad[0] = m.pad[1] = m.pad[2] = 0;
  m.cfg = scfg;
  swarmUdp.beginPacket(IPAddress(255, 255, 255, 255), SWARM_UDP_PORT);
  swarmUdp.write((const uint8_t *)&m, sizeof(m));
  swarmUdp.endPacket();
}

void sendSwarmHello() {
  SwarmHello m;
  m.hdr.magic = SWARM_MAGIC; m.hdr.key = swarmKey;
  m.type = 6; m.role = swarmConductor ? 1 : 0; m.pad[0] = m.pad[1] = 0;
  memset(m.host, 0, sizeof(m.host));
  strncpy(m.host, hostName.c_str(), sizeof(m.host) - 1);
  swarmUdp.beginPacket(IPAddress(255, 255, 255, 255), SWARM_UDP_PORT);
  swarmUdp.write((const uint8_t *)&m, sizeof(m));
  swarmUdp.endPacket();
}

// Upsert a discovered peer by IP, evicting the oldest entry when the table fills.
void swarmNotePeer(uint32_t ip, uint8_t role, const char *host) {
  int slot = -1, freeSlot = -1;
  uint32_t oldest = 0xFFFFFFFF; int oldestSlot = 0;
  for (int i = 0; i < SWARM_PEERS_MAX; i++) {
    if (swarmPeers[i].seen && swarmPeers[i].ip == ip) { slot = i; break; }
    if (!swarmPeers[i].seen && freeSlot < 0) freeSlot = i;
    if (swarmPeers[i].seen < oldest) { oldest = swarmPeers[i].seen; oldestSlot = i; }
  }
  if (slot < 0) slot = (freeSlot >= 0) ? freeSlot : oldestSlot;
  swarmPeers[slot].ip = ip;
  swarmPeers[slot].seen = millis();
  swarmPeers[slot].role = role;
  memset(swarmPeers[slot].host, 0, sizeof(swarmPeers[slot].host));
  strncpy(swarmPeers[slot].host, host, sizeof(swarmPeers[slot].host) - 1);
}

void swarmStorePing(uint32_t t, float x, float y) {
  uint8_t slot = 0; uint32_t oldest = 0xFFFFFFFF;
  for (uint8_t i = 0; i < SWARM_PING_MAX; i++) {
    if (!swarmPings[i].used) { slot = i; break; }
    if (swarmPings[i].t < oldest) { oldest = swarmPings[i].t; slot = i; }
  }
  swarmPings[slot] = { t, x, y, true };
}

// Hand gesture on a swarming device: broadcast a ripple source. Every device
// folds it into its overlay; ours is stored directly because own broadcasts
// are ignored on receive. Rate limited so a waving hand is one ripple, not ten.
void swarmGesturePing() {
  static uint32_t tLast = 0;
  if (mode != MODE_SWARM || !swarmActive) return;
  uint32_t now = millis();
  if (tLast && now - tLast < 1000) return;
  tLast = now;
  SwarmPing p;
  p.hdr.magic = SWARM_MAGIC; p.hdr.key = swarmKey;
  p.type = 2; p.pad[0] = p.pad[1] = p.pad[2] = 0;
  p.t = sharedMillis(); p.x = swarmX; p.y = swarmY;
  swarmUdp.beginPacket(IPAddress(255, 255, 255, 255), SWARM_UDP_PORT);
  swarmUdp.write((const uint8_t *)&p, sizeof(p));
  swarmUdp.endPacket();
  swarmStorePing(p.t, p.x, p.y);
  Serial.println("[swarm] gesture ping sent");
}

// Non-blocking receive, called every loop pass. Cheap when idle: parsePacket()
// returns 0 immediately when nothing is waiting.
void swarmNetTask() {
  int len = swarmUdp.parsePacket();
  if (len <= 0) return;
  uint8_t buf[96] __attribute__((aligned(4)));   // >= sizeof(SwarmField) (77)
  if (len > (int)sizeof(buf)) { swarmUdp.flush(); return; }
  swarmUdp.read(buf, len);
  if (len < (int)(sizeof(SwarmHdr) + 1)) return;
  SwarmHdr *h = (SwarmHdr *)buf;
  if (h->magic != SWARM_MAGIC) return;
  uint8_t type = buf[sizeof(SwarmHdr)];

  // Discovery hellos are accepted regardless of key so devices can find each
  // other before they are linked; everything else requires the shared key.
  if (type == 6 && len >= (int)sizeof(SwarmHello)) {
    IPAddress rip = swarmUdp.remoteIP();
    if (rip == WiFi.localIP()) return;       // skip our own broadcast
    SwarmHello *hlo = (SwarmHello *)buf;
    swarmNotePeer((uint32_t)rip, hlo->role, hlo->host);
    return;
  }
  if (h->key != swarmKey) return;

  if (type == 1 && len >= (int)sizeof(SwarmBeacon)) {
    if (swarmConductor) return;              // our own broadcast, looped back
    if (tofControl) return;                  // hand owns this sculpture: ignore the swarm
    SwarmBeacon *b = (SwarmBeacon *)buf;
    uint32_t now = millis();
    // Clock sync: snap on the first beacon from a (re)appearing conductor,
    // then low-pass. ASSUMPTION: one-way LAN latency (~1-5ms) plus tracker
    // smoothing keeps residual error far below what the eye can see.
    int32_t raw = (int32_t)(b->t - now);
    if (swarmLastBeacon == 0 || now - swarmLastBeacon > SWARM_TIMEOUT_MS)
      swarmClkOff = raw;
    else
      swarmClkOff += (int32_t)((raw - swarmClkOff) * 0.1f);
    swarmLastBeacon = now;
    swarmPatternId = b->pattern < SWARM_PATTERNS ? b->pattern : 0;
    swarmAmp = constrain(b->amplitude, 0.0f, 1.0f);
    swarmP[0] = b->p0; swarmP[1] = b->p1; swarmP[2] = b->p2; swarmP[3] = b->p3;
    if (b->active) {
      if (!swarmMuted) {
        if (mode != MODE_SWARM) swarmSetActive(true);   // followers engage via beacon
        else swarmActive = true;
      }
    } else {
      swarmMuted = false;                    // a release re-arms a muted device
      if (mode == MODE_SWARM || swarmActive) swarmSetActive(false);
    }
    if (swarmActive) swarmApplyAutoCeil();   // amplitude changes reach auto-capped devices
    static uint32_t tLog = 0;
    if (now - tLog >= 5000) {
      tLog = now;
      Serial.printf("[swarm] beacon rx off=%ld act=%u pat=%u\n",
                    (long)swarmClkOff, b->active, b->pattern);
    }
  } else if (type == 2 && len >= (int)sizeof(SwarmPing)) {
    IPAddress rip = swarmUdp.remoteIP();
    if (rip == WiFi.localIP() || rip == WiFi.softAPIP()) return;   // own ping
    SwarmPing *p = (SwarmPing *)buf;
    swarmStorePing(p->t, constrain(p->x, 0.0f, 1.0f), constrain(p->y, 0.0f, 1.0f));
    Serial.println("[swarm] gesture ping rx");
  } else if (type == 3 && len >= (int)sizeof(SwarmCue)) {
    // Store raw signals; senseTask() does all the routing/mapping.
    SwarmCue *c = (SwarmCue *)buf;
    senseLastCue = millis();
    sensePresence = constrain(c->presence / 255.0f, 0.0f, 1.0f);
    senseDepth = constrain(c->depth, 0.0f, 1.0f);   // node sends normalised 0..1 (1 = closest)
    senseCx = constrain(c->cx, 0.0f, 1.0f);
    senseCy = constrain(c->cy, 0.0f, 1.0f);
    senseVx = c->vx; senseVy = c->vy;
    senseBlobs = c->blobs;
    // A newly appeared blob (a second hand) drops a persistent ripple, so it
    // layers a ring over whatever the first is already driving.
    if (senseEnabled && c->blobs > sensePrevBlobs)
      swarmStorePing(sharedMillis(), senseCx, senseCy);
    sensePrevBlobs = c->blobs;
  } else if (type == 4 && len >= (int)sizeof(SwarmField)) {
    SwarmField *f = (SwarmField *)buf;
    if (f->w == SENSE_FIELD_W && f->h == SENSE_FIELD_H) {
      memcpy(senseField, f->cells, SENSE_FIELD_W * SENSE_FIELD_H);
      senseLastField = millis();
    }
  } else if (type == 5 && len >= (int)sizeof(SwarmSenseCfg)) {
    if (swarmConductor) return;               // our own broadcast, looped back
    scfg = ((SwarmSenseCfg *)buf)->cfg;       // adopt the conductor's routing
  }
}

void onWsEvent(uint8_t n, WStype_t type, uint8_t *payload, size_t len) {
  if (type == WStype_CONNECTED) {
    wsAuthed[n] = !authRequired();
    char h[192];
    snprintf(h, sizeof(h),
      "{\"type\":\"hello\",\"auth\":%s,\"fw\":\"%s\",\"host\":\"%s\",\"tok\":\"%s\"}",
      authRequired() ? "true" : "false", FW_VERSION, hostName.c_str(),
      authRequired() ? "" : uiToken.c_str());
    ws.sendTXT(n, h);
    return;
  }
  if (type == WStype_DISCONNECTED) { wsAuthed[n] = false; return; }
  if (type != WStype_TEXT) return;
  JsonDocument d;
  if (deserializeJson(d, payload, len)) return;
  const char *c = d["cmd"] | "";

  // Auth handshake and password management come before the command gate.
  if (!strcmp(c, "auth")) {
    if (!authRequired() || uiPass == String((const char*)(d["pw"] | ""))) {
      wsAuthed[n] = true;
      ws.sendTXT(n, String("{\"type\":\"authok\",\"tok\":\"") + uiToken + "\"}");
    } else {
      ws.sendTXT(n, "{\"type\":\"authfail\"}");
    }
    return;
  }
  if (authRequired() && !wsAuthed[n]) {
    ws.sendTXT(n, "{\"type\":\"authfail\"}");   // commands ignored until authed
    return;
  }
  if (!strcmp(c, "setpass")) {
    // Set, change, or clear (empty) the interface password. Requester is
    // already authed (or no password existed). All open sessions stay valid.
    String np = (const char*)(d["pw"] | "");
    if (np.length() > 32) np = np.substring(0, 32);
    uiPass = np;
    if (np.length()) prefs.putString("ui_pass", np); else prefs.remove("ui_pass");
    wsAuthed[n] = true;
    ws.sendTXT(n, "{\"type\":\"passset\"}");
    Serial.printf("[auth] interface password %s\n", np.length() ? "set" : "cleared");
    return;
  }

  // While the hand owns this sculpture (double-tapped into ToF control), remote
  // drive commands from the GUI/fleet are ignored so the person at it always wins.
  bool driveCmd = !strcmp(c, "mode") || !strcmp(c, "enable") || !strcmp(c, "speed");
  if (tofControl && driveCmd) { ws.sendTXT(n, "{\"type\":\"handlock\"}"); return; }

  // Last command wins. App speed overrides the ToF ceiling until the next ToF gesture.
  if (!strcmp(c, "mode")) {
    userSelectMode((uint8_t)constrain((int)(d["v"] | 0), 0, MODE_COUNT - 1));
  } else if (!strcmp(c, "enable")) {
    motorEnabled = d["v"] | false;
  } else if (!strcmp(c, "speed")) {
    // Bidirectional: -100..100. Magnitude sets the ceiling; the sign sets
    // direction in every mode. MANUAL applies the signed value directly; the
    // auto modes inherit the sign through lastManualDir (those that carry their
    // own reversal cadence, SWEEP and STUTTER, seed from it on next entry).
    int pct = constrain((int)(d["v"] | 0), -100, 100);
    // 0% stops; 1..100% maps onto a genuinely moving range so the low end of the
    // slider is not a dead zone (1-2% used to command < start speed).
    int mag = (pct == 0) ? 0 : map(abs(pct), 1, 100, MANUAL_MIN_MOVE, FREQ_MAX);
    maxSpeedCeiling = mag;
    swarmCeilAuto = false;   // a human set this device's cap; stop amplitude-follow
    lastSliderPct = pct;
    if (pct > 0) lastManualDir = 1; else if (pct < 0) lastManualDir = -1;   // 0 keeps last
    if (mode == MANUAL) manualSpeed = (pct >= 0) ? mag : -mag;
  } else if (!strcmp(c, "netcfg")) {
    // Persist network settings then reboot to apply. Passwords are only written
    // when present, so leaving the field blank keeps the stored one.
    prefs.putBool("use_sta", (bool)(d["sta"] | false));
    prefs.putString("sta_ssid", (const char*)(d["ssid"]   | ""));
    prefs.putString("ap_ssid",  (const char*)(d["apssid"] | DEF_AP_SSID));
    prefs.putString("ap_ip",    (const char*)(d["apip"]   | "192.168.4.1"));
    prefs.putString("host",     (const char*)(d["host"]   | DEF_HOST));
    prefs.putBool("use_static", (bool)(d["static"] | false));
    prefs.putString("sta_ip",   (const char*)(d["ip"]   | "0.0.0.0"));
    prefs.putString("sta_gw",   (const char*)(d["gw"]   | "0.0.0.0"));
    prefs.putString("sta_mask", (const char*)(d["mask"] | "255.255.255.0"));
    if (d["pass"].is<const char*>())   prefs.putString("sta_pass", (const char*)d["pass"]);
    if (d["appass"].is<const char*>()) prefs.putString("ap_pass",  (const char*)d["appass"]);
    wsSendAll("{\"type\":\"netsaved\"}");
    delay(300);
    ESP.restart();
  } else if (!strcmp(c, "netreset")) {
    fireNetworkReset();
  } else if (!strcmp(c, "motion")) {
    // Durations arrive in seconds; sweep value is a full period (half = /2).
    cfg.breatheMs   = (uint32_t)constrain((int)(d["bms"] | 20),  2, 600) * 1000UL;
    cfg.sweepHalfMs = (uint32_t)constrain((int)(d["sms"] | 20),  2, 600) * 1000UL / 2;
    float wp        = constrain((float)(d["wms"] | 28.0), 4.0, 300.0);
    cfg.wanderRate  = 6.2832f / (0.45f * wp * 1000.0f);
    cfg.tideMs      = (uint32_t)constrain((int)(d["tmin"] | 8),  1, 60) * 60000UL;
    cfg.pendMs      = (uint32_t)constrain((int)(d["pms"]  | 12), 4, 120) * 1000UL;
    cfg.beatMs      = (uint32_t)constrain((int)(d["hbs"]  | 4),  2, 20) * 1000UL;
    cfg.stutMs      = (uint32_t)constrain((int)(d["sts"]  | 900), 100, 4000);
    cfg.minSpeed    = constrain((int)(d["flo"] | MODE_MIN_SPEED), 0, FREQ_MAX);
    cfg.smoothUp    = constrain((float)(d["up"]  | 0.5), 0.1, 3.0);
    cfg.smoothDown  = constrain((float)(d["dn"]  | 1.1), 0.1, 5.0);
    cfg.breatheShape= constrain((float)(d["bsh"] | 4.0), 1.0, 8.0);
    cfg.sweepShape  = constrain((float)(d["ssh"] | 2.2), 1.0, 6.0);
    queueEnabled    = d["qen"] | false;
    queueLen = 0;
    JsonArrayConst qa = d["q"].as<JsonArrayConst>();
    for (JsonVariantConst st : qa) {
      if (queueLen >= QUEUE_MAX) break;
      queueSteps[queueLen].mode = constrain((int)(st[0] | 0), 0, MODE_COUNT - 2);   // SWARM is not queueable
      queueSteps[queueLen].secs = constrain((int)(st[1] | 10), 1, 3600);
      queueLen++;
    }
    queueStepStart = 0;          // restart the queue from step 0
    saveMotion();
    wsSendAll("{\"type\":\"motionsaved\"}");
  } else if (!strcmp(c, "fwupdate")) {
    fwUrl = (const char*)(d["url"] | "");
    if (fwUrl.length() < 8) {
      wsSendAll("{\"type\":\"fwstatus\",\"s\":\"failed\",\"m\":\"no url\"}");
    } else {
      prefs.putString("fw_url", fwUrl);
      performOtaPull(fwUrl);
    }
  } else if (!strcmp(c, "fleet")) {
    // Shared roster: the page pushes the full list of sculpture addresses to
    // every device it can reach, so opening ANY device's page shows the whole
    // fleet (the list used to live only in one phone's localStorage).
    String fl = "";
    JsonArrayConst la = d["l"].as<JsonArrayConst>();
    for (JsonVariantConst v : la) {
      const char *hs = v | "";
      if (!strlen(hs)) continue;
      if (strchr(hs, '"') || strchr(hs, ',')) continue;   // keep NVS + JSON clean
      if (fl.length() + strlen(hs) + 1 > 480) break;
      if (fl.length()) fl += ",";
      fl += hs;
    }
    fleetList = fl;
    prefs.putString("fl_list", fleetList);
    Serial.printf("[fleet] roster saved (%d bytes)\n", fleetList.length());
  } else if (!strcmp(c, "swarmpos")) {
    swarmX = constrain((float)(d["x"] | 0.5f), 0.0f, 1.0f);
    swarmY = constrain((float)(d["y"] | 0.5f), 0.0f, 1.0f);
    prefs.putFloat("sw_x", swarmX);
    prefs.putFloat("sw_y", swarmY);
    Serial.printf("[swarm] pos=%.2f,%.2f\n", swarmX, swarmY);
  } else if (!strcmp(c, "swarmrole")) {
    swarmConductor = d["conductor"] | false;
    prefs.putBool("sw_cond", swarmConductor);
    if (swarmConductor) { swarmClkOff = 0; swarmLastBeacon = 0; }   // conductor time IS shared time
    else swarmActive = false;
    Serial.printf("[swarm] role=%s\n", swarmConductor ? "conductor" : "follower");
  } else if (!strcmp(c, "swarmkey")) {
    swarmKey = (uint32_t)(d["v"] | 0);
    prefs.putUInt("sw_key", swarmKey);
    Serial.println("[swarm] key set");
  } else if (!strcmp(c, "sense")) {
    senseEnabled = d["on"] | true;
    prefs.putBool("sw_sense", senseEnabled);
    Serial.printf("[sense] respond=%s\n", senseEnabled ? "on" : "off");
  } else if (!strcmp(c, "sensecfg")) {
    // Routing matrix lives on the conductor; SwarmSenseCfg fans it to the wall.
    if (!swarmConductor) { ws.sendTXT(n, "{\"type\":\"err\",\"m\":\"not conductor\"}"); return; }
    scfg.mode = (uint8_t)constrain((int)(d["mode"] | scfg.mode), 0, 6);
    scfg.inv  = (uint8_t)((int)(d["inv"] | scfg.inv) & 0x07);
    scfg.spotR = constrain((float)(d["spotR"] | scfg.spotR), 0.02f, 2.0f);
    JsonArrayConst da = d["dst"].as<JsonArrayConst>();
    JsonArrayConst la = d["lo"].as<JsonArrayConst>();
    JsonArrayConst ha = d["hi"].as<JsonArrayConst>();
    for (int a = 0; a < 3; a++) {
      if (a < (int)da.size()) scfg.dst[a] = (uint8_t)constrain((int)(da[a] | 0), 0, SD_COUNT - 1);
      if (a < (int)la.size()) scfg.lo[a] = constrain((float)(la[a] | 0.0f), -10.0f, 10.0f);
      if (a < (int)ha.size()) scfg.hi[a] = constrain((float)(ha[a] | 1.0f), -10.0f, 10.0f);
    }
    prefs.putBytes("sw_scfg", &scfg, sizeof(scfg));
    sendSwarmSenseCfg();   // reach the wall immediately
    Serial.printf("[sense] cfg mode=%u dst=%u,%u,%u\n",
                  scfg.mode, scfg.dst[0], scfg.dst[1], scfg.dst[2]);
  } else if (!strcmp(c, "swarm")) {
    // Choreography lives on the conductor; the UDP beacon fans it out.
    if (tofControl) { ws.sendTXT(n, "{\"type\":\"handlock\"}"); return; }   // hand owns this device
    if (!swarmConductor) { ws.sendTXT(n, "{\"type\":\"err\",\"m\":\"not conductor\"}"); return; }
    swarmPatternId = (uint8_t)constrain((int)(d["pat"] | (int)swarmPatternId), 0, SWARM_PATTERNS - 1);
    swarmAmp  = constrain((float)(d["amp"] | swarmAmp), 0.0f, 1.0f);
    swarmP[0] = d["p0"] | swarmP[0];
    swarmP[1] = d["p1"] | swarmP[1];
    swarmP[2] = d["p2"] | swarmP[2];
    swarmP[3] = d["p3"] | swarmP[3];
    bool on = d["on"] | swarmActive;
    if (on) swarmSetActive(true);
    else if (swarmActive || mode == MODE_SWARM) { swarmSetActive(false); swarmReleaseBeacons = 6; }
    sendSwarmBeacon();   // params and engage state reach the wall immediately
    Serial.printf("[swarm] %s pat=%u amp=%.2f\n", on ? "engaged" : "released",
                  swarmPatternId, swarmAmp);
  }
#ifdef ENABLE_LEDS
  else if (!strcmp(c, "led")) {
    led.mode = (uint8_t)constrain((int)(d["m"]   | 0), 0, 4);
    led.hue  = (uint16_t)constrain((int)(d["hue"] | 30), 0, 359);
    led.bri  = (uint8_t)constrain((int)(d["bri"] | 60), 0, 100);
    led.rate = (uint8_t)constrain((int)(d["rt"]  | 50), 1, 100);
    ledDirty = true;   // applied immediately; NVS save is debounced in persistTask
    char lb[96];
    snprintf(lb, sizeof(lb), "{\"type\":\"led\",\"m\":%u,\"hue\":%u,\"bri\":%u,\"rt\":%u}",
             led.mode, led.hue, led.bri, led.rate);
    wsSendAll(lb);     // keep every open client's Light tab in sync
  }
#endif
}

// Slider position for cross-client sync. If the current ceiling is exactly the
// one the app's last percent produced, echo that percent verbatim; the forward
// and inverse integer map() calls do not round-trip, and the off-by-one nudged
// the slider on every release. The inverse map is kept only for ceilings set by
// gesture or pot, where no exact percent exists.
int sliderPctForTele() {
  if (maxSpeedCeiling <= 0) return 0;
  if (lastSliderPct != 0 &&
      map(abs(lastSliderPct), 1, 100, MANUAL_MIN_MOVE, FREQ_MAX) == maxSpeedCeiling)
    return lastSliderPct;
  return (int)lastManualDir * (int)map(constrain(maxSpeedCeiling, MANUAL_MIN_MOVE, FREQ_MAX),
                                       MANUAL_MIN_MOVE, FREQ_MAX, 1, 100);
}

void sendTelemetry() {
  if (queueOffPending) {          // firmware cancelled the queue: sync the UI
    queueOffPending = false;
    wsSendAll("{\"type\":\"queueoff\"}");
  }
  char buf[640];   // grown for the swarm + sensor + diagnostics fields; snprintf truncates silently if undersized
  snprintf(buf, sizeof(buf),
    "{\"type\":\"tele\",\"mode\":%u,\"enabled\":%s,\"speed\":%d,\"qi\":%d,\"gesture\":\"%s\","
    "\"derate\":%d,\"fault\":{\"tmc\":%s,\"otp\":%s,\"tof\":%s},\"sg\":%u,"
    "\"netmode\":\"%s\",\"netip\":\"%s\",\"sl\":%d,\"hand\":%d,"
    "\"tof\":%d,\"tofp\":%d,\"ta\":%u,\"dir\":%d,\"heap\":%u,\"up\":%lu,"
    "\"sw\":{\"on\":%d,\"cond\":%d,\"sync\":%d,\"x\":%.2f,\"y\":%.2f,\"pat\":%u,\"amp\":%.2f},"
    "\"sen\":{\"en\":%d,\"cue\":%d,\"g\":%.2f,\"blobs\":%u,\"mode\":%u},"
    "\"leds\":" LEDS_JSON "}",
    mode, motorEnabled ? "true" : "false", appliedFreq,
    (queueEnabled && queueLen && queueStepStart) ? (int)queueIdx : -1, gestureName(),
    (int)(speedDerate * 100),
    faultTmcComm ? "true" : "false",
    faultOvertemp ? "true" : "false",
    faultTof ? "true" : "false",
    sgLoad, netMode.c_str(), netIp.c_str(), sliderPctForTele(), tofControl ? 1 : 0,
    filtDist, tofPresent ? 1 : 0, (unsigned)tofAddr, tofDir, (unsigned)ESP.getFreeHeap(), (unsigned long)(millis() / 1000),
    (mode == MODE_SWARM) ? 1 : 0, swarmConductor ? 1 : 0,
    (swarmConductor || (swarmLastBeacon && millis() - swarmLastBeacon < SWARM_TIMEOUT_MS)) ? 1 : 0,
    swarmX, swarmY, swarmPatternId, swarmAmp,
    senseEnabled ? 1 : 0, senseCueFresh ? 1 : 0, senseGain, senseBlobs, scfg.mode);
  wsSendAll(buf);
}

void loadNetSettings() {
  uiPass = prefs.getString("ui_pass", "");
  char tok[17];
  snprintf(tok, sizeof(tok), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
  uiToken = tok;
  // Per-board suffix from the chip's unique factory ID, so multiple unconfigured
  // boards do not collide on the AP name or sculpture.local hostname.
  char uid[8];
  // ESP.getEfuseMac() returns the base MAC with byte 0 (the OUI vendor prefix)
  // in the LSB, so "& 0xFFFF" gave the same value on every board (e.g. 8C58).
  // The device-unique octets are the LAST two MAC bytes, at bits 32-47.
  snprintf(uid, sizeof(uid), "%04X", (uint16_t)(ESP.getEfuseMac() >> 32));
  apSsid   = prefs.getString("ap_ssid", String(DEF_AP_SSID) + "-" + uid);
  apPass   = prefs.getString("ap_pass", DEF_AP_PASS);
  staSsid  = prefs.getString("sta_ssid", "");
  staPass  = prefs.getString("sta_pass", "");
  hostName = prefs.getString("host", String(DEF_HOST) + "-" + uid);
  useSta   = prefs.getBool("use_sta", false) && staSsid.length() > 0;
  useStatic= prefs.getBool("use_static", false);
  apIP.fromString(prefs.getString("ap_ip", "192.168.4.1"));
  staIP.fromString(prefs.getString("sta_ip", "0.0.0.0"));
  staGw.fromString(prefs.getString("sta_gw", "0.0.0.0"));
  staMask.fromString(prefs.getString("sta_mask", "255.255.255.0"));
  fwUrl = prefs.getString("fw_url", DEF_FW_URL);
  fleetList = prefs.getString("fl_list", "");
}

// Pull a firmware .bin from a URL (e.g. a GitHub release) and self-install.
// Blocks while downloading; the motor is stopped first. On success the device
// reboots inside update(); only failures return here. Needs internet (STA mode).
// Minimal Stream that forwards bytes straight into the OTA Update partition,
// so a download can be streamed into flash even when the server uses chunked
// transfer encoding and reports no Content-Length (which is what GitHub's asset
// CDN does, and which the stock httpUpdate cannot handle).
class OtaSink : public Stream {
public:
  // Image-magic validation lives HERE, on the first dechunked byte, because
  // this is the only layer that sees the actual payload. Peeking the raw TCP
  // stream (the previous approach) reads chunked-encoding size lines and would
  // reject even a valid image served chunked.
  bool badMagic = false;
  uint8_t firstByte = 0;
  bool seen = false;
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *buf, size_t n) override {
    if (!seen && n) {
      seen = true;
      firstByte = buf[0];
      if (firstByte != 0xE9) { badMagic = true; }
    }
    if (badMagic) return 0;   // abort writeToStream without touching flash
    return Update.write((uint8_t *)buf, n);
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
};

void performOtaPull(const String &url) {
  if (!useSta || WiFi.status() != WL_CONNECTED) {
    // No upstream internet (AP mode or link down): a remote pull cannot work.
    wsSendAll("{\"type\":\"fwstatus\",\"s\":\"failed\",\"m\":\"no internet - join your wifi first (Setup, Join wifi), then update from there\"}");
    return;
  }
  otaActive = true;
  motorEnabled = false;
  if (stepper) { stepper->forceStop(); stepper->disableOutputs(); }
  wsSendAll("{\"type\":\"fwstatus\",\"s\":\"downloading\"}");
  ws.loop();
  Serial.printf("[ota] pulling %s\n", url.c_str());

  // Redirects are followed MANUALLY with a fresh connection per hop. GitHub
  // now serves release assets via a ~1.4KB signed redirect (JWT query) to
  // release-assets.githubusercontent.com; the stock HTTPClient auto-follow
  // mishandled it and delivered the redirect's HTML body to Update, which
  // failed with "Decryption error" (first byte not the 0xE9 image magic).
  WiFiClientSecure sc;
  WiFiClient cc;
  HTTPClient http;
  const char *hdrs[] = { "Location", "Content-Type" };
  String cur = url;
  int code = 0;
  for (int hop = 0; hop < 6; hop++) {
    bool https = cur.startsWith("https");
    if (https) { sc.stop(); sc.setInsecure(); sc.setHandshakeTimeout(30); http.begin(sc, cur); }
    else       { cc.stop(); http.begin(cc, cur); }
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.collectHeaders(hdrs, 2);
    http.setTimeout(20000);
    code = http.GET();
    if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
      String loc = http.header("Location");
      http.end();
      if (!loc.length()) { code = -100; break; }
      Serial.printf("[ota] redirect %d -> %s\n", code, loc.substring(0, 80).c_str());
      cur = loc;
      continue;
    }
    break;
  }
  if (code != HTTP_CODE_OK) {
    otaActive = false;
    String m = (code == -100) ? "redirect without Location header"
                              : "http " + String(code) + " (heap " + String(ESP.getFreeHeap()) + ")";
    Serial.printf("[ota] %s\n", m.c_str());
    http.end();
    wsSendAll(String("{\"type\":\"fwstatus\",\"s\":\"failed\",\"m\":\"") + m + "\"}");
    return;
  }


  // getSize() is -1 when the response is chunked; UPDATE_SIZE_UNKNOWN handles that.
  int len = http.getSize();
  OtaSink sink;
  bool ok = Update.begin(len > 0 ? (size_t)len : UPDATE_SIZE_UNKNOWN);
  if (ok) {
    int written = http.writeToStream(&sink);   // decodes chunked + content-length
    ok = !sink.badMagic && (written > 0) && Update.end(true) && Update.isFinished();
  }
  String finalCt = http.header("Content-Type");
  http.end();

  if (sink.badMagic) {
    Update.abort();
    otaActive = false;
    String m = "not a firmware image (server sent " +
               (finalCt.length() ? finalCt : String("unknown type")) +
               ", payload starts 0x" + String(sink.firstByte, HEX) +
               "). The URL must be the raw .bin release asset, e.g. "
               "/releases/download/latest/firmware.bin";
    Serial.printf("[ota] %s\n", m.c_str());
    wsSendAll(String("{\"type\":\"fwstatus\",\"s\":\"failed\",\"m\":\"") + m + "\"}");
    return;
  }

  if (ok) {
    Serial.println("[ota] complete, rebooting");
    wsSendAll("{\"type\":\"fwstatus\",\"s\":\"ok\"}");
    ws.loop();
    delay(300);
    ESP.restart();
  } else {
    otaActive = false;
    String m = String("write failed: ") + Update.errorString() + " (heap " + String(ESP.getFreeHeap()) + ")";
    Serial.printf("[ota] %s\n", m.c_str());
    wsSendAll(String("{\"type\":\"fwstatus\",\"s\":\"failed\",\"m\":\"") + m + "\"}");
  }
}

void startServers() {
  // ArduinoOTA brings up mDNS (hostname + its own service); we add http after.
  ArduinoOTA.setHostname(hostName.c_str());
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([]() {
    otaActive = true;                 // stop the control loop touching the motor
    motorEnabled = false;
    if (stepper) { stepper->forceStop(); stepper->disableOutputs(); }
    Serial.println("[ota] update started, motor disabled");
  });
  ArduinoOTA.onEnd([]() { Serial.println("[ota] complete, rebooting"); });
  ArduinoOTA.onError([](ota_error_t e) {
    otaActive = false;                // failed update: resume normal operation
    Serial.printf("[ota] error %u\n", e);
  });
  ArduinoOTA.begin();
  if (MDNS.addService("http", "tcp", 80))
    Serial.printf("[net] http + OTA advertised as %s.local\n", hostName.c_str());
  http.on("/net", []() {
    if (!httpAuthed()) { http.send(401, "application/json", "{\"err\":\"auth\"}"); return; }
    char b[420];
    snprintf(b, sizeof(b),
      "{\"sta\":%s,\"ssid\":\"%s\",\"apssid\":\"%s\",\"apip\":\"%s\",\"host\":\"%s\","
      "\"static\":%s,\"ip\":\"%s\",\"gw\":\"%s\",\"mask\":\"%s\",\"mode\":\"%s\",\"cur\":\"%s\","
      "\"fwver\":\"%s\",\"fwurl\":\"%s\"}",
      useSta ? "true" : "false", staSsid.c_str(), apSsid.c_str(), apIP.toString().c_str(), hostName.c_str(),
      useStatic ? "true" : "false", staIP.toString().c_str(), staGw.toString().c_str(),
      staMask.toString().c_str(), netMode.c_str(), netIp.c_str(),
      FW_VERSION, fwUrl.c_str());
    http.send(200, "application/json", b);
  });
  http.on("/motion", []() {
    if (!httpAuthed()) { http.send(401, "application/json", "{\"err\":\"auth\"}"); return; }
    String q = "[";
    for (uint8_t i = 0; i < queueLen; i++) {
      if (i) q += ",";
      q += "[" + String(queueSteps[i].mode) + "," + String(queueSteps[i].secs) + "]";
    }
    q += "]";
    float wp = 6.2832f / (0.45f * cfg.wanderRate * 1000.0f);   // rate -> period (s)
    char b[420];
    snprintf(b, sizeof(b),
      "{\"bms\":%u,\"sms\":%u,\"wms\":%.0f,\"tmin\":%u,\"pms\":%u,\"hbs\":%u,"
      "\"up\":%.2f,\"dn\":%.2f,\"bsh\":%.1f,\"ssh\":%.1f,\"flo\":%d,\"sts\":%u,"
      "\"qen\":%s,\"q\":%s}",
      cfg.breatheMs / 1000, cfg.sweepHalfMs * 2 / 1000, wp,
      cfg.tideMs / 60000, cfg.pendMs / 1000, cfg.beatMs / 1000,
      cfg.smoothUp, cfg.smoothDown, cfg.breatheShape, cfg.sweepShape, cfg.minSpeed, cfg.stutMs,
      queueEnabled ? "true" : "false", q.c_str());
    http.send(200, "application/json", b);
  });
  http.on("/fleet", []() {
    if (!httpAuthed()) { http.send(401, "application/json", "{\"err\":\"auth\"}"); return; }
    String b = "{\"l\":[";
    int i = 0;
    while (i < (int)fleetList.length()) {
      int cpos = fleetList.indexOf(',', i);
      if (cpos < 0) cpos = fleetList.length();
      if (i) b += ",";
      b += "\"" + fleetList.substring(i, cpos) + "\"";
      i = cpos + 1;
    }
    b += "]}";
    http.send(200, "application/json", b);
  });
  http.on("/peers", []() {
    if (!httpAuthed()) { http.send(401, "application/json", "{\"err\":\"auth\"}"); return; }
    // Devices heard announcing on the LAN within the TTL. The page uses this to
    // auto-populate the fleet: no typing addresses one by one.
    String b = "{\"peers\":[";
    uint32_t now = millis();
    bool first = true;
    for (int i = 0; i < SWARM_PEERS_MAX; i++) {
      if (!swarmPeers[i].seen || now - swarmPeers[i].seen > SWARM_PEER_TTL_MS) continue;
      IPAddress ip(swarmPeers[i].ip);
      if (!first) b += ",";
      first = false;
      b += "{\"host\":\"" + String(swarmPeers[i].host) + "\",\"ip\":\"" + ip.toString() +
           "\",\"cond\":" + (swarmPeers[i].role & 1 ? "1" : "0") + "}";
    }
    b += "]}";
    http.send(200, "application/json", b);
  });
#ifdef ENABLE_LEDS
  http.on("/led", []() {
    if (!httpAuthed()) { http.send(401, "application/json", "{\"err\":\"auth\"}"); return; }
    char b[96];
    snprintf(b, sizeof(b), "{\"m\":%u,\"hue\":%u,\"bri\":%u,\"rt\":%u}",
             led.mode, led.hue, led.bri, led.rate);
    http.send(200, "application/json", b);
  });
#endif
  http.on("/", []() { http.send_P(200, "text/html", PAGE); });
  http.onNotFound([]() { http.send_P(200, "text/html", PAGE); });  // captive catch-all
  http.begin();
  ws.begin();
  // Heartbeat: ping every 15s, pong within 3s, drop after 2 misses. Phones that
  // lock their screen kill the socket silently; without this the dead client
  // holds one of the library's 5 server slots until TCP timeout, and after a
  // few lock/refresh cycles new connections are refused or flaky.
  ws.enableHeartbeat(15000, 3000, 2);
  ws.onEvent(onWsEvent);
  swarmUdp.begin(SWARM_UDP_PORT);   // swarm clock beacon + gesture ping channel
}

void initWiFi() {
  loadNetSettings();
  bool connected = false;

  if (useSta) {
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);          // we own the creds in NVS; don't wear flash
    WiFi.setAutoReconnect(true);     // core-level retry (backed up by netWatchdog)
    WiFi.setHostname(hostName.c_str());
    if (useStatic && staIP != IPAddress(0, 0, 0, 0)) {
      WiFi.config(staIP, staGw, staMask, staGw);   // gateway doubles as DNS
    }
    WiFi.begin(staSsid.c_str(), staPass.c_str());
    Serial.printf("[net] joining %s ...\n", staSsid.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(250);
    connected = (WiFi.status() == WL_CONNECTED);
  }

  if (connected) {
    WiFi.setSleep(false);   // modem sleep adds latency and drops with phones
    netMode = "STA";
    netIp = WiFi.localIP().toString();
    captiveActive = false;
    Serial.printf("[net] joined %s as %s\n", staSsid.c_str(), netIp.c_str());
  } else {
    // AP fallback (also the default first-boot state).
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    if (apPass.length() >= 8) WiFi.softAP(apSsid.c_str(), apPass.c_str());
    else                      WiFi.softAP(apSsid.c_str());   // open if too short
    WiFi.setSleep(false);   // modem sleep adds latency and drops with phones
    netMode = "AP";
    netIp = WiFi.softAPIP().toString();
    dns.start(53, "*", WiFi.softAPIP());   // captive portal: any host -> our page
    captiveActive = true;
    Serial.printf("[net] AP '%s' at %s\n", apSsid.c_str(), netIp.c_str());
  }
  startServers();
}

// STA link supervision. Without this a dropped connection (router reboot, AP
// hiccup) leaves the board unreachable until a manual reset, and the mDNS
// responder that makes .local work often dies on the drop. Called ~1Hz.
void netWatchdog() {
  if (!useSta) return;               // AP mode has no upstream link to lose
  static bool wasDown = false;
  static uint32_t downSince = 0, lastRetry = 0;
  uint32_t now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (wasDown) {                   // link just came back: refresh address + mDNS
      wasDown = false;
      netIp = WiFi.localIP().toString();
      MDNS.end();
      ArduinoOTA.begin();            // re-registers mDNS hostname + OTA service
      MDNS.addService("http", "tcp", 80);
      Serial.printf("[net] reconnected as %s, mDNS refreshed\n", netIp.c_str());
    }
    return;
  }

  // Disconnected.
  if (!wasDown) { wasDown = true; downSince = now; lastRetry = 0; Serial.println("[net] link lost"); }
  if (now - lastRetry > 5000) { lastRetry = now; WiFi.reconnect(); }      // retry every 5s
  if (now - downSince > 300000) {                                        // unrecoverable: reboot
    Serial.println("[net] down >5 min, rebooting");
    delay(100);
    ESP.restart();
  }
}
#endif  // INPUT_TOF_WIFI

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  // FastAccelStepper owns STEP, DIR and the active-low ENN pin.
  engine.init();
  stepper = engine.stepperConnectToPin(STEP_PIN);
  if (stepper) {
    stepper->setDirectionPin(DIR_PIN);
    stepper->setEnablePin(EN_PIN, true);   // true = LOW enables the driver
    stepper->setAutoEnable(false);         // we manage enable explicitly
    stepper->disableOutputs();             // motor starts disabled
    // Match FAS acceleration to the tracker's own ceiling. The tracker shapes
    // the profile; FAS then ramps smoothly between our per-tick speed commands
    // instead of snapping to each instantly (the snapping showed up as low-speed
    // vibration during the slow wind down). FAS now also bounds accel itself.
    stepper->setAcceleration(MAX_ACCEL);
    stepper->setSpeedInHz(FREQ_MIN);
  } else {
    Serial.println("[stepper] FAULT: could not connect STEP pin");
  }

  initTMC();   // report OK or FAULT, never hang

#ifdef INPUT_POT_BTN
  pinMode(EN_BTN, INPUT_PULLUP);
  pinMode(MODE_BTN, INPUT_PULLUP);
  analogReadResolution(12);
  // Safe start enforced: pot must pass through centre before motion is allowed.
  Serial.println("[boot] pot+button build");
#endif

#ifdef USES_TOF
  bool tofOk = initToF();   // report OK or ABSENT
  tofPresent = tofOk;       // absent at boot is not a fault, just no gestures
#ifndef INPUT_TOF_WIFI
  tofControl = true;        // no web GUI in this build: the hand owns motion by default
#endif
  faultTof = false;
  // No safe start restriction on ToF. Gesture system starts in IDLE.
  Serial.println("[boot] ToF build");
#endif

  loadSettings();           // restore mode + ceiling from NVS
  loadMotion();             // restore motion config + mode queue

#ifdef ENABLE_LEDS
  pixels.begin();
  pixels.clear();
  pixels.show();            // dark until the first ledTask frame
  loadLed();
  Serial.println("[boot] leds: chained rings on GPIO20");
#endif

#ifdef INPUT_TOF_WIFI
  initWiFi();   // WiFi init last
#endif

  motorEnabled = false;     // disabled at boot regardless of restored state
  Serial.println("[boot] ready");
}

// ============================================================
//  LOOP  (cooperative scheduler, millis-gated tasks)
//  WiFi servicing runs every pass for responsiveness; everything else runs on
//  its own cadence. The motion tracker uses real measured dt, so jitter in the
//  control cadence does not affect the smoothness of the motion.
// ============================================================
void loop() {
  uint32_t now = millis();

#ifdef INPUT_TOF_WIFI
  ArduinoOTA.handle();   // accept OTA uploads
  if (captiveActive) dns.processNextRequest();   // captive portal in AP mode
  http.handleClient();   // service web + websocket every pass
  ws.loop();
  swarmNetTask();        // swarm UDP receive, non-blocking
  // Conductor beacon: 2Hz while engaged, and a short trail of release beacons
  // afterwards so every follower reliably sees the swarm end.
  static uint32_t tBeacon = 0;
  if (swarmConductor && (swarmActive || swarmReleaseBeacons) && now - tBeacon >= SWARM_BEACON_MS) {
    tBeacon = now;
    sendSwarmBeacon();
    if (!swarmActive && swarmReleaseBeacons) swarmReleaseBeacons--;
  }
  // Conductor refreshes the sensor routing config at 1Hz so a rejoining follower
  // picks it up (immediate resend also happens on each change).
  static uint32_t tScfg = 0;
  if (swarmConductor && swarmActive && now - tScfg >= 1000) { tScfg = now; sendSwarmSenseCfg(); }
  // Presence announce so any device's page can discover the fleet (all devices,
  // any swarm state, as long as the network is up).
  static uint32_t tHello = 0;
  if (netMode.length() && now - tHello >= SWARM_HELLO_MS) { tHello = now; sendSwarmHello(); }
  static uint32_t tNet = 0;
  if (now - tNet >= 1000) { tNet = now; netWatchdog(); }   // STA link supervision
#endif

  // Control: 100Hz. Compute the mode target, apply the health derate, drive motor.
  static uint32_t tCtrl = 0;
  if (now - tCtrl >= 10) {
    tCtrl = now;
#ifdef INPUT_POT_BTN
    inputUpdate();
#endif
    if (!otaActive) {            // hold still while firmware is being written
      queueTask();              // advance mode queue if enabled
      senseTask();              // ease sensor presence gain + motion direction
      targetFreq = (int)(modeTarget() * speedDerate);
      applyMotion();
    }
  }

  // ToF gesture: 30Hz.
#ifdef USES_TOF
  static uint32_t tTof = 0;
  if (now - tTof >= 33) { tTof = now; gestureTick(); }
#endif

#ifdef ENABLE_LEDS
  // LED animation: ~30Hz. Skipped during OTA so flashing gets full bandwidth.
  static uint32_t tLed = 0;
  if (now - tLed >= 33 && !otaActive) { tLed = now; ledTask(); }
#endif

  // Health poll: 2Hz.
  static uint32_t tHealth = 0;
  if (now - tHealth >= 500) { tHealth = now; pollHealth(); }

  // Persistence check: 1Hz (the actual NVS write is debounced inside).
  static uint32_t tPersist = 0;
  if (now - tPersist >= 1000) { tPersist = now; persistTask(); }

#ifdef INPUT_TOF_WIFI
  // Telemetry broadcast: 4Hz.
  static uint32_t tTele = 0;
  if (now - tTele >= 250) { tTele = now; sendTelemetry(); }
#endif

  delay(1);   // yield to the RTOS / WiFi stack
}

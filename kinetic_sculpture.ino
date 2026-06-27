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

#ifdef USES_TOF
#include <Wire.h>
#endif

#ifdef INPUT_TOF_WIFI
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <HTTPUpdate.h>
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
const int SDA_PIN = 1;     // ToF10120 I2C data
// GPIO2 unused on ToF versions
const int SCL_PIN = 3;     // ToF10120 I2C clock
#endif

// ============================================================
//  CONSTANTS
// ============================================================
#define FREQ_MIN   100      // steps/sec, below this the motor is treated as stopped
#define FREQ_MAX   4000     // steps/sec maximum
#define SMOOTH_TIME_UP   0.5f   // sec, responsiveness when speeding up (lower = snappier)
#define SMOOTH_TIME_DOWN 1.1f   // sec, when slowing down (higher = gentler wind down)
#define MAX_ACCEL  10000    // steps/sec^2, hard ceiling on rate of change
#define DEAD_BAND  180      // ADC counts (pot version only)

// Minimum speed ceiling for the autonomous modes (BREATHE, SWEEP, WANDER) so the
// sculpture always keeps visibly moving and cannot be starved to a standstill by
// the ToF, pot, or app. MANUAL is exempt and keeps its full range including stop.
// Kept above FREQ_MIN / 0.15 so BREATHE's trough (0.15 x ceiling) never stalls.
#define MODE_MIN_SPEED 700  // steps/sec

// Envelope shaping for the autonomous modes. Higher values make the speed
// profile linger longer at the low end (BREATHE) and around the zero crossing
// at each reversal (SWEEP). 1.0 = the original unshaped curve.
#define BREATHE_SHAPE 4.0f  // power on sin: sharper peak, broader low dwell
#define SWEEP_SHAPE   2.2f  // power on ramp: slow crawl away from each reversal

// Cycle timing for the autonomous modes (longer = more languid).
#define BREATHE_PERIOD_MS 20000   // one full breath in/out
#define SWEEP_HALF_MS     10000   // one ramp 0 -> peak (a full sweep is 2x this)
#define WANDER_RATE       0.0005f // time scale for the noise drift (lower = slower)

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

int  currFreq = 0;          // signed steps/sec actually applied
int  targetFreq = 0;        // signed steps/sec requested this tick
bool motorEnabled = false;  // tracked driver enable state

float trackPos = 0.0f;      // smoothed signed frequency (tracker output)
float trackVel = 0.0f;      // tracker velocity state

int  maxSpeedCeiling = FREQ_MAX;  // ToF/pot set ceiling for BREATHE/SWEEP/WANDER
int  manualSpeed = 0;             // signed, frozen between MANUAL speed gestures
int  lastManualDir = 1;           // +1 or -1, inherited by BREATHE

// Health / fault flags, surfaced to Serial, telemetry, and the response logic.
bool     faultTmcComm  = false;   // TMC2209 UART not responding
bool     faultOvertemp = false;   // TMC2209 overtemperature prewarning active
bool     faultTof      = false;   // ToF sensor init or sustained read failure
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
  float    smoothUp    = SMOOTH_TIME_UP;
  float    smoothDown  = SMOOTH_TIME_DOWN;
  float    breatheShape= BREATHE_SHAPE;
  float    sweepShape  = SWEEP_SHAPE;
} cfg;

// Mode queue: an optional playlist that auto-advances the mode on a timer.
#define QUEUE_MAX 8
struct QStep { uint8_t mode; uint16_t secs; };
QStep    queueSteps[QUEUE_MAX];
uint8_t  queueLen = 0;
bool     queueEnabled = false;
uint8_t  queueIdx = 0;
uint32_t queueStepStart = 0;

#define CROSSFADE_MS 1000          // mode change envelope blend duration

// Set to 0 to fully de-energise on disable (true power off, lower heat, no
// holding torque). Safe here because the sculpture geometry is balanced, so
// there is no gravity load to backdrive when current cuts. Set to 1 to keep
// the driver energised at hold current if an unbalanced piece needs holding.
#define HOLD_TORQUE_ON_DISABLE 0

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
  if (!motorEnabled) targetFreq = 0;   // disabled means ease down to a stop

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
  trackPos = smoothDamp(trackPos, (float)targetFreq, trackVel, st, MAX_ACCEL, dt);
  currFreq = (int)lroundf(trackPos);

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

struct ModeDef {
  const char *name;
  ModeFn      fn;
  bool        floorSpeed;   // apply MODE_MIN_SPEED floor (MANUAL is exempt)
};

const ModeDef MODES[] = {
  { "MANUAL",  modeManual,  false },
  { "BREATHE", modeBreathe, true  },
  { "SWEEP",   modeSweep,   true  },
  { "WANDER",  modeWander,  true  },
};
const uint8_t MODE_COUNT = sizeof(MODES) / sizeof(MODES[0]);

// Ceiling for a given mode, floored to MODE_MIN_SPEED unless the mode is exempt.
int modeCeiling(uint8_t m) {
  int cap = maxSpeedCeiling;
  if (MODES[m].floorSpeed && cap < MODE_MIN_SPEED) cap = MODE_MIN_SPEED;
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
  driver.rms_current(900, 0.3);   // 900mA run, 0.3 hold multiplier
  driver.iholddelay(6);
}

void initTMC() {
  Serial1.begin(115200, SERIAL_8N1, UART_RX, UART_TX);
  driver.begin();
  applyDriverConfig();
  uint32_t g = driver.GCONF();
  uartOk = !(g == 0 || g == 0xFFFFFFFF);
  faultTmcComm = !uartOk;
  Serial.printf("[tmc] GCONF=0x%08X %s\n", g, uartOk ? "UART OK" : "UART FAULT");
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
#define COMM_CONFIRM 3    // consecutive bad GCONF reads before flagging comm loss

void pollHealth() {
  uint32_t g = driver.GCONF();
  bool commBad = (g == 0 || g == 0xFFFFFFFF);

  static uint8_t commBadCount = 0;
  if (commBad) { if (commBadCount < 255) commBadCount++; }
  else commBadCount = 0;
  faultTmcComm = (commBadCount >= COMM_CONFIRM);

  // Read overtemp only on a frame whose GCONF looked sane this cycle.
  bool otpw = (!commBad) && driver.otpw();
  static uint8_t otpwCount = 0;
  if (otpw) { if (otpwCount < 255) otpwCount++; }
  else otpwCount = 0;

  if (otpwCount >= OTPW_CONFIRM) {
    faultOvertemp = true;
    speedDerate = 0.5f;                            // back off while genuinely warm
    if (otpwCount >= OTPW_DISABLE) motorEnabled = false;   // sustained: disable + latch
  } else {
    faultOvertemp = false;
    speedDerate = 1.0f;
  }

  if (!faultTmcComm) sgLoad = driver.SG_RESULT();

  static bool prevAny = false;
  bool any = faultTmcComm || faultOvertemp || faultTof;
  if (any != prevAny) {
    Serial.printf("[health] tmc_comm=%d overtemp=%d tof=%d derate=%.2f\n",
                  faultTmcComm, faultOvertemp, faultTof, speedDerate);
    prevAny = any;
  }
}

// ============================================================
//  NVS PERSISTENCE  (shared)
//  Restores mode + ceiling on boot, saves them debounced so a power cycle
//  resumes gracefully. The motor still boots disabled regardless.
// ============================================================
void loadSettings() {
  prefs.begin("sculpt", false);
  mode = prefs.getUChar("mode", MANUAL);
  if (mode >= MODE_COUNT) mode = MANUAL;
  maxSpeedCeiling = prefs.getInt("ceil", FREQ_MAX);
  if (maxSpeedCeiling < 0) maxSpeedCeiling = 0;
  if (maxSpeedCeiling > FREQ_MAX) maxSpeedCeiling = FREQ_MAX;
  Serial.printf("[nvs] restored mode=%u ceil=%d\n", mode, maxSpeedCeiling);
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
      if (m < 0) m = 0; if (m >= MODE_COUNT) m = MODE_COUNT - 1;
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
    mode = (mode + 1) % MODE_COUNT;
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
#define TOF_ADDR 0x52   // ToF10120 I2C address as given in spec

// Window geometry (millimetres)
#define WIN_START      250
#define WIN_END        650
#define DB_LOW         420
#define DB_CENTRE      450
#define DB_HIGH        480
#define GESTURE_EXIT   650
#define SPEED_EXIT     700
#define REENTRY_HYST    50   // must drop 50mm below exit to re-enter
#define NO_HAND_DIST  2000   // sentinel: invalid or beyond usable band = no hand (far)
#define EMA_ALPHA     0.4f   // distance smoothing, 1.0 = no smoothing, lower = calmer

// Still-hold classification, measured from beam entry to exit:
//   < HOLD_IGNORE         ignored (accidental)
//   HOLD_IGNORE..MODE     mode change
//   MODE..ENABLE          enable / disable toggle
//   > ENABLE              network reset to default AP (WiFi build)
#define HOLD_IGNORE_MS  2000
#define HOLD_MODE_MS    5000
#define HOLD_ENABLE_MS 15000

// Window geometry must stay ordered or the gesture math silently misbehaves.
static_assert(WIN_START < DB_LOW,  "window start must be below dead band low");
static_assert(DB_LOW < DB_CENTRE,  "dead band low must be below centre");
static_assert(DB_CENTRE < DB_HIGH, "centre must be below dead band high");
static_assert(DB_HIGH < WIN_END,   "dead band high must be below window end");
static_assert(GESTURE_EXIT <= WIN_END,  "gesture exit must be within the window");
static_assert(SPEED_EXIT > GESTURE_EXIT, "speed exit must be beyond gesture exit");
static_assert(WIN_END < NO_HAND_DIST,    "no hand sentinel must sit beyond the window");

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
  Wire.beginTransmission(TOF_ADDR);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission() != 0) return -1;
  if (Wire.requestFrom(TOF_ADDR, 2) != 2) return -1;
  int hi = Wire.read();
  int lo = Wire.read();
  return (hi << 8) | lo;
}

bool initToF() {
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  delay(50);
  int d = readToFRaw();
  bool ok = (d >= 0);
  Serial.printf("[tof] init %s (d=%d)\n", ok ? "OK" : "FAULT", d);
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
  int d = readToFRaw();
  static uint16_t failRun = 0;
  if (d < 0) { if (failRun < 0xFFFF) failRun++; }   // raw read failed
  else failRun = 0;
  faultTof = (failRun > 60);                         // ~2s of dead sensor at 30Hz
  if (d < 0 || d > NO_HAND_DIST) d = NO_HAND_DIST;   // no hand / out of range = far
  med[medIdx] = d;
  medIdx = (medIdx + 1) % 3;
  int m = median3(med[0], med[1], med[2]);
  if (!emaInit) { emaDist = m; emaInit = true; }
  else          { emaDist += EMA_ALPHA * (m - emaDist); }
  filtDist = (int)(emaDist + 0.5f);
  return true;
}

// Map current distance to a SIGNED manual speed (bidirectional, dead band).
int manualSpeedFromDist(int d) {
  if (d <= DB_LOW) {
    // Forward zone 250..420, closer = faster
    float norm = (float)(DB_LOW - d) / (float)(DB_LOW - WIN_START); // 0..1
    return logSpeed(norm, FREQ_MAX);
  } else if (d >= DB_HIGH) {
    // Reverse zone 480..650, further = faster
    float norm = (float)(d - DB_HIGH) / (float)(WIN_END - DB_HIGH); // 0..1
    return -logSpeed(norm, FREQ_MAX);
  }
  return 0;  // dead band 420..480
}

// Map current distance to a ceiling for BREATHE/SWEEP/WANDER.
// closer = lower ceiling, further = higher ceiling.
int ceilingFromDist(int d) {
  float norm = (float)(d - WIN_START) / (float)(WIN_END - WIN_START); // 0..1
  return logSpeed(norm, FREQ_MAX);
}

void fireModeChange() {
  mode = (mode + 1) % MODE_COUNT;
  Serial.printf("[gesture] mode change -> %u\n", mode);
}

void fireEnableToggle() {
  motorEnabled = !motorEnabled;
  Serial.printf("[gesture] enable toggle -> %s\n", motorEnabled ? "ON" : "OFF");
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
  delay(200);
  ESP.restart();
#else
  Serial.println("[gesture] 15s hold (no network layer in this build)");
#endif
}

void applySpeedControl() {
  // Live mapping while in SPEED_CONTROL, mode dependent.
  if (mode == MANUAL) {
    int s = manualSpeedFromDist(filtDist);
    manualSpeed = s;
    if (s > 0) lastManualDir = 1; else if (s < 0) lastManualDir = -1;
  } else {
    maxSpeedCeiling = ceilingFromDist(filtDist);
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
      // Motor ignores the hand completely here. No speed changes.
      if (abs(d - entryDist) > 30) {
        gstate = G_SPEED;                 // movement confirmed: speed control now
        // Auto enable if disabled, then control speed.
        if (!motorEnabled) {
          motorEnabled = true;
          Serial.println("[gesture] auto-enable on speed control");
        }
        break;
      }
      if (d > GESTURE_EXIT) {             // hand left while still
        uint32_t held = now - tEnter;
        if (held < HOLD_IGNORE_MS) {
          // ignored, accidental
        } else if (held <= HOLD_MODE_MS) {
          fireModeChange();
        } else if (held <= HOLD_ENABLE_MS) {
          fireEnableToggle();
        } else {
          fireNetworkReset();             // > 15s: revert to default AP
        }
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
#define DEF_AP_SSID "KineticSculpture"
#define DEF_AP_PASS "kinetic123"
#define DEF_HOST    "sculpture"
#define OTA_PASS    "kinetic"    // required by the IDE when uploading over WiFi
#define FW_VERSION  "1.3.0"      // shown in the UI; bump on each release

// Loaded network settings + live status.
String    apSsid, apPass, staSsid, staPass, hostName;
IPAddress apIP, staIP, staGw, staMask;
bool      useSta = false, useStatic = false, captiveActive = false;
String    netMode = "AP", netIp = "0.0.0.0";
String    fwUrl = "";              // GitHub (or other) .bin URL for pull updates
void performOtaPull(const String &url);   // defined below, called from the WS handler

// The web app hardcodes one button per mode. If MODE_COUNT changes, the buttons
// in PAGE must change too. This catches the mismatch at build time.
static_assert(MODE_COUNT == 4, "web app PAGE has 4 mode buttons; update it to match MODE_COUNT");

const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Kinetic</title><style>
:root{--h:210;--ink:#070a10;--mist:#dde3ef;--dim:#697287;--line:rgba(255,255,255,.09);--glass:rgba(10,13,20,.55)}
*{box-sizing:border-box;margin:0;-webkit-tap-highlight-color:transparent}
html,body{height:100%}
body{background:var(--ink);color:var(--mist);font-family:system-ui,-apple-system,sans-serif;overflow-x:hidden}
#moire{position:fixed;inset:0;width:100%;height:100%;display:block;z-index:0}
.wrap{position:relative;z-index:1;max-width:440px;margin:0 auto;padding:24px 16px 44px}
.title{font-family:ui-monospace,"SF Mono",Menlo,Consolas,monospace;font-weight:400;font-size:13px;letter-spacing:.55em;text-transform:uppercase;color:var(--mist);text-shadow:0 0 18px hsla(var(--h),90%,60%,.55);padding-left:.55em}
.pills{display:flex;flex-wrap:wrap;gap:6px;margin:11px 0 16px}
.pill{font-family:ui-monospace,Menlo,monospace;font-size:10px;letter-spacing:.12em;text-transform:uppercase;padding:4px 9px;border-radius:99px;border:1px solid var(--line);background:var(--glass);color:var(--dim);backdrop-filter:blur(8px)}
.pill.ok{color:hsl(var(--h),80%,70%);border-color:hsla(var(--h),80%,60%,.35)}
.pill.bad{color:#ff6b6b;border-color:rgba(255,107,107,.4)}
.tabs{display:flex;gap:4px;margin-bottom:14px;background:var(--glass);border:1px solid var(--line);border-radius:12px;padding:4px;backdrop-filter:blur(10px)}
.tab{flex:1;text-align:center;font-family:ui-monospace,Menlo,monospace;font-size:11px;letter-spacing:.1em;text-transform:uppercase;color:var(--dim);padding:9px 0;border-radius:9px;transition:.2s}
.tab.on{color:#fff;background:hsla(var(--h),80%,55%,.18);box-shadow:inset 0 0 16px hsla(var(--h),85%,60%,.14)}
.card{background:var(--glass);border:1px solid var(--line);border-radius:18px;padding:16px;margin-bottom:12px;backdrop-filter:blur(16px) saturate(1.3);box-shadow:0 8px 40px rgba(0,0,0,.35)}
label{font-family:ui-monospace,Menlo,monospace;font-size:10px;letter-spacing:.14em;text-transform:uppercase;color:var(--dim)}
.row{display:flex;gap:8px;align-items:center;justify-content:space-between}
.hero{text-align:center;padding:24px 16px}
.mname{font-family:ui-monospace,Menlo,monospace;font-size:30px;font-weight:600;letter-spacing:.22em;text-transform:uppercase;color:var(--mist);text-shadow:0 0 30px hsla(var(--h),90%,60%,.7);line-height:1}
.mstate{font-family:ui-monospace,Menlo,monospace;font-size:11px;letter-spacing:.2em;text-transform:uppercase;color:var(--dim);margin-top:11px}
.read{font-family:ui-monospace,Menlo,monospace;font-size:13px;color:hsl(var(--h),70%,72%);margin-top:6px;font-variant-numeric:tabular-nums}
.val{font-family:ui-monospace,Menlo,monospace;font-variant-numeric:tabular-nums;color:var(--mist);font-size:13px}
input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:3px;border-radius:3px;margin:16px 0 8px;background:linear-gradient(90deg,hsla(var(--h),70%,60%,.22),hsla(var(--h),70%,60%,.55) 50%,hsla(var(--h),70%,60%,.22))}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:24px;height:24px;border-radius:50%;background:radial-gradient(circle at 35% 30%,#fff,hsl(var(--h),85%,60%));box-shadow:0 0 22px hsla(var(--h),90%,60%,.85),0 0 4px #fff;cursor:pointer}
input[type=range]::-moz-range-thumb{width:24px;height:24px;border:0;border-radius:50%;background:hsl(var(--h),85%,62%);box-shadow:0 0 22px hsla(var(--h),90%,60%,.85)}
.ends{display:flex;justify-content:space-between}
.ends span{font-family:ui-monospace,Menlo,monospace;font-size:9px;letter-spacing:.1em;text-transform:uppercase;color:var(--dim)}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:8px}
.grid4{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:10px}
.chip{font-family:ui-monospace,Menlo,monospace;font-size:12px;letter-spacing:.12em;text-transform:uppercase;border:1px solid var(--line);border-radius:12px;padding:13px 0;color:var(--dim);background:rgba(255,255,255,.02);text-align:center;transition:.25s}
.chip.on{color:#fff;border-color:hsl(var(--h),85%,62%);background:hsla(var(--h),80%,55%,.16);box-shadow:0 0 22px hsla(var(--h),85%,55%,.35),inset 0 0 18px hsla(var(--h),85%,60%,.12)}
.power{width:100%;font-family:ui-monospace,Menlo,monospace;font-size:14px;letter-spacing:.2em;text-transform:uppercase;border:1px solid var(--line);border-radius:14px;padding:16px;color:var(--dim);background:rgba(255,255,255,.02);transition:.25s}
.power.on{color:#fff;border-color:hsl(var(--h),85%,62%);background:hsla(var(--h),80%,55%,.2);box-shadow:0 0 30px hsla(var(--h),85%,55%,.5),inset 0 0 24px hsla(var(--h),85%,60%,.15)}
.ti{width:100%;padding:11px;margin-top:4px;border-radius:10px;border:1px solid var(--line);background:rgba(0,0,0,.35);color:var(--mist);font-size:14px;font-family:ui-monospace,Menlo,monospace}
.fr{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-top:10px}
.fr .ti{width:96px;margin-top:0;text-align:right}
.sub{color:var(--dim);font-size:12px;line-height:1.75}
.nbtn{font-family:ui-monospace,Menlo,monospace;font-size:12px;letter-spacing:.1em;text-transform:uppercase;border:1px solid var(--line);border-radius:10px;padding:11px;color:var(--dim);background:rgba(255,255,255,.02);text-align:center}
.nbtn.on{color:#fff;border-color:hsl(var(--h),85%,62%);background:hsla(var(--h),80%,55%,.16)}
.save{width:100%;margin-top:14px;font-family:ui-monospace,Menlo,monospace;letter-spacing:.15em;text-transform:uppercase;border:0;border-radius:12px;padding:13px;color:#06121f;font-weight:700;background:hsl(var(--h),85%,62%)}
.qrow{display:flex;gap:8px;align-items:center;margin-top:8px}
.qrow select,.qrow input{font-family:ui-monospace,Menlo,monospace;font-size:13px;padding:9px;border-radius:9px;border:1px solid var(--line);background:rgba(0,0,0,.35);color:var(--mist)}
.qrow select{flex:1}
.qrow input{width:74px;text-align:right}
.qx{border:1px solid var(--line);background:rgba(255,80,80,.08);color:#ff8a8a;border-radius:9px;padding:9px 12px;font-family:ui-monospace,monospace}
.addq{width:100%;margin-top:10px;border:1px dashed var(--line);background:transparent;color:var(--dim);border-radius:10px;padding:11px;font-family:ui-monospace,monospace;letter-spacing:.1em;text-transform:uppercase;font-size:11px}
.sw{width:auto;flex:0 0 auto}
.eyebrow{font-family:ui-monospace,Menlo,monospace;font-size:10px;letter-spacing:.18em;text-transform:uppercase;color:hsl(var(--h),60%,68%);margin-bottom:4px;display:block}
@media(prefers-reduced-motion:reduce){.chip,.power,.tab{transition:none}}
</style></head><body>
<canvas id="moire"></canvas>
<div class="wrap">
<div class="title">Kinetic</div>
<div class="pills"><span id="conn" class="pill">offline</span>
<span id="gs" class="pill">idle</span>
<span id="flt" class="pill">health ok</span>
<span id="nip" class="pill">net</span></div>

<div class="tabs">
 <div class="tab on" id="tc" onclick="tab('c')">Control</div>
 <div class="tab" id="tm" onclick="tab('m')">Motion</div>
 <div class="tab" id="tn" onclick="tab('n')">Setup</div>
 <div class="tab" id="th" onclick="tab('h')">Help</div>
</div>

<div id="pc">
 <div class="card hero">
  <div class="mname" id="mname">MANUAL</div>
  <div class="mstate" id="mstate">standby</div>
  <div class="read" id="act">0 st/s</div>
 </div>
 <div class="card">
  <div class="row"><label>Speed / Direction</label><span class="val" id="spv">stop</span></div>
  <input type="range" id="sp" min="-100" max="100" value="0" oninput="spShow(this.value)" onchange="cmd({cmd:'speed',v:+this.value})">
  <div class="ends"><span>&#9664; reverse</span><span>stop</span><span>forward &#9654;</span></div>
 </div>
 <div class="card">
  <label>Mode</label>
  <div class="grid4">
   <div class="chip" id="m0" onclick="cmd({cmd:'mode',v:0})">Manual</div>
   <div class="chip" id="m1" onclick="cmd({cmd:'mode',v:1})">Breathe</div>
   <div class="chip" id="m2" onclick="cmd({cmd:'mode',v:2})">Sweep</div>
   <div class="chip" id="m3" onclick="cmd({cmd:'mode',v:3})">Wander</div>
  </div>
 </div>
 <div class="card"><div class="power" id="en" onclick="tgl()">Motor enable</div></div>
</div>

<div id="pm" style="display:none">
 <div class="card">
  <span class="eyebrow">Cycle durations</span>
  <label>How long each auto mode takes to complete one cycle</label>
  <div class="fr"><label>Breathe period</label><input id="bms" class="ti" type="number" min="2" max="600"> </div>
  <div class="fr"><label>Sweep period</label><input id="sms" class="ti" type="number" min="2" max="600"></div>
  <div class="fr"><label>Wander pace</label><input id="wms" class="ti" type="number" min="4" max="300"></div>
  <div class="sub" style="margin-top:6px">seconds</div>
 </div>
 <div class="card">
  <span class="eyebrow">Speed profile</span>
  <label>How the motor eases between speeds, and how the curves dwell</label>
  <div class="fr"><label>Ramp up</label><input id="up" class="ti" type="number" step="0.1" min="0.1" max="3"></div>
  <div class="fr"><label>Wind down</label><input id="dn" class="ti" type="number" step="0.1" min="0.1" max="5"></div>
  <div class="fr"><label>Breathe dwell</label><input id="bsh" class="ti" type="number" step="0.5" min="1" max="8"></div>
  <div class="fr"><label>Sweep dwell</label><input id="ssh" class="ti" type="number" step="0.1" min="1" max="6"></div>
  <div class="sub" style="margin-top:6px">ramp times in seconds, dwell is a curve sharpness</div>
 </div>
 <div class="card">
  <span class="eyebrow">Mode queue</span>
  <div class="row"><label>Auto-play a sequence of modes</label><input id="qen" type="checkbox" class="sw"></div>
  <div id="qlist"></div>
  <button class="addq" onclick="addStep()">+ Add step</button>
  <div class="sub" style="margin-top:8px">When on, the sculpture steps through this list and loops. It overrides manual mode selection.</div>
 </div>
 <button class="save" onclick="saveMotion()">Save motion settings</button>
 <div class="sub" id="mnote" style="margin-top:10px;text-align:center"></div>
</div>

<div id="pn" style="display:none">
 <div class="card">
  <div class="grid2">
   <div class="nbtn" id="nmAp" onclick="setSta(false)">Access point</div>
   <div class="nbtn" id="nmSta" onclick="setSta(true)">Join wifi</div>
  </div>
  <div id="staF" style="margin-top:12px;display:none">
   <label>Home wifi name</label><input id="ssid" class="ti">
   <label>Home wifi password</label><input id="pass" class="ti" type="password" placeholder="(unchanged)">
   <div class="row" style="margin-top:10px"><label>Use static IP</label><input id="us" type="checkbox" class="sw" onchange="updS()"></div>
   <div id="stF" style="display:none">
    <input id="ip" class="ti" placeholder="IP e.g. 192.168.1.50">
    <input id="gw" class="ti" placeholder="Gateway e.g. 192.168.1.1">
    <input id="mask" class="ti" placeholder="Mask e.g. 255.255.255.0">
   </div>
  </div>
  <div id="apF" style="margin-top:12px">
   <label>Access point name</label><input id="apssid" class="ti">
   <label>Access point password (8+ chars, blank = open)</label><input id="appass" class="ti" type="password" placeholder="(unchanged)">
   <label>Access point IP</label><input id="apip" class="ti">
  </div>
  <label style="margin-top:12px;display:block">Hostname (reach it at name.local)</label>
  <input id="host" class="ti">
  <button class="save" onclick="saveNet()">Save &amp; reboot</button>
  <div class="sub" id="note" style="margin-top:10px"></div>
 </div>
 <div class="card">
  <span class="eyebrow">Firmware</span>
  <div class="row"><label>Installed version</label><span class="val" id="fwver">-</span></div>
  <label style="margin-top:10px;display:block">Update from URL (a .bin, e.g. a GitHub release)</label>
  <input id="fwurl" class="ti" placeholder="https://github.com/you/repo/releases/latest/download/firmware.bin">
  <button class="save" onclick="doUpdate()">Download &amp; install</button>
  <div class="sub" id="fwnote" style="margin-top:10px">Needs the sculpture joined to an internet network (Join wifi above). It downloads, installs, and reboots. Keep it powered.</div>
 </div>
</div>

<div id="ph" style="display:none">
 <div class="card">
  <label style="display:block;margin-bottom:6px">Hand gestures (ToF sensor, hold still then withdraw)</label>
  <div class="sub">Move hand more than 3cm: speed control (closer = slower, bidirectional).<br>Hold still 2 to 5s, then withdraw: next mode.<br>Hold still 5 to 15s, then withdraw: enable / disable.<br>Hold still over 15s, then withdraw: reset network to the default access point.</div>
  <label style="display:block;margin:14px 0 6px">Modes</label>
  <div class="sub">Manual: speed and direction follow your input.<br>Breathe: slow sinusoidal swell, lingers low.<br>Sweep: ramps up, eases through zero, reverses.<br>Wander: organic drifting speed and direction.</div>
  <label style="display:block;margin:14px 0 6px">Status pills (top)</label>
  <div class="sub">Connection, current gesture state, health (turns red on overtemp, driver comms, or sensor fault), and the active network mode and address.</div>
  <label style="display:block;margin:14px 0 6px">Reaching the interface</label>
  <div class="sub">Access point: join the sculpture's wifi, the page opens automatically.<br>Joined to your network: open the hostname above with .local, or the address in the network pill.</div>
 </div>
</div>
</div>

<script>
const MODES=['MANUAL','BREATHE','SWEEP','WANDER'],HUE=[210,165,280,35];
let w,en=false,staMode=false,Q=[],St={speed:0,mode:0,en:false};
function spShow(v){v=+v;spv.textContent=v?((v>0?'+':'')+v+'% '+(v>0?'fwd':'rev')):'stop';}
function tab(t){let p={c:pc,m:pm,n:pn,h:ph},b={c:tc,m:tm,n:tn,h:th};for(let k in p){p[k].style.display=k==t?'block':'none';b[k].className='tab'+(k==t?' on':'');}if(t=='n')loadNet();if(t=='m')loadMotion();}
function connect(){
 w=new WebSocket('ws://'+location.hostname+':81');
 w.onopen=()=>{conn.textContent='online';conn.className='pill ok'};
 w.onclose=()=>{conn.textContent='offline';conn.className='pill';setTimeout(connect,1500)};
 w.onmessage=e=>{const t=JSON.parse(e.data);
  if(t.type=='netsaved'){note.textContent='Saved. Rebooting. Reconnect to your network, then open '+host.value+'.local';return;}
  if(t.type=='motionsaved'){mnote.textContent='Saved and applied';return;}
  if(t.type=='fwstatus'){fwnote.textContent=t.s=='downloading'?'Downloading and installing, do not power off...':('Update failed: '+(t.m||'')); return;}
  if(t.type!='tele')return;
  St.speed=t.speed;St.mode=t.mode;St.en=t.enabled;
  document.documentElement.style.setProperty('--h',HUE[t.mode]||210);
  act.textContent=t.speed+' st/s';
  mname.textContent=MODES[t.mode]||'-';
  mstate.textContent=t.enabled?(t.speed==0?'holding':'running'):'standby';
  gs.textContent=t.gesture;gs.className='pill ok';
  en=t.enabled;let eb=document.getElementById('en');eb.className='power'+(en?' on':'');eb.textContent=en?'Motor enabled':'Motor enable';
  for(let i=0;i<4;i++)document.getElementById('m'+i).className='chip'+(t.mode==i?' on':'');
  let f=t.fault||{};let bad=f.tmc||f.otp||f.tof;
  flt.textContent=f.tmc?'TMC comm':f.otp?('OVERTEMP '+t.derate+'%'):f.tof?'ToF fault':'health ok';
  flt.className=bad?'pill bad':'pill ok';
  nip.textContent=t.netmode+' '+t.netip;
  if(reduce)draw();
 };
}
function cmd(o){if(w&&w.readyState==1)w.send(JSON.stringify(o))}
function tgl(){cmd({cmd:'enable',v:!en})}
function setSta(v){staMode=v;document.getElementById('nmSta').className='nbtn'+(v?' on':'');document.getElementById('nmAp').className='nbtn'+(v?'':' on');staF.style.display=v?'block':'none';apF.style.display=v?'none':'block';}
function updS(){stF.style.display=us.checked?'block':'none';}
function loadNet(){fetch('/net').then(r=>r.json()).then(j=>{
 ssid.value=j.ssid;apssid.value=j.apssid;apip.value=j.apip;host.value=j.host;
 ip.value=j.ip;gw.value=j.gw;mask.value=j.mask;us.checked=j.static;updS();setSta(j.sta);
 fwver.textContent=j.fwver||'-';fwurl.value=j.fwurl||'';
 note.textContent='Now: '+j.mode+' '+j.cur;});}
function doUpdate(){if(!fwurl.value){fwnote.textContent='Enter a .bin URL first';return;}fwnote.textContent='Starting...';cmd({cmd:'fwupdate',url:fwurl.value});}
function saveNet(){
 let o={cmd:'netcfg',sta:staMode,ssid:ssid.value,apssid:apssid.value,apip:apip.value,host:host.value,
  static:us.checked,ip:ip.value,gw:gw.value,mask:mask.value};
 if(pass.value)o.pass=pass.value;if(appass.value)o.appass=appass.value;
 note.textContent='Saving...';cmd(o);
}
function renderQueue(){let h='';Q.forEach((s,i)=>{
 let opts=MODES.map((m,k)=>'<option value="'+k+'"'+(k==s.m?' selected':'')+'>'+m+'</option>').join('');
 h+='<div class="qrow"><select onchange="Q['+i+'].m=+this.value"> '+opts+'</select>'
  +'<input type="number" min="1" max="3600" value="'+s.s+'" onchange="Q['+i+'].s=+this.value"><div class="qx" onclick="delStep('+i+')">&times;</div></div>';});
 qlist.innerHTML=h;}
function addStep(){if(Q.length<8){Q.push({m:1,s:60});renderQueue();}}
function delStep(i){Q.splice(i,1);renderQueue();}
function loadMotion(){fetch('/motion').then(r=>r.json()).then(j=>{
 bms.value=j.bms;sms.value=j.sms;wms.value=j.wms;up.value=j.up;dn.value=j.dn;bsh.value=j.bsh;ssh.value=j.ssh;
 qen.checked=j.qen;Q=(j.q||[]).map(a=>({m:a[0],s:a[1]}));renderQueue();});}
function saveMotion(){
 let o={cmd:'motion',bms:+bms.value,sms:+sms.value,wms:+wms.value,up:+up.value,dn:+dn.value,bsh:+bsh.value,ssh:+ssh.value,
  qen:qen.checked,q:Q.map(s=>[s.m,s.s])};
 mnote.textContent='Saving...';cmd(o);
}
const cv=document.getElementById('moire'),cx=cv.getContext('2d');
const reduce=matchMedia('(prefers-reduced-motion:reduce)').matches;
let pa=0,pb=0,dpr=Math.min(devicePixelRatio||1,2),raf;
function resize(){cv.width=innerWidth*dpr;cv.height=innerHeight*dpr;}
function disc(x,y,ri,ro,ph,hue,al){
 const N=30;
 for(let i=0;i<N;i++){let a=ph+i/N*6.2832;
  cx.beginPath();cx.moveTo(x+Math.cos(a)*ri,y+Math.sin(a)*ri);
  cx.lineTo(x+Math.cos(a)*ro,y+Math.sin(a)*ro);
  cx.strokeStyle='hsla('+hue+',88%,62%,'+al+')';cx.lineWidth=1.1*dpr;cx.stroke();}
 for(let r=1;r<=4;r++){let rr=ri+(ro-ri)*r/4;
  cx.beginPath();cx.arc(x,y,rr,0,6.2832);
  cx.strokeStyle='hsla('+hue+',80%,58%,'+(al*0.6)+')';cx.lineWidth=1*dpr;cx.stroke();}
}
function draw(){
 const ww=cv.width,hh=cv.height,x=ww/2,y=hh*0.42,ro=Math.hypot(ww,hh)*0.55,ri=ro*0.06;
 cx.clearRect(0,0,ww,hh);
 let v=Math.max(-1,Math.min(1,St.speed/4000));
 if(!reduce){pa+=v*0.05;pb-=v*0.05;}
 let hue=HUE[St.mode]||210,al=(St.en?1:0.28);
 cx.globalCompositeOperation='lighter';
 disc(x,y,ri,ro,pa,hue,0.05*al);
 disc(x,y,ri,ro,pb,hue+26,0.05*al);
 cx.globalCompositeOperation='source-over';
 let g=cx.createRadialGradient(x,y,0,x,y,ro);
 g.addColorStop(0,'rgba(7,10,16,0)');g.addColorStop(0.62,'rgba(7,10,16,0)');g.addColorStop(1,'rgba(7,10,16,0.85)');
 cx.fillStyle=g;cx.fillRect(0,0,ww,hh);
}
function loop(){draw();raf=requestAnimationFrame(loop);}
addEventListener('resize',()=>{resize();draw();});
document.addEventListener('visibilitychange',()=>{if(document.hidden)cancelAnimationFrame(raf);else if(!reduce)loop();});
resize();draw();if(!reduce)loop();
connect();
</script></body></html>
)rawliteral";

void onWsEvent(uint8_t n, WStype_t type, uint8_t *payload, size_t len) {
  if (type != WStype_TEXT) return;
  JsonDocument d;
  if (deserializeJson(d, payload, len)) return;
  const char *c = d["cmd"] | "";

  // Last command wins. App speed overrides the ToF ceiling until the next ToF gesture.
  if (!strcmp(c, "mode")) {
    mode = constrain((int)(d["v"] | 0), 0, MODE_COUNT - 1);
  } else if (!strcmp(c, "enable")) {
    motorEnabled = d["v"] | false;
  } else if (!strcmp(c, "speed")) {
    // Bidirectional: -100..100. Magnitude sets the ceiling; in MANUAL the sign
    // also sets direction. Auto modes take only the magnitude (their direction
    // is programmatic), so the slider behaves as a speed ceiling there.
    int pct = constrain((int)(d["v"] | 0), -100, 100);
    int mag = map(abs(pct), 0, 100, 0, FREQ_MAX);
    maxSpeedCeiling = mag;
    if (mode == MANUAL) {
      manualSpeed = (pct >= 0) ? mag : -mag;
      if (pct > 0) lastManualDir = 1; else if (pct < 0) lastManualDir = -1;
    }
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
    ws.broadcastTXT("{\"type\":\"netsaved\"}");
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
    cfg.smoothUp    = constrain((float)(d["up"]  | 0.5), 0.1, 3.0);
    cfg.smoothDown  = constrain((float)(d["dn"]  | 1.1), 0.1, 5.0);
    cfg.breatheShape= constrain((float)(d["bsh"] | 4.0), 1.0, 8.0);
    cfg.sweepShape  = constrain((float)(d["ssh"] | 2.2), 1.0, 6.0);
    queueEnabled    = d["qen"] | false;
    queueLen = 0;
    JsonArrayConst qa = d["q"].as<JsonArrayConst>();
    for (JsonVariantConst st : qa) {
      if (queueLen >= QUEUE_MAX) break;
      queueSteps[queueLen].mode = constrain((int)(st[0] | 0), 0, MODE_COUNT - 1);
      queueSteps[queueLen].secs = constrain((int)(st[1] | 10), 1, 3600);
      queueLen++;
    }
    queueStepStart = 0;          // restart the queue from step 0
    saveMotion();
    ws.broadcastTXT("{\"type\":\"motionsaved\"}");
  } else if (!strcmp(c, "fwupdate")) {
    fwUrl = (const char*)(d["url"] | "");
    if (fwUrl.length() < 8) {
      ws.broadcastTXT("{\"type\":\"fwstatus\",\"s\":\"failed\",\"m\":\"no url\"}");
    } else {
      prefs.putString("fw_url", fwUrl);
      performOtaPull(fwUrl);
    }
  }
}

void sendTelemetry() {
  char buf[320];
  snprintf(buf, sizeof(buf),
    "{\"type\":\"tele\",\"mode\":%u,\"enabled\":%s,\"speed\":%d,\"gesture\":\"%s\","
    "\"derate\":%d,\"fault\":{\"tmc\":%s,\"otp\":%s,\"tof\":%s},\"sg\":%u,"
    "\"netmode\":\"%s\",\"netip\":\"%s\"}",
    mode, motorEnabled ? "true" : "false", currFreq, gestureName(),
    (int)(speedDerate * 100),
    faultTmcComm ? "true" : "false",
    faultOvertemp ? "true" : "false",
    faultTof ? "true" : "false",
    sgLoad, netMode.c_str(), netIp.c_str());
  ws.broadcastTXT(buf);
}

void loadNetSettings() {
  apSsid   = prefs.getString("ap_ssid", DEF_AP_SSID);
  apPass   = prefs.getString("ap_pass", DEF_AP_PASS);
  staSsid  = prefs.getString("sta_ssid", "");
  staPass  = prefs.getString("sta_pass", "");
  hostName = prefs.getString("host", DEF_HOST);
  useSta   = prefs.getBool("use_sta", false) && staSsid.length() > 0;
  useStatic= prefs.getBool("use_static", false);
  apIP.fromString(prefs.getString("ap_ip", "192.168.4.1"));
  staIP.fromString(prefs.getString("sta_ip", "0.0.0.0"));
  staGw.fromString(prefs.getString("sta_gw", "0.0.0.0"));
  staMask.fromString(prefs.getString("sta_mask", "255.255.255.0"));
  fwUrl = prefs.getString("fw_url", "");
}

// Pull a firmware .bin from a URL (e.g. a GitHub release) and self-install.
// Blocks while downloading; the motor is stopped first. On success the device
// reboots inside update(); only failures return here. Needs internet (STA mode).
void performOtaPull(const String &url) {
  otaActive = true;
  motorEnabled = false;
  if (stepper) { stepper->forceStop(); stepper->disableOutputs(); }
  ws.broadcastTXT("{\"type\":\"fwstatus\",\"s\":\"downloading\"}");
  ws.loop();
  Serial.printf("[ota] pulling %s\n", url.c_str());

  httpUpdate.rebootOnUpdate(true);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // GitHub redirects to a CDN
  t_httpUpdate_return r;
  if (url.startsWith("https")) {
    WiFiClientSecure sc; sc.setInsecure();   // skip cert check (hobby device)
    r = httpUpdate.update(sc, url);
  } else {
    WiFiClient c;
    r = httpUpdate.update(c, url);
  }

  // Only reached if the update failed (success reboots).
  otaActive = false;
  String msg = httpUpdate.getLastErrorString();
  Serial.printf("[ota] failed (%d): %s\n", r, msg.c_str());
  String j = "{\"type\":\"fwstatus\",\"s\":\"failed\",\"m\":\"" + msg + "\"}";
  ws.broadcastTXT(j);
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
    String q = "[";
    for (uint8_t i = 0; i < queueLen; i++) {
      if (i) q += ",";
      q += "[" + String(queueSteps[i].mode) + "," + String(queueSteps[i].secs) + "]";
    }
    q += "]";
    float wp = 6.2832f / (0.45f * cfg.wanderRate * 1000.0f);   // rate -> period (s)
    char b[320];
    snprintf(b, sizeof(b),
      "{\"bms\":%u,\"sms\":%u,\"wms\":%.0f,\"up\":%.2f,\"dn\":%.2f,\"bsh\":%.1f,\"ssh\":%.1f,"
      "\"qen\":%s,\"q\":%s}",
      cfg.breatheMs / 1000, cfg.sweepHalfMs * 2 / 1000, wp,
      cfg.smoothUp, cfg.smoothDown, cfg.breatheShape, cfg.sweepShape,
      queueEnabled ? "true" : "false", q.c_str());
    http.send(200, "application/json", b);
  });
  http.on("/", []() { http.send_P(200, "text/html", PAGE); });
  http.onNotFound([]() { http.send_P(200, "text/html", PAGE); });  // captive catch-all
  http.begin();
  ws.begin();
  ws.onEvent(onWsEvent);
}

void initWiFi() {
  loadNetSettings();
  bool connected = false;

  if (useSta) {
    WiFi.mode(WIFI_STA);
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
    netMode = "AP";
    netIp = WiFi.softAPIP().toString();
    dns.start(53, "*", WiFi.softAPIP());   // captive portal: any host -> our page
    captiveActive = true;
    Serial.printf("[net] AP '%s' at %s\n", apSsid.c_str(), netIp.c_str());
  }
  startServers();
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
  bool tofOk = initToF();   // report OK or FAULT
  faultTof = !tofOk;
  // No safe start restriction on ToF. Gesture system starts in IDLE.
  Serial.println("[boot] ToF build");
#endif

  loadSettings();           // restore mode + ceiling from NVS
  loadMotion();             // restore motion config + mode queue

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
      targetFreq = (int)(modeTarget() * speedDerate);
      applyMotion();
    }
  }

  // ToF gesture: 30Hz.
#ifdef USES_TOF
  static uint32_t tTof = 0;
  if (now - tTof >= 33) { tTof = now; gestureTick(); }
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

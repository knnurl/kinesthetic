/*
 * KINESTHETIC SENSOR NODE  ESP32-C3 Super Mini + VL53L5CX (8x8 ToF array)
 *
 * A stepper-less companion to the kinetic sculpture. It reads an 8x8 depth
 * frame, turns it into presence / blob-centroid / motion cues (and, for the
 * MIRROR pattern, a low-res depth image), and broadcasts them over the same
 * swarm UDP channel the wall already listens on. It MODULATES the swarm; it is
 * not the conductor. See SENSOR_PLAN.md.
 *
 * LIBRARIES (Arduino Library Manager):
 *     SparkFun VL53L5CX Arduino Library   (SparkFun)
 *     WiFi, WiFiUdp, Wire                 (built in)
 *
 * WIRING (VL53L5CX in I2C mode): VDD->3V3, GND->GND, SDA->GPIO1, SCL->GPIO3.
 * Only SDA/SCL/power are needed; LPn/INT/RST are optional and unused here.
 *
 * STYLE: no em dashes; assumptions tagged // ASSUMPTION:.
 */

#include <Wire.h>
#include <SparkFun_VL53L5CX_Library.h>
#include <WiFi.h>
#include <WiFiUdp.h>

// ============================================================
//  CONFIG  (edit these for your install)
// ============================================================
#define WIFI_SSID   "YOUR_WIFI"        // the same LAN the wall is on (STA)
#define WIFI_PASS   "YOUR_PASSWORD"
#define SWARM_KEY   0u                  // MUST match the wall's swarm key (0 = unset default)

// I2C pins for the VL53L5CX (match the sculpture's ToF wiring by default).
#define SDA_PIN     1
#define SCL_PIN     3

// Sensor -> wall coordinate mapping. The sensor faces the viewer, so its left
// and right are mirrored relative to the wall. Flip these if follow-you runs
// the wrong way or upside down after a bench test.
#define MAP_MIRROR_X  1                 // 1: wall_x = 1 - zone_x
#define MAP_FLIP_Y    0                 // 1: wall_y = 1 - zone_y
#define MAP_TRANSPOSE 0                 // 1: swap axes (sensor mounted rotated 90)

// Detection tuning (millimetres unless noted). The sensor faces OUT from the
// wall, so distance = how far a hand is in front of the wall. Depth is
// normalised against the hand-reach band below (install dependent).
#define BG_FRAMES        32             // frames averaged for the empty-scene reference
#define BG_MARGIN_MM     200            // must be at least this much closer than background
#define BG_MARGIN_FRAC   0.08f          // ...or this fraction of the background distance
#define BG_FAR_MM        4000           // assumed background for zones with no valid boot reading
#define HAND_NEAR_MM     150            // at/inside this = depth 1.0 (closest to the wall)
#define HAND_FAR_MM      1200           // at/beyond this = depth 0.0 (arm's length out)
#define NEAR_LIMIT_MM    HAND_NEAR_MM   // full field intensity at the near limit
#define PRESENCE_ZONES   12.0f          // this many active zones = full presence
#define DEBUG_HEATMAP    0             // 1: print an ASCII depth map + blobs to Serial

// ============================================================
//  SWARM WIRE PROTOCOL  (KEEP IN SYNC WITH kinetic_sculpture.ino)
// ============================================================
#define SWARM_UDP_PORT   47269
#define SWARM_MAGIC      0x4B535731UL   // 'KSW1'
#define SENSE_FIELD_W    8
#define SENSE_FIELD_H    8

struct __attribute__((packed)) SwarmHdr { uint32_t magic; uint32_t key; };
struct __attribute__((packed)) SwarmBeacon {   // received, to learn engage state + pattern
  SwarmHdr hdr; uint8_t type, pattern, active, seq; uint32_t t;
  float amplitude, p0, p1, p2, p3;
};
struct __attribute__((packed)) SwarmCue {
  SwarmHdr hdr; uint8_t type, presence, blobs, flags;
  float cx, cy, vx, vy, depth;
};
struct __attribute__((packed)) SwarmField {
  SwarmHdr hdr; uint8_t type, w, h, seq;
  uint8_t cells[SENSE_FIELD_W * SENSE_FIELD_H];
};

// ============================================================
//  STATE
// ============================================================
SparkFun_VL53L5CX imager;
VL53L5CX_ResultsData meas;
WiFiUDP udp;

uint16_t bg[64];                 // per-zone background distance
bool     bgReady = false;
uint8_t  fieldSeq = 0;

// Smoothed cue outputs.
float presEMA = 0.0f;            // 0..1 presence
float cxEMA = 0.5f, cyEMA = 0.5f;
float vxEMA = 0.0f, vyEMA = 0.0f;
float cxPrev = 0.5f, cyPrev = 0.5f;
uint32_t tPrevFrame = 0;

// Swarm engage state, learned from beacons, gates field-frame TX.
bool    swarmEngaged = false;
uint8_t swarmPattern = 1;
uint32_t lastBeacon = 0;

#define SWARM_MIRROR 5

// ============================================================
//  HELPERS
// ============================================================
// Map a sensor grid cell (col, row) to normalised wall coordinates.
void cellToWall(int col, int row, float &wx, float &wy) {
  float zx = col / (float)(SENSE_FIELD_W - 1);
  float zy = row / (float)(SENSE_FIELD_H - 1);
  if (MAP_TRANSPOSE) { float t = zx; zx = zy; zy = t; }
  wx = MAP_MIRROR_X ? (1.0f - zx) : zx;
  wy = MAP_FLIP_Y   ? (1.0f - zy) : zy;
}

bool zoneValid(int i) {
  // Matches the tested reference: a zone counts only when it actually detected
  // a target and the ST ranging status is 5 or 9 (the datasheet's valid codes).
  if (meas.nb_target_detected[i] == 0) return false;
  uint8_t s = meas.target_status[i];
  return (s == 5 || s == 9) && meas.distance_mm[i] > 0;
}

void captureBackground() {
  uint32_t sum[64] = { 0 }; uint16_t cnt[64] = { 0 };
  for (int f = 0; f < BG_FRAMES; ) {
    if (imager.isDataReady() && imager.getRangingData(&meas)) {
      for (int i = 0; i < 64; i++) if (zoneValid(i)) { sum[i] += meas.distance_mm[i]; cnt[i]++; }
      f++;
    }
    delay(5);
  }
  for (int i = 0; i < 64; i++) bg[i] = cnt[i] ? (uint16_t)(sum[i] / cnt[i]) : BG_FAR_MM;
  bgReady = true;
  Serial.println("[sense] background captured");
}

// A zone is active when it reads meaningfully closer than its background.
bool zoneActive(int i) {
  if (!zoneValid(i)) return false;
  uint16_t b = bg[i];
  float margin = BG_MARGIN_MM;
  if (b * BG_MARGIN_FRAC > margin) margin = b * BG_MARGIN_FRAC;
  return meas.distance_mm[i] < (int)b - (int)margin;
}

// ============================================================
//  FRAME PROCESSING  ->  broadcast cue (+ field)
// ============================================================
void processFrame() {
  bool active[64];
  int activeCount = 0;
  for (int i = 0; i < 64; i++) { active[i] = zoneActive(i); if (active[i]) activeCount++; }

  // Speckle rejection: drop lone active zones with no active 4-neighbour.
  bool clean[64];
  for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) {
    int i = r * 8 + c; clean[i] = false;
    if (!active[i]) continue;
    bool nb = (c > 0 && active[i - 1]) || (c < 7 && active[i + 1]) ||
              (r > 0 && active[i - 8]) || (r < 7 && active[i + 8]);
    clean[i] = nb;
  }

  // Connected components (4-connectivity), keep the heaviest blob as primary.
  int label[64]; for (int i = 0; i < 64; i++) label[i] = -1;
  int stack[64], blobCount = 0;
  float bestWeight = 0.0f, bx = 0.5f, by = 0.5f, bDepth = 0.0f;
  for (int s = 0; s < 64; s++) {
    if (!clean[s] || label[s] >= 0) continue;
    int sp = 0; stack[sp++] = s; label[s] = blobCount;
    float wsum = 0, xsum = 0, ysum = 0, dsum = 0; int nsum = 0;
    while (sp) {
      int i = stack[--sp], c = i % 8, r = i / 8;
      float w = (float)bg[i] - meas.distance_mm[i]; if (w < 1) w = 1;   // closeness weight
      float wx, wy; cellToWall(c, r, wx, wy);
      wsum += w; xsum += w * wx; ysum += w * wy; dsum += meas.distance_mm[i]; nsum++;
      int nb[4] = { (c > 0 ? i - 1 : -1), (c < 7 ? i + 1 : -1),
                    (r > 0 ? i - 8 : -1), (r < 7 ? i + 8 : -1) };
      for (int k = 0; k < 4; k++) if (nb[k] >= 0 && clean[nb[k]] && label[nb[k]] < 0) {
        label[nb[k]] = blobCount; stack[sp++] = nb[k];
      }
    }
    blobCount++;
    if (wsum > bestWeight) { bestWeight = wsum; bx = xsum / wsum; by = ysum / wsum;
                             bDepth = dsum / (nsum > 0 ? nsum : 1); }   // mean distance, mm
  }
  if (blobCount > 4) blobCount = 4;

  // Presence: area driven, gently boosted when the primary blob is close.
  float pres = activeCount / PRESENCE_ZONES; if (pres > 1) pres = 1;
  // Attack fast so the wall wakes at once; release slow so it calms gently.
  float a = (pres > presEMA) ? 0.5f : 0.05f;
  presEMA += (pres - presEMA) * a;

  // Centroid + velocity, smoothed. Hold last centroid when nobody is present.
  uint32_t now = millis();
  float dt = tPrevFrame ? (now - tPrevFrame) / 1000.0f : 0.066f;
  if (dt < 0.001f) dt = 0.001f;
  tPrevFrame = now;
  if (blobCount > 0) {
    cxEMA += (bx - cxEMA) * 0.3f;
    cyEMA += (by - cyEMA) * 0.3f;
    float vx = (cxEMA - cxPrev) / dt, vy = (cyEMA - cyPrev) / dt;
    vxEMA += (vx - vxEMA) * 0.3f; vyEMA += (vy - vyEMA) * 0.3f;
  } else {
    vxEMA *= 0.8f; vyEMA *= 0.8f;   // decay motion when the scene empties
  }
  cxPrev = cxEMA; cyPrev = cyEMA;

  bool present = blobCount > 0;
  bool sendField = swarmEngaged && swarmPattern == SWARM_MIRROR &&
                   (now - lastBeacon < 3000);

  // Broadcast the cue: fast while present, a 2 Hz heartbeat when idle.
  static uint32_t tCue = 0;
  uint32_t cueGap = present ? 66 : 500;
  if (now - tCue >= cueGap) {
    tCue = now;
    SwarmCue cue;
    cue.hdr.magic = SWARM_MAGIC; cue.hdr.key = SWARM_KEY;
    cue.type = 3;
    cue.presence = (uint8_t)(presEMA * 255.0f);
    cue.blobs = (uint8_t)blobCount;
    cue.flags = sendField ? 1 : 0;
    cue.cx = cxEMA; cue.cy = cyEMA; cue.vx = vxEMA; cue.vy = vyEMA;
    // Normalise distance-from-wall to 0..1 (1 = closest). The wall routes this.
    float dn = (float)(HAND_FAR_MM - bDepth) / (float)(HAND_FAR_MM - HAND_NEAR_MM);
    cue.depth = present ? constrain(dn, 0.0f, 1.0f) : 0.0f;
    udp.beginPacket(IPAddress(255, 255, 255, 255), SWARM_UDP_PORT);
    udp.write((const uint8_t *)&cue, sizeof(cue));
    udp.endPacket();
  }

  // Broadcast the depth field for MIRROR at ~10 Hz.
  static uint32_t tField = 0;
  if (sendField && now - tField >= 100) {
    tField = now;
    SwarmField ff;
    ff.hdr.magic = SWARM_MAGIC; ff.hdr.key = SWARM_KEY;
    ff.type = 4; ff.w = SENSE_FIELD_W; ff.h = SENSE_FIELD_H; ff.seq = fieldSeq++;
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) {
      int i = r * 8 + c;
      float wx, wy; cellToWall(c, r, wx, wy);
      int wi = (int)(wy * (SENSE_FIELD_H - 1) + 0.5f) * SENSE_FIELD_W +
               (int)(wx * (SENSE_FIELD_W - 1) + 0.5f);
      uint8_t val = 0;
      if (clean[i]) {
        int d = meas.distance_mm[i];
        int lo = NEAR_LIMIT_MM, hi = bg[i];
        if (hi <= lo + 1) hi = lo + 1;
        int inten = 255 - (int)((long)(d - lo) * 254 / (hi - lo));  // closer = brighter
        val = (uint8_t)constrain(inten, 1, 255);
      }
      ff.cells[wi] = val;   // store already mapped to wall orientation
    }
    udp.beginPacket(IPAddress(255, 255, 255, 255), SWARM_UDP_PORT);
    udp.write((const uint8_t *)&ff, sizeof(ff));
    udp.endPacket();
  }

#if DEBUG_HEATMAP
  static uint32_t tDbg = 0;
  if (now - tDbg >= 500) {
    tDbg = now;
    Serial.printf("[sense] blobs=%d active=%d pres=%.2f c=(%.2f,%.2f) v=(%.2f,%.2f)\n",
                  blobCount, activeCount, presEMA, cxEMA, cyEMA, vxEMA, vyEMA);
    for (int r = 0; r < 8; r++) {
      char line[16];
      for (int c = 0; c < 8; c++) line[c] = clean[r * 8 + c] ? '#' : (zoneValid(r * 8 + c) ? '.' : ' ');
      line[8] = 0; Serial.println(line);
    }
  }
#endif
}

// Learn swarm engage state + current pattern from beacons (gates field TX).
void readBeacons() {
  int len = udp.parsePacket();
  if (len < (int)sizeof(SwarmBeacon)) return;
  uint8_t buf[96];
  if (len > (int)sizeof(buf)) { udp.flush(); return; }
  udp.read(buf, len);
  SwarmHdr *h = (SwarmHdr *)buf;
  if (h->magic != SWARM_MAGIC || h->key != SWARM_KEY) return;
  if (buf[sizeof(SwarmHdr)] != 1) return;   // only beacons interest us
  SwarmBeacon *b = (SwarmBeacon *)buf;
  swarmEngaged = b->active;
  swarmPattern = b->pattern;
  lastBeacon = millis();
}

// ============================================================
//  SETUP / LOOP
// ============================================================
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("[net] joining %s ...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(250);
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);
    Serial.printf("[net] joined as %s\n", WiFi.localIP().toString().c_str());
    udp.begin(SWARM_UDP_PORT);
  } else {
    Serial.println("[net] join failed, will retry");
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("[boot] kinesthetic sensor node");

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(1000000);   // 1MHz, matching the tested config; needs short wires
  if (!imager.begin()) {
    Serial.println("[sense] FAULT: VL53L5CX not found (check wiring/address)");
    while (1) delay(1000);   // nothing useful to do without the sensor
  }
  imager.setResolution(8 * 8);
  imager.setRangingFrequency(15);   // 15 Hz is the 8x8 ceiling
  imager.setIntegrationTime(10);
  imager.setTargetOrder(SF_VL53L5CX_TARGET_ORDER::CLOSEST);   // report the nearest thing per zone
  imager.startRanging();

  ensureWiFi();
  captureBackground();
  Serial.println("[boot] ready");
}

void loop() {
  static uint32_t tNet = 0;
  if (millis() - tNet >= 2000) { tNet = millis(); ensureWiFi(); }

  readBeacons();

  if (imager.isDataReady() && imager.getRangingData(&meas)) {
    if (bgReady) processFrame();
  }
  delay(2);   // yield to WiFi/RTOS; frames arrive at ~15 Hz regardless
}

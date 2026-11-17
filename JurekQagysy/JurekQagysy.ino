#include "secrets.h" // Copy secrets.example.h and configure locally.
/*
  ESP32-S3 + MAX30102: 60-second PPG pulse-interval research prototype.
  SDA=8, SCL=9, VIN=3.3 V, GND=GND; Serial 115200.
  Uses the bundled SparkFun MAX3010x 1.1.2 driver. heartRate.h is retained for
  compatibility, but checkForBeat() is NOT called (fixed amplitude gate,
  narrow intermediate types and non-resettable global history).

  Acquisition: 400 conversions/s, hardware averaging/decimation by 4 =>
  100 FIFO frames/s, 10 ms nominal resolution. Two active LEDs => 6 bytes
  per frame, RED then IR. Read hardware FIFO directly with checked I2C
  transactions, bypassing SparkFun's 4-slot software ring. Do not mix this
  reader with getIR()/check()/available(). Bundled driver changes only fix
  a macro name collision and an unused local; buffer sizes are preserved.

  PPG intervals estimate PRV, not ECG-confirmed NN intervals. Motion,
  pulse transit time, ectopy and missed/double pulse detection remain
  confounders. Validate against synchronized ECG and annotated raw PPG.
  Nominal sensor sample clock (not synchronized to ESP32) sets RR scale.
  The 400 ms minimum deliberately limits this resting protocol to <=150 BPM.
  The minimum 50 accepted intervals/minute can label slow valid rhythms POOR.
  RMSSD thresholds <25, 25..40, >40 ms are EXPERIMENTAL categories, NOT
  a medical diagnosis of stress. LOW_HRV does not mean HIGH_STRESS.
  No imputation, no splitting long RR, no differences across data gaps.
*/
#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "src/max3010x/MAX30105.h"
#include "src/max3010x/heartRate.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

constexpr uint8_t SDA_PIN = 8, SCL_PIN = 9, SENSOR_ADDRESS = 0x57;

// Fill in these three values before uploading this separate Firebase sketch.
// FIREBASE_DATABASE_URL must not end with '/'. Example:
// https://my-project-default-rtdb.asia-southeast1.firebasedatabase.app



// Leave empty only when Realtime Database rules temporarily allow writes.
// Otherwise use a valid Firebase Realtime Database auth token.

constexpr uint32_t FINGER_THRESHOLD = 8000;
constexpr uint32_t FINGER_LOSS_MS = 2500, STABILIZATION_MS = 5000;
constexpr uint32_t MEASUREMENT_DURATION_MS = 60000, SAMPLE_MS = 10;
// The recorded MAX30102 PPG falls at the pulse; detect the trough.
// Set +1 only for an independently verified inverted optical waveform.
constexpr float PPG_POLARITY = -1.0f;
constexpr uint32_t MIN_RR_MS = 400, MAX_RR_MS = 1500;
constexpr uint16_t RR_SIZE = 180, BASELINE_SIZE = 9;
constexpr uint16_t BASELINE_SEED_SIZE = 3;
constexpr float BASELINE_TOLERANCE = 0.20f;
static_assert(BASELINE_SEED_SIZE <= BASELINE_SIZE, "Baseline seed capacity");
static_assert(RR_SIZE > MEASUREMENT_DURATION_MS / MIN_RR_MS, "RR capacity");
constexpr uint32_t MAX_SERVICE_GAP_MS = 250;
// Optional raw FIFO-frame CSV before/after measurement too. Extra lines use
// #PPG,sample_ms,IR. Capture these to validate the detector offline.
constexpr bool EXPORT_RAW_PPG = false;
MAX30105 sensor;

enum class SessionState : uint8_t {
  WAITING_FINGER, STABILIZING, CALIBRATING, MEASURING, COMPLETE
};
SessionState sessionState = SessionState::WAITING_FINGER;
enum class RunMode : uint8_t { TEST, PROFILE, MONITOR };
RunMode runMode = RunMode::TEST;
bool hostControlled = false, sessionArmed = true, sessionAllocated = false;
bool protocolEnabled = false, streamEnabled = false;
uint32_t bootId = 0, sessionId = 0, recordSequence = 0, totalTxDrops = 0;
uint32_t rrStoredWall[RR_SIZE] = {}, lastBeatWall = 0;
uint8_t startupAttempts = 0;
uint32_t lastStartupAttempt = 0;
const char *startupFault = "NOT_STARTED";
bool baselineReady = false;
// Seeds are provisional until three candidates agree. Only then do they
// enter baselineRR; an early outlier cannot lock the median to itself.
uint32_t baselineSeeds[BASELINE_SEED_SIZE] = {};
uint16_t baselineSeedCount = 0;
bool dataTimeoutPending = false;
uint32_t measurementStart = 0, fingerDetectedAt = 0, lastBeat = 0;
float rrReference = 0; // Frozen baseline; written only at commit/session reset.
float rrTrackingReference = 0; // Existing adaptive measurement rejection gate.
uint16_t baselineCount = 0, rrCount = 0;
uint16_t rejectedShort = 0, rejectedLong = 0;
uint32_t baselineRR[BASELINE_SIZE] = {}, rrIntervals[RR_SIZE] = {};
bool adjacentRR[RR_SIZE] = {};
bool haveBeat = false, previousRRValid = false;
bool lowSignalPending = false, sensorReady = false;
uint32_t lowSignalStart = 0, calibrationStart = 0, lastCalibrationNotice = 0;
uint32_t lastSampleWall = 0, lastPollWall = 0, sampleTime = 0;
uint32_t acquisitionStartedAt = 0;
bool sampleClockReady = false;
uint32_t lastIR = 0, lastAcceptedWall = 0, maxValidGap = 0;
uint32_t acquisitionErrors = 0, fifoLossEvents = 0, droppedLines = 0;
uint32_t acceptedSamples = 0, badSamples = 0, lastCSV = 0;
bool bufferFull = false;
float instantBPM = NAN;
// Transport diagnostics persist across session resets, unlike HRV statistics.
uint32_t totalFrames = 0, totalI2CErrors = 0, totalFIFOErrors = 0;
uint32_t lastDiagnosticAt = 0, diagnosticFrames = 0, lastRed = 0;
uint8_t latestWritePointer = 0, latestReadPointer = 0, latestOverflow = 0;

// Nonblocking serial: acquisition never waits for a host/Serial monitor.
constexpr size_t TX_SIZE = 8192;
char txBuffer[TX_SIZE];
size_t txHead = 0, txTail = 0;
void logLine(const char *format, ...) {
  char line[256];
  va_list args;
  va_start(args, format);
  int n = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  size_t freeBytes = (txTail + TX_SIZE - txHead - 1) % TX_SIZE;
  if (n < 0 || n >= (int)sizeof(line) || (size_t)n > freeBytes) {
    ++totalTxDrops;
    if (sessionState == SessionState::MEASURING) ++droppedLines;
    return;
  }
  for (int i = 0; i < n; ++i) {
    txBuffer[txHead] = line[i];
    txHead = (txHead + 1) % TX_SIZE;
  }
}
void serviceSerial() {
  int room = Serial.availableForWrite();
  if (room <= 0 || txHead == txTail) return;
  size_t count = txHead > txTail ? txHead - txTail : TX_SIZE - txTail;
  if (count > (size_t)room) count = room;
  if (count > 64) count = 64;
  txTail = (txTail + Serial.write((const uint8_t *)txBuffer + txTail, count)) % TX_SIZE;
}

// Protocol v1. Sequence advances even when the bounded TX queue drops a record.
// All numeric clocks/counters on the wire are unsigned 32-bit modulo counters.
void emitRecord(const char *kind, uint32_t wall, const char *format, ...) {
  if (!protocolEnabled) return;
  char payload[176];
  va_list args; va_start(args, format);
  int n = vsnprintf(payload, sizeof(payload), format, args); va_end(args);
  uint32_t seq = ++recordSequence;
  if (n < 0 || n >= (int)sizeof(payload)) { ++totalTxDrops; ++droppedLines; return; }
  logLine("@%s,1,%08lX,%lu,%lu,%lu,%s\n", kind, (unsigned long)bootId,
          (unsigned long)sessionId, (unsigned long)seq, (unsigned long)wall, payload);
}

bool firebaseConfigured() {
  return strcmp(WIFI_SSID, "YOUR_WIFI_NAME") != 0 &&
         strstr(FIREBASE_DATABASE_URL, "YOUR_PROJECT") == nullptr &&
         strncmp(FIREBASE_DATABASE_URL, "https://", 8) == 0;
}

bool connectWiFi(uint32_t timeoutMs = 15000) {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (!firebaseConfigured()) {
    logLine("#WIFI configuration missing; fill WIFI_SSID and FIREBASE_DATABASE_URL.\n");
    return false;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  logLine("#WIFI connecting");
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && (uint32_t)(millis() - started) < timeoutMs) {
    delay(250);
    logLine(".");
    serviceSerial();
  }
  if (WiFi.status() != WL_CONNECTED) {
    logLine(" FAILED\n");
    return false;
  }
  logLine(" CONNECTED IP=%s\n", WiFi.localIP().toString().c_str());
  return true;
}
const char *modeName() {
  return runMode == RunMode::TEST ? "TEST" : runMode == RunMode::PROFILE ? "PROFILE" : "MONITOR";
}
void allocateSession(uint32_t wall) {
  ++sessionId; sessionAllocated = true;
  emitRecord("SESSION", wall, "%s", modeName());
}
void pruneContinuousRR(uint32_t wall) {
  if (runMode == RunMode::TEST) return;
  uint16_t remove = 0;
  while (remove < rrCount && (uint32_t)(wall - rrStoredWall[remove]) >= MEASUREMENT_DURATION_MS) ++remove;
  if (!remove) return;
  rrCount -= remove;
  memmove(rrIntervals, rrIntervals + remove, rrCount * sizeof(rrIntervals[0]));
  memmove(rrStoredWall, rrStoredWall + remove, rrCount * sizeof(rrStoredWall[0]));
  memmove(adjacentRR, adjacentRR + remove, rrCount * sizeof(adjacentRR[0]));
  if (rrCount) adjacentRR[0] = false;
}

// Beat detection: DC removal (~0.65 Hz), low-pass (~4.6 Hz), adaptive
// amplitude envelope, negative-lobe arming, positive local maximum of polarity-corrected AC.
// Float arithmetic preserves the full 18-bit IR range. A threshold is a
// noise gate, not evidence that the pulse is sinus or free of motion.
struct PulseDetector {
  bool ready = false, armed = false;
  float dc = 0, smooth = 0, previous = 0, slope = 0, envelope = 0;
  uint32_t warmedAt = 0;
  void reset() { ready = false; armed = false; }
  bool update(uint32_t ir, uint32_t t, uint32_t &peakAt) {
    if (!ready) {
      ready = true; armed = false; dc = (float)ir;
      smooth = previous = slope = envelope = 0; warmedAt = t;
      return false;
    }
    // Track baseline drift so a real trough is not hidden by a DC offset.
    dc += 0.04f * ((float)ir - dc);
    smooth += 0.25f * (PPG_POLARITY * ((float)ir - dc) - smooth);
    envelope += 0.02f * (fabsf(smooth) - envelope);
    const float gate = fmaxf(10.0f, envelope * 0.45f);
    float d = smooth - previous;
    bool beat = false;
    if ((uint32_t)(t - warmedAt) >= 1000) {
      if (smooth < -gate) armed = true;
      if (armed && slope > 0 && d <= 0 && previous > gate) {
        peakAt = t - SAMPLE_MS;
        beat = true; armed = false;
      }
    }
    previous = smooth; slope = d;
    return beat;
  }
} detector;

// HRV calculation: sample SDNN; RMSSD and pNN50 use only actually adjacent
// accepted intervals. Undefined metrics are nan, never a fabricated zero.
struct Statistics {
  double mean = NAN, bpm = NAN, rmssd = NAN, sdnn = NAN, pnn50 = NAN;
  uint16_t pairs = 0;
};
Statistics statistics(); // Explicit prototype for Arduino sketch preprocessing.
bool uploadMeasurementToFirebase(const Statistics &s, double artifacts,
                                 double badPercent, const char *quality,
                                 const char *level);
Statistics statistics() {
  Statistics s;
  if (!rrCount) return s;
  double sum = 0, variance = 0, diffSq = 0;
  uint16_t nn50 = 0;
  for (uint16_t i = 0; i < rrCount; ++i) sum += rrIntervals[i];
  s.mean = sum / rrCount; s.bpm = 60000.0 / s.mean;
  for (uint16_t i = 0; i < rrCount; ++i) {
    double delta = rrIntervals[i] - s.mean;
    variance += delta * delta;
    if (i && adjacentRR[i]) {
      double d = (double)rrIntervals[i] - rrIntervals[i - 1];
      diffSq += d * d; ++s.pairs;
      if (fabs(d) > 50.0) ++nn50;
    }
  }
  if (rrCount > 1) s.sdnn = sqrt(variance / (rrCount - 1));
  if (s.pairs) {
    s.rmssd = sqrt(diffSq / s.pairs);
    s.pnn50 = 100.0 * nn50 / s.pairs;
  }
  return s;
}

bool uploadMeasurementToFirebase(const Statistics &s, double artifacts,
                                 double badPercent, const char *quality,
                                 const char *level) {
  if (!connectWiFi()) {
    logLine("#FIREBASE upload skipped: Wi-Fi unavailable.\n");
    return false;
  }

  String url = String(FIREBASE_DATABASE_URL) + "/hrv_measurements.json";
  if (FIREBASE_AUTH_TOKEN[0] != '\0') {
    url += "?auth=";
    url += FIREBASE_AUTH_TOKEN;
  }

  String json;
  json.reserve(768);
  json = "{";
  json += "\"boot_id\":\"" + String(bootId, HEX) + "\",";
  json += "\"session_id\":" + String(sessionId) + ",";
  json += "\"device_uptime_ms\":" + String(millis()) + ",";
  json += "\"duration_s\":60,";
  json += "\"valid_rr\":" + String(rrCount) + ",";
  json += "\"rejected_short\":" + String(rejectedShort) + ",";
  json += "\"rejected_long\":" + String(rejectedLong) + ",";
  json += "\"artifact_percent\":" + String(artifacts, 2) + ",";
  json += "\"mean_rr_ms\":" + String(s.mean, 2) + ",";
  json += "\"average_bpm\":" + String(s.bpm, 2) + ",";
  json += "\"rmssd_ms\":" + String(s.rmssd, 2) + ",";
  json += "\"sdnn_ms\":" + String(s.sdnn, 2) + ",";
  json += "\"pnn50_percent\":" + String(s.pnn50, 2) + ",";
  json += "\"signal_quality\":\"" + String(quality) + "\",";
  json += "\"hrv_level\":\"" + String(level) + "\",";
  json += "\"adjacent_rr_pairs\":" + String(s.pairs) + ",";
  json += "\"samples\":" + String(acceptedSamples) + ",";
  json += "\"bad_signal_percent\":" + String(badPercent, 2) + ",";
  json += "\"fifo_loss_events\":" + String(fifoLossEvents) + ",";
  json += "\"acquisition_errors\":" + String(acquisitionErrors) + ",";
  json += "\"dropped_csv_lines\":" + String(droppedLines) + ",";
  json += "\"max_valid_rr_gap_ms\":" + String(maxValidGap) + ",";
  json += "\"rr_buffer_full\":" + String(bufferFull ? "true" : "false");
  json += "}";

  WiFiClientSecure secureClient;
  // Easy first-run setup. For deployment, replace this with Firebase's CA cert.
  secureClient.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(secureClient, url)) {
    logLine("#FIREBASE HTTP setup failed.\n");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  const int status = http.POST(json);
  const String response = status > 0 ? http.getString() : String();
  http.end();

  if (status >= 200 && status < 300) {
    logLine("#FIREBASE upload OK HTTP=%d response=%.120s\n", status, response.c_str());
    return true;
  }
  logLine("#FIREBASE upload FAILED HTTP=%d response=%.120s\n", status, response.c_str());
  return false;
}
// Count-aware median: unused zero-filled slots never enter the estimate.
float medianRR(const uint32_t *values, uint16_t count) {
  if (!count || count > BASELINE_SIZE) return NAN;
  uint32_t temp[BASELINE_SIZE];
  memcpy(temp, values, count * sizeof(temp[0]));
  for (uint16_t i = 1; i < count; ++i) {
    uint32_t v = temp[i]; int j = i - 1;
    while (j >= 0 && temp[j] > v) { temp[j + 1] = temp[j]; --j; }
    temp[j + 1] = v;
  }
  return count % 2 ? (float)temp[count / 2] :
         0.5f * ((float)temp[count / 2 - 1] + (float)temp[count / 2]);
}
bool baselineInlier(uint32_t rr, float median) {
  return rr >= MIN_RR_MS && rr <= MAX_RR_MS &&
         fabsf((float)rr - median) <= median * BASELINE_TOLERANCE;
}
const char *stateName() {
  switch (sessionState) {
    case SessionState::WAITING_FINGER: return "WAITING_FINGER";
    case SessionState::STABILIZING: return "STABILIZING";
    case SessionState::CALIBRATING: return "CALIBRATING";
    case SessionState::MEASURING: return "MEASURING";
    case SessionState::COMPLETE: return "COMPLETE";
  }
  return "UNKNOWN";
}
void breakContinuity() {
  if (haveBeat || previousRRValid) emitRecord("GAP", millis(), "SIGNAL_OR_ACQUISITION");
  haveBeat = false; lastBeat = 0; previousRRValid = false;
  instantBPM = NAN; detector.reset();
}
void clearSession() {
  sessionAllocated = false;
  sessionState = SessionState::WAITING_FINGER;
  baselineReady = false; baselineSeedCount = 0; dataTimeoutPending = false;
  measurementStart = fingerDetectedAt = 0;
  lowSignalStart = calibrationStart = lastCalibrationNotice = 0;
  lowSignalPending = false;
  rrReference = rrTrackingReference = 0;
  baselineCount = rrCount = rejectedShort = rejectedLong = 0;
  acquisitionErrors = fifoLossEvents = droppedLines = 0;
  acceptedSamples = badSamples = maxValidGap = 0;
  lastAcceptedWall = lastCSV = 0; bufferFull = false;
  memset(baselineSeeds, 0, sizeof(baselineSeeds));
  memset(baselineRR, 0, sizeof(baselineRR));
  memset(rrIntervals, 0, sizeof(rrIntervals));
  memset(adjacentRR, 0, sizeof(adjacentRR));
  breakContinuity();
}
void cancelSession(const char *reason) {
  emitRecord("END", millis(), "CANCELLED,%s", reason);
  if (hostControlled) sessionArmed = false;
  logLine("Measurement cancelled: %s.\n", reason);
  clearSession();
  logLine("Place finger on sensor and keep still.\n");
}
void startMeasurement(uint32_t wall) {
  if (sessionState != SessionState::CALIBRATING || !baselineReady ||
      baselineCount != BASELINE_SIZE || !isfinite(rrReference) ||
      rrReference < MIN_RR_MS || rrReference > MAX_RR_MS) return;
  // Separate 60 s wall-clock processing window. RR differences use only
  // sample-clock peaks. Reset the anchor: two subsequent beats are required.
  rrCount = rejectedShort = rejectedLong = 0;
  memset(rrIntervals, 0, sizeof(rrIntervals));
  memset(adjacentRR, 0, sizeof(adjacentRR));
  acquisitionErrors = fifoLossEvents = droppedLines = 0;
  acceptedSamples = badSamples = maxValidGap = 0; bufferFull = false;
  haveBeat = previousRRValid = false; lastBeat = 0; instantBPM = NAN;
  rrTrackingReference = rrReference;
  measurementStart = lastAcceptedWall = lastCSV = wall;
  sessionState = SessionState::MEASURING;
  emitRecord("START", wall, "%s,%.2f", modeName(), rrReference);
  if (runMode == RunMode::TEST) logLine("60-second HRV measurement started.\nDo not move your finger.\n");
  else logLine("Continuous recording started.\n");
  logLine("time_s,IR,BPM,Avg_BPM,RR_ms,RMSSD_ms,Status\n");
}
void collectBaselineRR(uint32_t rr, uint32_t wall) {
  if (sessionState != SessionState::CALIBRATING || baselineReady) return;
  if (rr < MIN_RR_MS || rr > MAX_RR_MS) {
    logLine("#CAL RR=%lu %s\n", (unsigned long)rr,
            rr < MIN_RR_MS ? "REJECT_SHORT" : "REJECT_LONG");
    return;
  }
  if (baselineCount == 0) {
    if (baselineSeedCount >= BASELINE_SEED_SIZE) return;
    baselineSeeds[baselineSeedCount++] = rr;
    logLine("#CAL RR=%lu CANDIDATE seed=%u/%u\n", (unsigned long)rr,
            baselineSeedCount, BASELINE_SEED_SIZE);
    if (baselineSeedCount < BASELINE_SEED_SIZE) return;
    float med = medianRR(baselineSeeds, baselineSeedCount);
    uint16_t kept = 0;
    for (uint16_t i = 0; i < baselineSeedCount; ++i) {
      if (baselineInlier(baselineSeeds[i], med)) {
        baselineSeeds[kept++] = baselineSeeds[i];
      } else {
        logLine("#CAL RR=%lu REJECT_OUTLIER seed median=%.2f\n",
                (unsigned long)baselineSeeds[i], med);
      }
    }
    baselineSeedCount = kept;
    if (kept < BASELINE_SEED_SIZE) return;
    for (uint16_t i = 0; i < kept; ++i) {
      baselineRR[baselineCount++] = baselineSeeds[i];
      logLine("#CAL RR=%lu ACCEPTED count=%u/%u\n",
              (unsigned long)baselineSeeds[i], baselineCount, BASELINE_SIZE);
    }
    baselineSeedCount = 0;
  } else {
    if (baselineCount >= BASELINE_SIZE) return;
    const float currentMedian = medianRR(baselineRR, baselineCount);
    if (!baselineInlier(rr, currentMedian)) {
      logLine("#CAL RR=%lu REJECT_OUTLIER median=%.2f\n",
              (unsigned long)rr, currentMedian);
      return;
    }
    // Validate the whole prospective set BEFORE increasing the valid count.
    // This also prevents a median shift from making an older RR an outlier.
    uint32_t candidate[BASELINE_SIZE];
    memcpy(candidate, baselineRR, baselineCount * sizeof(candidate[0]));
    candidate[baselineCount] = rr;
    const float nextMedian = medianRR(candidate, baselineCount + 1);
    for (uint16_t i = 0; i <= baselineCount; ++i) {
      if (!baselineInlier(candidate[i], nextMedian)) {
        logLine("#CAL RR=%lu REJECT_OUTLIER set_median=%.2f\n",
                (unsigned long)rr, nextMedian);
        return;
      }
    }
    baselineRR[baselineCount++] = rr;
    logLine("#CAL RR=%lu ACCEPTED count=%u/%u\n",
            (unsigned long)rr, baselineCount, BASELINE_SIZE);
  }
  if (baselineCount == BASELINE_SIZE) {
    rrReference = medianRR(baselineRR, baselineCount);
    baselineReady = true;
    logLine("\n=== BASELINE READY ===\nBaseline_RR_ms: %.2f\nBaseline_BPM: %.2f\nValid_baseline_RR: %u\n\n",
            rrReference, 60000.0 / rrReference, baselineCount);
    startMeasurement(wall); // Single call site, reached only on baseline commit.
  }
}
void csv(uint32_t wall, uint32_t ir, uint32_t rr, const char *status) {
  Statistics s = statistics();
  logLine("%.2f,%lu,%.2f,%.2f,%lu,%.2f,%s\n",
          (wall - measurementStart) / 1000.0, (unsigned long)ir,
          instantBPM, s.bpm, (unsigned long)rr, s.rmssd, status);
  lastCSV = wall;
}

// Artifact rejection: early peaks do NOT move the beat anchor. Long
// intervals move the anchor but are not stored, divided or interpolated.
void processBeat(uint32_t peak, uint32_t wall, uint32_t ir) {
  if (sessionState != SessionState::CALIBRATING &&
      sessionState != SessionState::MEASURING) return;
  // Never compare sample-clock peak with wall-clock measurementStart.
  if (sessionState == SessionState::MEASURING && runMode == RunMode::TEST &&
      (uint32_t)(wall - measurementStart) >= MEASUREMENT_DURATION_MS) return;
  if (!haveBeat) { lastBeat = peak; lastBeatWall = wall; haveBeat = true; return; }
  const uint32_t rr = peak - lastBeat;
  const uint32_t beginWall = lastBeatWall;
  if (sessionState == SessionState::CALIBRATING) {
    if (rr < MIN_RR_MS) { collectBaselineRR(rr, wall); return; }
    if (baselineCount && rr < medianRR(baselineRR, baselineCount) *
                                  (1.0f - BASELINE_TOLERANCE)) {
      logLine("#CAL RR=%lu REJECT_OUTLIER early_peak\n", (unsigned long)rr);
      return; // Preserve the anchor for an early extra peak.
    }
    lastBeat = peak; lastBeatWall = wall;
    collectBaselineRR(rr, wall);
    return;
  }
  const float low = fmaxf(MIN_RR_MS, rrTrackingReference * 0.75f);
  if (rr < low) {
    ++rejectedShort; previousRRValid = false; instantBPM = NAN;
    emitRecord("RR", wall, "%lu,%lu,%lu,0,0,SHORT", (unsigned long)peak, (unsigned long)beginWall, (unsigned long)rr);
    csv(wall, ir, rr, "REJECT_SHORT");
    return;
  }
  lastBeat = peak; lastBeatWall = wall;
  if (rr > MAX_RR_MS || rr > rrTrackingReference * 1.25f) {
    ++rejectedLong; previousRRValid = false; instantBPM = NAN;
    emitRecord("RR", wall, "%lu,%lu,%lu,0,0,LONG", (unsigned long)peak, (unsigned long)beginWall, (unsigned long)rr);
    csv(wall, ir, rr, "REJECT_LONG"); return;
  }
  // Continuous sessions retain at most the last minute in the legacy summary.
  pruneContinuousRR(wall);
  emitRecord("RR", wall, "%lu,%lu,%lu,1,%u,OK", (unsigned long)peak,
             (unsigned long)beginWall, (unsigned long)rr, previousRRValid ? 1 : 0);
  // RR storage, bounded static arrays. Capacity failure invalidates quality.
  if (rrCount >= RR_SIZE) { bufferFull = true; previousRRValid = false; return; }
  rrStoredWall[rrCount] = wall;
  rrIntervals[rrCount] = rr;
  adjacentRR[rrCount] = previousRRValid && rrCount > 0;
  ++rrCount; previousRRValid = true;
  instantBPM = 60000.0f / rr;
  rrTrackingReference += 0.05f * ((float)rr - rrTrackingReference);
  const uint32_t gap = wall - lastAcceptedWall;
  if (gap > maxValidGap) maxValidGap = gap;
  lastAcceptedWall = wall;
  csv(wall, ir, rr, "MEASURING");
}

// Quality control and final report. Artifact_percent counts rejected
// CANDIDATE intervals, not missing beats. Separate time coverage and
// acquisition checks prevent silent detector failure from looking GOOD.
void finishMeasurement() {
  if (sessionState != SessionState::MEASURING) return;
  Statistics s = statistics();
  uint32_t total = (uint32_t)rrCount + rejectedShort + rejectedLong;
  double artifacts = total ? 100.0 * ((uint32_t)rejectedShort + rejectedLong) / total : 100.0;
  const uint32_t acceptedElapsed = lastAcceptedWall - measurementStart;
  const uint32_t tailGap = acceptedElapsed >= MEASUREMENT_DURATION_MS ? 0 :
                           MEASUREMENT_DURATION_MS - acceptedElapsed;
  if (tailGap > maxValidGap) maxValidGap = tailGap;
  double badPercent = acceptedSamples ? 100.0 * badSamples / acceptedSamples : 100.0;
  bool poor = rrCount < 50 || s.pairs < 30 || artifacts > 10.0 ||
              maxValidGap > 2500 || badPercent > 5.0 || acceptedSamples < 5700 ||
              acquisitionErrors || fifoLossEvents || droppedLines || bufferFull;
  const char *quality = poor ? "POOR" : artifacts <= 5.0 ? "GOOD" : "ACCEPTABLE";
  const char *level = poor || !isfinite(s.rmssd) ? "INVALID_RECORDING" :
                     s.rmssd < 25 ? "LOW_HRV" : s.rmssd <= 40 ? "MODERATE_HRV" : "HIGH_HRV";
  sessionState = SessionState::COMPLETE;
  if (hostControlled) sessionArmed = false;
  emitRecord("END", measurementStart + MEASUREMENT_DURATION_MS, "COMPLETE,60_SECONDS");
  logLine("\n========================================\n=== MEASUREMENT COMPLETE ===\n========================================\n");
  logLine("Duration_s: 60\nValid_RR: %u\nRejected_short: %u\nRejected_long: %u\n", rrCount, rejectedShort, rejectedLong);
  logLine("Artifact_percent: %.2f\nMean_RR_ms: %.2f\nAverage_BPM: %.2f\n", artifacts, s.mean, s.bpm);
  logLine("RMSSD_ms: %.2f\nSDNN_ms: %.2f\npNN50_percent: %.2f\n", s.rmssd, s.sdnn, s.pnn50);
  logLine("Signal_Quality: %s\nHRV_Level: %s\n", quality, level);
  logLine("Adjacent_RR_pairs: %u\nSamples: %lu\nBad_signal_percent: %.2f\n", s.pairs, (unsigned long)acceptedSamples, badPercent);
  logLine("FIFO_loss_events: %lu\nAcquisition_errors: %lu\nDropped_CSV_lines: %lu\n",
          (unsigned long)fifoLossEvents, (unsigned long)acquisitionErrors, (unsigned long)droppedLines);
  logLine("Max_valid_RR_gap_ms: %lu\nRR_buffer_full: %s\n", (unsigned long)maxValidGap, bufferFull ? "YES" : "NO");
  logLine("========================================\nRemove finger to start a new measurement.\n");
  uploadMeasurementToFirebase(s, artifacts, badPercent, quality, level);
}

// Wall-clock state deadlines are checked even with low IR or empty FIFO.
void serviceState(uint32_t now) {
  // Require recent samples confirming low IR; missing data is not evidence
  // of finger removal. The >= boundary implements the 2500 ms debounce.
  if (sessionState != SessionState::WAITING_FINGER && lowSignalPending &&
      (uint32_t)(now - lowSignalStart) >= FINGER_LOSS_MS &&
      (uint32_t)(now - lastSampleWall) < MAX_SERVICE_GAP_MS) {
    if (sessionState == SessionState::COMPLETE) {
      emitRecord("READY", now, "REMOVE_COMPLETE"); clearSession();
      logLine("Ready for the next session.\n");
    } else cancelSession("finger removed for 2500 ms");
    return;
  }
  if (sessionState != SessionState::WAITING_FINGER &&
      sessionState != SessionState::COMPLETE &&
      (uint32_t)(now - lastSampleWall) >= FINGER_LOSS_MS) {
    if (sessionState == SessionState::MEASURING) {
      cancelSession("sensor data timeout"); return;
    }
    if (!dataTimeoutPending) {
      dataTimeoutPending = true; lowSignalPending = false;
      breakContinuity();
      logLine("#STATE sensor data timeout; baseline preserved=%u/%u\n",
              baselineCount, BASELINE_SIZE);
    }
    return;
  }
  if (sessionState == SessionState::MEASURING && runMode == RunMode::TEST &&
      (uint32_t)(now - measurementStart) >= MEASUREMENT_DURATION_MS) {
    finishMeasurement(); return;
  }
  if (sessionState == SessionState::STABILIZING && !lowSignalPending &&
      !dataTimeoutPending && lastIR < 260000 &&
      (uint32_t)(now - fingerDetectedAt) >= STABILIZATION_MS) {
    sessionState = SessionState::CALIBRATING;
    haveBeat = previousRRValid = false; lastBeat = 0;
    calibrationStart = lastCalibrationNotice = now;
    logLine("Signal stabilized.\nCalibration started: waiting for 9 consistent RR intervals.\n");
  }
  if (sessionState == SessionState::CALIBRATING &&
      (uint32_t)(now - lastCalibrationNotice) >= 30000) {
    lastCalibrationNotice = now;
    logLine("#CAL WAIT elapsed_s=%lu valid=%u/%u seed=%u/%u; keep still, baseline preserved.\n",
            (unsigned long)((now - calibrationStart) / 1000),
            baselineCount, BASELINE_SIZE, baselineSeedCount, BASELINE_SEED_SIZE);
  }
  if (sessionState == SessionState::MEASURING && (uint32_t)(now - lastCSV) >= 1000) {
    if ((uint32_t)(now - lastAcceptedWall) > (uint32_t)(rrTrackingReference * 1.5f)) instantBPM = NAN;
    csv(now, lastIR, 0, lowSignalPending ? "LOW_SIGNAL" : "WAITING_BEAT");
  }
}
void processSample(uint32_t ir, uint32_t t, uint32_t wall) {
  // acquire() also services deadlines before each frame. Keep this guard so
  // direct callers cannot insert samples at/after the measurement deadline.
  if (sessionState == SessionState::MEASURING && runMode == RunMode::TEST &&
      (uint32_t)(wall - measurementStart) >= MEASUREMENT_DURATION_MS) {
    serviceState(wall);
  }
  lastIR = ir; lastSampleWall = wall;
  if (streamEnabled) emitRecord("PPG", wall, "%lu,%lu,%lu,%u",
      (unsigned long)t, (unsigned long)ir, (unsigned long)lastRed,
      ir < FINGER_THRESHOLD ? 1 : ir >= 260000 ? 2 : 0);
  if (dataTimeoutPending && sessionState == SessionState::STABILIZING) fingerDetectedAt = wall;
  dataTimeoutPending = false;
  if (EXPORT_RAW_PPG) logLine("#PPG,%lu,%lu\n", (unsigned long)t, (unsigned long)ir);
  if (sessionState == SessionState::MEASURING) ++acceptedSamples;
  // Finger detection is separate from pulse amplitude/motion quality.
  if (ir < FINGER_THRESHOLD) {
    if (sessionState == SessionState::MEASURING) ++badSamples;
    if (!lowSignalPending) {
      lowSignalPending = true; lowSignalStart = wall;
      breakContinuity();
      if (sessionState == SessionState::CALIBRATING)
        logLine("#CAL LOW_SIGNAL baseline preserved=%u/%u\n", baselineCount, BASELINE_SIZE);
    }
    return;
  }
  if (lowSignalPending && sessionState == SessionState::STABILIZING) fingerDetectedAt = wall;
  lowSignalPending = false;
  if (sessionState == SessionState::WAITING_FINGER) {
    if (!sessionArmed) return;
    bool allocated = sessionAllocated;
    clearSession(); sessionAllocated = allocated;
    if (!sessionAllocated) allocateSession(wall);
    sessionState = SessionState::STABILIZING; fingerDetectedAt = wall;
    logLine("Finger detected.\nStabilizing signal for 5 seconds...\n");
  }
  if (sessionState == SessionState::COMPLETE) return;
  if (ir >= 260000) {
    if (sessionState == SessionState::MEASURING) ++badSamples;
    if (sessionState == SessionState::STABILIZING) fingerDetectedAt = wall;
    if (sessionState == SessionState::CALIBRATING && detector.ready)
      logLine("#CAL SATURATED baseline preserved=%u/%u\n", baselineCount, BASELINE_SIZE);
    breakContinuity(); return;
  }
  uint32_t peak = 0;
  if (detector.update(ir, t, peak)) processBeat(peak, wall, ir);
}

// Acquisition: pointer snapshot at 0x04 = WR_PTR, OVF_COUNTER, RD_PTR.
// Read <=5 complete frames per I2C burst (30 bytes), portable to 32-byte Wire.
bool readBytes(uint8_t reg, uint8_t *data, uint8_t count) {
  Wire.beginTransmission(SENSOR_ADDRESS); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  size_t got = Wire.requestFrom(SENSOR_ADDRESS, count);
  if (got != count) { while (Wire.available()) Wire.read(); return false; }
  for (uint8_t i = 0; i < count; ++i) data[i] = Wire.read();
  return true;
}
bool writeByte(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(SENSOR_ADDRESS);
  Wire.write(reg); Wire.write(value);
  return Wire.endTransmission() == 0;
}
// Stop conversions before changing pointers. Pop one whole RED+IR frame:
// the MAX30102 clears its overflow counter when FIFO data is popped.
// Discard this frame, then reset and verify pointers before restarting.
bool restartFIFO() {
  uint8_t mode = 0, discarded[6], pointers[3];
  if (!readBytes(0x09, &mode, 1)) return false;
  if (!writeByte(0x09, (mode & 0x07) | 0x80)) return false;
  delay(3); // Allow the in-flight conversion to finish (400 conversions/s).
  if (!readBytes(0x07, discarded, 6)) return false;
  if (!writeByte(0x04, 0) || !writeByte(0x05, 0) || !writeByte(0x06, 0)) return false;
  if (!readBytes(0x04, pointers, 3)) return false;
  if ((pointers[0] | pointers[1] | pointers[2]) & 0x1F) return false;
  if (!writeByte(0x09, mode & 0x07)) return false;
  sampleClockReady = false;
  acquisitionStartedAt = lastPollWall = millis();
  return true;
}
void acquisitionFault(bool overflow) {
  if (overflow) ++totalFIFOErrors; else ++totalI2CErrors;
  if (sessionState == SessionState::MEASURING) {
    if (overflow) ++fifoLossEvents; else ++acquisitionErrors;
  }
  breakContinuity(); sampleClockReady = false;
  lowSignalPending = false; // Transport gaps cannot prove continuous finger loss.
  if (sessionState == SessionState::STABILIZING) fingerDetectedAt = millis();
  if (!restartFIFO()) {
    ++totalI2CErrors;
    if (sessionState == SessionState::MEASURING) cancelSession("FIFO recovery failed");
    sensorReady = false;
    logLine("#ERROR FIFO recovery failed; check sensor and restart.\n");
  }
}
void acquire(uint32_t now) {
  if (now - lastPollWall < 2) return;
  uint32_t elapsed = now - lastPollWall; lastPollWall = now;
  if (elapsed >= MAX_SERVICE_GAP_MS) { acquisitionFault(true); return; }
  uint8_t pointers[3];
  if (!readBytes(0x04, pointers, 3)) { acquisitionFault(false); return; }
  latestWritePointer = pointers[0] & 0x1F;
  latestOverflow = pointers[1] & 0x1F;
  latestReadPointer = pointers[2] & 0x1F;
  if (pointers[1] & 0x1F) { acquisitionFault(true); return; }
  uint8_t count = (pointers[0] - pointers[2]) & 0x1F;
  if (!count) {
    if (now - (sampleClockReady ? lastSampleWall : acquisitionStartedAt) >= MAX_SERVICE_GAP_MS) acquisitionFault(false);
    return;
  }
  if (!sampleClockReady) {
    sampleTime = now - (uint32_t)count * SAMPLE_MS;
    sampleClockReady = true;
  }
  // RR uses nominal sample spacing. Do not mistake accumulated oscillator
  // drift relative to millis() for dropped FIFO frames. Service-gap, overflow
  // and no-data checks handle actual transport discontinuities.
  while (count) {
    uint8_t frames = count > 5 ? 5 : count;
    uint8_t bytes[30];
    if (!readBytes(0x07, bytes, frames * 6)) { acquisitionFault(false); return; }
    for (uint8_t i = 0; i < frames; ++i) {
      uint32_t wall = millis();
      serviceState(wall);
      sampleTime += SAMPLE_MS;
      const uint8_t *p = bytes + i * 6 + 3;
      uint32_t ir = (((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]) & 0x3FFFF;
      const uint8_t *r = bytes + i * 6;
      lastRed = (((uint32_t)r[0] << 16) | ((uint32_t)r[1] << 8) | r[2]) & 0x3FFFF;
      ++totalFrames;
      processSample(ir, sampleTime, wall);
    }
    count -= frames;
  }
}
// One status line per second outside the measurement CSV window. This makes
// missing frames distinguishable from low IR and failed pulse calibration.
void protocolStatus(uint32_t now);
void serviceDiagnostics(uint32_t now) {
  if (now - lastDiagnosticAt < 1000) return;
  uint32_t elapsed = now - lastDiagnosticAt;
  uint32_t frames = totalFrames - diagnosticFrames;
  lastDiagnosticAt = now; diagnosticFrames = totalFrames;
  protocolStatus(now);
  if (sessionState == SessionState::MEASURING || sessionState == SessionState::COMPLETE) return;
  const char *state = !sensorReady ? "SENSOR_ERROR" : stateName();
  logLine("#STATUS %s IR=%lu RED=%lu samples/s=%.1f I2C_errors=%lu FIFO_errors=%lu WR=%u RD=%u OVF=%u baseline=%u/9\n",
          state, (unsigned long)lastIR, (unsigned long)lastRed,
          1000.0 * frames / elapsed, (unsigned long)totalI2CErrors,
          (unsigned long)totalFIFOErrors, latestWritePointer, latestReadPointer,
          latestOverflow, baselineCount);
}
void tryStartup() {
  if (sensorReady || totalFrames || startupAttempts >= 3) return;
  ++startupAttempts; lastStartupAttempt = millis();
  sensorReady = false;
  if (!Wire.begin(SDA_PIN, SCL_PIN)) startupFault = "WIRE_BEGIN_FAILED";
  else {
    Wire.setTimeOut(20);
    Wire.beginTransmission(SENSOR_ADDRESS);
    if (Wire.endTransmission() != 0) startupFault = "NO_ACK_0x57";
    else {
      uint8_t id = 0;
      if (!readBytes(0xFF, &id, 1)) startupFault = "PART_ID_READ_FAILED";
      else if (id != 0x15) startupFault = "PART_ID_MISMATCH";
      else if (!sensor.begin(Wire, I2C_SPEED_STANDARD)) startupFault = "DRIVER_BEGIN_FAILED";
      else sensorReady = true;
    }
  }
  if (!sensorReady) {
    ++totalI2CErrors;
    logLine("#ERROR startup=%s attempt=%u/3 I2C_errors=%lu\n", startupFault,
            startupAttempts, (unsigned long)totalI2CErrors);
    emitRecord("ERROR", millis(), "%s,%u", startupFault, startupAttempts);
    return;
  }
  // Preserve the experimentally verified optical configuration.
  sensor.setup(0x1F, 4, 2, 400, 411, 8192);
  sensor.setPulseAmplitudeRed(0x1F);
  sensor.setPulseAmplitudeIR(0x2F);
  sensor.setPulseAmplitudeGreen(0);
  // No silent replacement of unread data: overflow invalidates the record.
  sensor.disableFIFORollover();
  if (!restartFIFO()) {
    sensorReady = false; ++totalI2CErrors; startupFault = "FIFO_INIT_FAILED";
    logLine("#ERROR FIFO initialization failed; check sensor and restart.\n");
    return;
  }
  uint8_t config[6];
  if (!readBytes(0x08, config, 6)) {
    sensorReady = false; ++totalI2CErrors; startupFault = "CONFIG_READ_FAILED";
    logLine("#ERROR CONFIG_READ_FAILED\n"); return;
  }
  startupFault = "OK";
  logLine("#CONFIG FIFO=0x%02X MODE=0x%02X SPO2=0x%02X RED=0x%02X IR=0x%02X\n",
          config[0], config[1], config[2], config[4], config[5]);
  lastPollWall = lastSampleWall = millis(); sampleClockReady = false;
  logLine("Sensor ready.\nPlace finger on sensor and keep still.\n");
}
void protocolStatus(uint32_t now) {
  emitRecord("STATUS", now, "%s,%s,%u,%lu,%u,%u,%lu,%lu,%lu,%lu,%lu,%lu,%s",
      stateName(), modeName(), sensorReady ? 1 : 0, (unsigned long)lastIR,
      baselineCount, baselineReady ? 1 : 0,
      (unsigned long)(sessionState == SessionState::STABILIZING ? now - fingerDetectedAt : 0),
      (unsigned long)(sessionState == SessionState::MEASURING ? now - measurementStart : 0),
      (unsigned long)totalFrames, (unsigned long)totalI2CErrors,
      (unsigned long)totalFIFOErrors, (unsigned long)totalTxDrops, startupFault);
}
void handleCommand(char *line) {
  char *save = nullptr;
  char *prefix = strtok_r(line, ",", &save);
  char *version = strtok_r(nullptr, ",", &save);
  char *request = strtok_r(nullptr, ",", &save);
  char *operation = strtok_r(nullptr, ",", &save);
  char *argument = strtok_r(nullptr, ",", &save);
  if (!prefix || strcmp(prefix, "C") || !version || strcmp(version, "1") ||
      !request || strlen(request) > 12 || !operation || strlen(operation) > 12) return;
  for (const char *c = request; *c; ++c) if (*c < '0' || *c > '9') return;
  protocolEnabled = true;
  const char *result = "OK";
  if (!strcmp(operation, "HELLO")) {
    emitRecord("HELLO", millis(), "ppg-monitor-1,100,400,1500,60");
    protocolStatus(millis());
  } else if (!strcmp(operation, "STREAM") && argument && !strcmp(argument, "ON")) streamEnabled = true;
  else if (!strcmp(operation, "START") && argument &&
           (!strcmp(argument, "TEST") || !strcmp(argument, "PROFILE") || !strcmp(argument, "MONITOR"))) {
    if (!sensorReady) result = "SENSOR_ERROR";
    else {
      if (sessionAllocated) emitRecord("END", millis(), "REPLACED,NEW_COMMAND");
      clearSession(); hostControlled = true; sessionArmed = true;
      runMode = !strcmp(argument, "TEST") ? RunMode::TEST :
                !strcmp(argument, "PROFILE") ? RunMode::PROFILE : RunMode::MONITOR;
      allocateSession(millis());
    }
  } else if (!strcmp(operation, "STOP")) {
    emitRecord("END", millis(), "STOPPED,USER");
    clearSession(); hostControlled = true; sessionArmed = false;
  } else result = "BAD_COMMAND";
  emitRecord("ACK", millis(), "%s,%s,%s", request, operation, result);
}
void serviceCommands() {
  static char line[96]; static size_t used = 0; static bool overflow = false;
  for (uint8_t budget = 0; budget < 32 && Serial.available(); ++budget) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (!overflow) { line[used] = 0; handleCommand(line); }
      else emitRecord("ERROR", millis(), "COMMAND_TOO_LONG,0");
      used = 0; overflow = false;
    } else if (used + 1 < sizeof(line) && !overflow) line[used++] = c;
    else overflow = true;
  }
}
void setup() {
  Serial.begin(115200); bootId = esp_random(); clearSession();
  logLine("Starting Jurek Qagysy HRV + Firebase...\n");
  tryStartup(); lastDiagnosticAt = millis();
  connectWiFi();
}
void loop() {
  serviceCommands();
  if (!sensorReady && !totalFrames && startupAttempts < 3 &&
      (uint32_t)(millis() - lastStartupAttempt) >= 1000) tryStartup();
  serviceState(millis());
  if (sensorReady) acquire(millis());
  serviceDiagnostics(millis());
  serviceSerial();
  delay(1);
}


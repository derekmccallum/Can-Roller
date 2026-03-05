/*
  UNO + CNC Shield V3 + (2x) DRV8825
  - START button (A0):
      * if idle: start run (planned duration set by RUNTIME pot)
      * if running: cancel and ramp down smoothly from CURRENT speed, then stop
  - REVERSE button (A1): toggles direction for NEXT run only
  - SPEED pot (A2): continuously read during run with filtering for smooth speed changes
  - RUNTIME pot (A3): read once at start, sets run duration 3-10 seconds
  - Soft start/stop: 0.5s ramp up / 0.5s ramp down
  - DDS/phase-accumulator stepping (smooth, low jitter)
  - Real-time speed adjustment with non-blocking reads

  Buttons wired to GND, using INPUT_PULLUP (pressed = LOW)
*/

#include <Arduino.h>

// CNC Shield V3 (typical)
const uint8_t X_STEP = 2;
const uint8_t X_DIR  = 5;
const uint8_t A_STEP = 12;
const uint8_t A_DIR  = 13;
const uint8_t EN_PIN = 8;

// Inputs
const uint8_t PIN_START   = A0;
const uint8_t PIN_REVERSE = A1;
const uint8_t PIN_POT     = A2;
const uint8_t PIN_RUNTIME = A3;

// Timing / profile
const uint16_t RAMP_MS = 500;
const uint16_t MIN_STEPS_PER_SEC = 200;

// Runtime mapping (milliseconds)
const uint32_t MIN_RUNTIME_MS = 3000;   // 3 seconds
const uint32_t MAX_RUNTIME_MS = 10000;  // 10 seconds

// Pot mapping
const uint16_t POT_MIN_STEPS_PER_SEC = 1000;
const uint16_t POT_MAX_STEPS_PER_SEC = 9500;

// Real-time speed reading - NON-BLOCKING
const uint16_t SPEED_READ_INTERVAL_MS = 100;  // Read pot every 100ms
const uint8_t SPEED_FILTER_SAMPLES = 8;       // Moving average window

// Speed change rate limiting (steps/sec per second)
const uint16_t MAX_SPEED_CHANGE_RATE = 3000; // Max 3000 steps/sec change per second

// DRV8825 pulse width
const uint8_t STEP_PULSE_US = 6;

// Debounce
const uint16_t DEBOUNCE_MS = 30;

// State machine
enum RunState : uint8_t { IDLE, RAMP_UP, CRUISE, RAMP_DOWN };
RunState state = IDLE;

bool nextRunForward = true;

uint16_t targetStepsPerSec = 2000;     // Target speed (filtered from pot)
uint16_t rampDownStartRate = 0;        // Captured instantaneous rate at ramp-down start
uint32_t plannedRunMs = 8000;          // Latched runtime at start

uint32_t runStartMs = 0;               // When run began (for planned duration)
uint32_t phaseStartMs = 0;             // When current phase began
uint32_t lastMicros = 0;
uint32_t lastSpeedReadMs = 0;          // Last time we read the speed pot

uint64_t accumUnits = 0;               // DDS accumulator

// Speed filtering - simple moving average
uint32_t speedAccumulator = 0;         // Running sum for average
uint16_t speedHistory[SPEED_FILTER_SAMPLES];
uint8_t speedHistoryIndex = 0;
bool speedHistoryFilled = false;

// ---- helpers ----
static inline void stepBothOnce() {
  digitalWrite(X_STEP, HIGH);
  digitalWrite(A_STEP, HIGH);
  delayMicroseconds(STEP_PULSE_US);
  digitalWrite(X_STEP, LOW);
  digitalWrite(A_STEP, LOW);
}

static inline uint16_t lerp_u16(uint16_t a, uint16_t b, uint32_t num, uint32_t den) {
  if (den == 0) return b;
  int32_t diff = (int32_t)b - (int32_t)a;
  int32_t val  = (int32_t)a + (diff * (int32_t)num) / (int32_t)den;
  if (val < 1) val = 1;
  return (uint16_t)val;
}

struct DebouncedButton {
  uint8_t pin;
  bool stableState;
  bool lastStableState;
  uint32_t lastChangeMs;

  void begin(uint8_t p) {
    pin = p;
    stableState = digitalRead(pin);
    lastStableState = stableState;
    lastChangeMs = millis();
  }

  bool pressedEvent() {
    bool raw = digitalRead(pin);
    uint32_t now = millis();

    if (raw != stableState) {
      if (now - lastChangeMs >= DEBOUNCE_MS) {
        lastStableState = stableState;
        stableState = raw;
        lastChangeMs = now;
        return (lastStableState == HIGH && stableState == LOW);
      }
    } else {
      lastChangeMs = now;
    }
    return false;
  }
};

DebouncedButton btnStart;
DebouncedButton btnReverse;

// Read speed pot with basic averaging (BLOCKING - only use at start)
uint16_t readSpeedFromPotBlocking() {
  uint32_t sum = 0;
  const uint8_t samples = 10;
  for (uint8_t i = 0; i < samples; i++) {
    sum += analogRead(PIN_POT);
    delayMicroseconds(900);
  }
  uint16_t v = (uint16_t)(sum / samples); // 0..1023

  uint32_t rate = (uint32_t)POT_MIN_STEPS_PER_SEC +
                  ((uint32_t)(POT_MAX_STEPS_PER_SEC - POT_MIN_STEPS_PER_SEC) * v) / 1023UL;

  if (rate < MIN_STEPS_PER_SEC) rate = MIN_STEPS_PER_SEC;
  if (rate > 65000) rate = 65000;
  return (uint16_t)rate;
}

// Read speed pot - SINGLE SAMPLE, non-blocking
uint16_t readSpeedFromPotFast() {
  uint16_t v = analogRead(PIN_POT); // Single read, ~100us

  uint32_t rate = (uint32_t)POT_MIN_STEPS_PER_SEC +
                  ((uint32_t)(POT_MAX_STEPS_PER_SEC - POT_MIN_STEPS_PER_SEC) * v) / 1023UL;

  if (rate < MIN_STEPS_PER_SEC) rate = MIN_STEPS_PER_SEC;
  if (rate > 65000) rate = 65000;
  return (uint16_t)rate;
}

// Read runtime from pot on A3 (BLOCKING - only at start)
uint32_t readRuntimeFromPot() {
  uint32_t sum = 0;
  const uint8_t samples = 10;
  for (uint8_t i = 0; i < samples; i++) {
    sum += analogRead(PIN_RUNTIME);
    delayMicroseconds(900);
  }
  uint16_t v = (uint16_t)(sum / samples); // 0..1023

  uint32_t runtime = MIN_RUNTIME_MS +
                     ((MAX_RUNTIME_MS - MIN_RUNTIME_MS) * (uint32_t)v) / 1023UL;

  return runtime;
}

// Initialize speed filter with a value
void initSpeedFilter(uint16_t initialSpeed) {
  speedAccumulator = (uint32_t)initialSpeed * SPEED_FILTER_SAMPLES;
  for (uint8_t i = 0; i < SPEED_FILTER_SAMPLES; i++) {
    speedHistory[i] = initialSpeed;
  }
  speedHistoryIndex = 0;
  speedHistoryFilled = true;
}

// Add sample to moving average filter - OPTIMIZED
uint16_t filterSpeed(uint16_t newSpeed) {
  // Subtract oldest value from accumulator
  speedAccumulator -= speedHistory[speedHistoryIndex];
  
  // Add new value
  speedHistory[speedHistoryIndex] = newSpeed;
  speedAccumulator += newSpeed;
  
  speedHistoryIndex = (speedHistoryIndex + 1) % SPEED_FILTER_SAMPLES;
  if (speedHistoryIndex == 0) speedHistoryFilled = true;

  // Return average
  return (uint16_t)(speedAccumulator / SPEED_FILTER_SAMPLES);
}

// Apply rate limiting to speed changes
uint16_t rateLimitSpeed(uint16_t target, uint16_t current, uint32_t dtMs) {
  if (dtMs == 0) return current;
  
  int32_t diff = (int32_t)target - (int32_t)current;
  int32_t maxChange = ((int32_t)MAX_SPEED_CHANGE_RATE * (int32_t)dtMs) / 1000;
  
  if (diff > maxChange) {
    return current + (uint16_t)maxChange;
  } else if (diff < -maxChange) {
    return current - (uint16_t)maxChange;
  } else {
    return target;
  }
}

// Update target speed from pot during cruise (called periodically)
// NON-BLOCKING - single fast ADC read
void updateTargetSpeed(uint32_t nowMs) {
  if (state != CRUISE) return;
  if (nowMs - lastSpeedReadMs < SPEED_READ_INTERVAL_MS) return;
  
  uint32_t dtMs = nowMs - lastSpeedReadMs;
  lastSpeedReadMs = nowMs;
  
  // FAST single sample read (~100us)
  uint16_t rawSpeed = readSpeedFromPotFast();
  
  // Apply moving average filter
  uint16_t filteredSpeed = filterSpeed(rawSpeed);
  
  // Apply rate limiting
  targetStepsPerSec = rateLimitSpeed(filteredSpeed, targetStepsPerSec, dtMs);
}

void enableDrivers()  { digitalWrite(EN_PIN, LOW); }
void disableDrivers() { digitalWrite(EN_PIN, HIGH); }

// Returns the instantaneous commanded rate *for the current state* without changing state.
uint16_t instantaneousRateNow(uint32_t nowMs) {
  switch (state) {
    case RAMP_UP: {
      uint32_t t = nowMs - phaseStartMs;
      if (t >= RAMP_MS) return targetStepsPerSec;
      return lerp_u16(MIN_STEPS_PER_SEC, targetStepsPerSec, t, RAMP_MS);
    }
    case CRUISE:
      return targetStepsPerSec;

    case RAMP_DOWN: {
      // ramp from rampDownStartRate -> MIN over RAMP_MS
      uint32_t t = nowMs - phaseStartMs;
      if (t >= RAMP_MS) return 0;
      return lerp_u16(MIN_STEPS_PER_SEC, rampDownStartRate, (RAMP_MS - t), RAMP_MS);
    }

    case IDLE:
    default:
      return 0;
  }
}

void beginRun() {
  targetStepsPerSec = readSpeedFromPotBlocking();  // Blocking read at start is fine
  plannedRunMs = readRuntimeFromPot();
  
  // Initialize speed filter with starting speed
  initSpeedFilter(targetStepsPerSec);

  digitalWrite(X_DIR, nextRunForward ? HIGH : LOW);
  digitalWrite(A_DIR, nextRunForward ? HIGH : LOW);

  enableDrivers();

  runStartMs = millis();
  phaseStartMs = runStartMs;
  lastMicros = micros();
  lastSpeedReadMs = runStartMs;
  accumUnits = 0;

  state = RAMP_UP;
}

void beginRampDownFromCurrent() {
  if (state == IDLE) return;

  uint32_t nowMs = millis();
  uint16_t current = instantaneousRateNow(nowMs);

  // Make sure we don't start below MIN (keeps ramp math sensible)
  if (current < MIN_STEPS_PER_SEC) current = MIN_STEPS_PER_SEC;

  rampDownStartRate = current;
  state = RAMP_DOWN;
  phaseStartMs = nowMs;
}

void endRun() {
  disableDrivers();   // comment out if you want hold torque
  state = IDLE;
}

// Advances state machine and returns commanded rate for this moment.
// This may transition from RAMP_UP->CRUISE or CRUISE->RAMP_DOWN automatically.
uint16_t commandedRate() {
  uint32_t nowMs = millis();

  switch (state) {
    case RAMP_UP: {
      uint32_t t = nowMs - phaseStartMs;
      if (t >= RAMP_MS) {
        state = CRUISE;
        phaseStartMs = nowMs;
        lastSpeedReadMs = nowMs;  // Reset speed read timer
        return targetStepsPerSec;
      }
      return lerp_u16(MIN_STEPS_PER_SEC, targetStepsPerSec, t, RAMP_MS);
    }

    case CRUISE: {
      // Update target speed from pot (non-blocking single read)
      updateTargetSpeed(nowMs);
      
      // Start ramp-down automatically so planned run time includes ramp-down.
      uint32_t elapsed = nowMs - runStartMs;
      uint32_t cruiseUntil = (plannedRunMs > RAMP_MS) ? (plannedRunMs - RAMP_MS) : 0;

      if (elapsed >= cruiseUntil) {
        // ramp down from current cruise speed
        rampDownStartRate = targetStepsPerSec;
        state = RAMP_DOWN;
        phaseStartMs = nowMs;
        return targetStepsPerSec;
      }
      return targetStepsPerSec;
    }

    case RAMP_DOWN: {
      uint32_t t = nowMs - phaseStartMs;
      if (t >= RAMP_MS) {
        endRun();
        return 0;
      }
      return lerp_u16(MIN_STEPS_PER_SEC, rampDownStartRate, (RAMP_MS - t), RAMP_MS);
    }

    case IDLE:
    default:
      return 0;
  }
}

void setup() {
  pinMode(X_STEP, OUTPUT);
  pinMode(X_DIR, OUTPUT);
  pinMode(A_STEP, OUTPUT);
  pinMode(A_DIR, OUTPUT);
  pinMode(EN_PIN, OUTPUT);

  pinMode(PIN_START, INPUT_PULLUP);
  pinMode(PIN_REVERSE, INPUT_PULLUP);

  disableDrivers();

  btnStart.begin(PIN_START);
  btnReverse.begin(PIN_REVERSE);
}

void loop() {
  // Reverse toggles NEXT run direction (safe anytime)
  if (btnReverse.pressedEvent()) {
    nextRunForward = !nextRunForward;
  }

  // START toggles: start if idle; cancel (graceful) if running
  if (btnStart.pressedEvent()) {
    if (state == IDLE) beginRun();
    else beginRampDownFromCurrent();
  }

  if (state == IDLE) return;

  // Get commanded rate for this moment (may advance state)
  uint16_t rate = commandedRate();
  if (state == IDLE || rate == 0) return;

  // DDS stepping using elapsed micros
  uint32_t nowUs = micros();
  uint32_t dtUs = (uint32_t)(nowUs - lastMicros);
  lastMicros = nowUs;

  accumUnits += (uint64_t)rate * (uint64_t)dtUs;

  while (accumUnits >= 1000000ULL) {
    accumUnits -= 1000000ULL;
    stepBothOnce();
  }
}

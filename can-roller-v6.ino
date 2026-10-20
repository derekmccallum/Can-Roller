/*
  UNO + CNC Shield V3 + (2x) DRV8825
  - START button (A0):
      * if idle: start run (planned duration set by RUNTIME pot)
      * if running: cancel and ramp down smoothly from CURRENT speed, then stop
  - REVERSE button (A1):
      * if idle: toggles direction for NEXT run only
      * if running: ramp down, toggle direction, wait for period defined in REVERSE_RESTART_DELAY_MS,
        then ramp up again while preserving the original position reference
  - SPEED pot (A2): continuously read during run with filtering for smooth speed changes
  - RUNTIME pot (A3): read once at start, sets run duration 3-10 seconds
  - CAN SIZE switch (A4/A5): centre = 400 ml standard can (66.04 mm),
      A4 closed to GND = 250 ml slim can (53 mm),
      A5 closed to GND = third can size option
  - Position-aligned stop: planned runs stop after a whole number of selected-can rotations
    relative to the original START position, even after an in-run reverse
  - Soft start/stop: 0.5s ramp up / 0.5s ramp down
  - DDS/phase-accumulator stepping (smooth, low jitter)
  - Real-time speed adjustment with non-blocking reads

  Buttons wired to GND, using INPUT_PULLUP (pressed = LOW)
*/

#include <Arduino.h>

// CNC Shield V3 (typical)
// A_STEP/A_DIR are jumpered to X_STEP/X_DIR on the shield, so code drives X only.
const uint8_t X_STEP = 2;
const uint8_t X_DIR  = 5;
const uint8_t EN_PIN = 8;

// Inputs
const uint8_t PIN_START   = A0;
const uint8_t PIN_REVERSE = A1;
const uint8_t PIN_POT     = A2;
const uint8_t PIN_RUNTIME = A3;
const uint8_t PIN_CAN_SIZE_SLIM = A4;
const uint8_t PIN_CAN_SIZE_OPTION_3 = A5;

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

// Reverse direction pause between ramp-down and restart
const uint16_t REVERSE_RESTART_DELAY_MS = 130;

// Position-aligned stop geometry
const uint16_t MOTOR_STEPS_PER_REV = 200;
const uint8_t MICROSTEPS = 8;
const uint16_t STEPS_PER_MOTOR_REV = MOTOR_STEPS_PER_REV * MICROSTEPS;
const uint8_t PULLEY_TEETH = 20;
const uint16_t GT2_PITCH_UM = 2000; // 2 mm
const uint32_t BELT_TRAVEL_PER_MOTOR_REV_UM = (uint32_t)PULLEY_TEETH * GT2_PITCH_UM;
const uint32_t CAN_DIAMETER_SLIM_UM = 53000;      // 53.00 mm, 250 ml slim can
const uint32_t CAN_DIAMETER_STANDARD_UM = 66040;  // 66.04 mm, 400 ml standard can
const uint32_t CAN_DIAMETER_OPTION_3_UM = 58000;  // Update when the third can size is measured
const uint32_t PI_MICRO = 3141593;                // pi * 1,000,000

// State machine
enum RunState : uint8_t { IDLE, RAMP_UP, CRUISE, RAMP_DOWN };
RunState state = IDLE;

bool nextRunForward = true;
bool reverseRestartPending = false;     // Reverse pressed while running; waiting to finish ramp-down
bool reverseWaitActive = false;         // Pause after direction toggle before restart
uint32_t reverseWaitStartMs = 0;

uint16_t targetStepsPerSec = 2000;     // Target speed (filtered from pot)
uint16_t rampDownStartRate = 0;        // Captured instantaneous rate at ramp-down start
uint32_t plannedRunMs = 8000;          // Latched runtime at start

uint32_t selectedCanDiameterUm = CAN_DIAMETER_STANDARD_UM;
uint32_t selectedStepsPerCanRev = 0;
uint32_t targetRunSteps = 0;           // Minimum total pulses before the aligned stop is allowed
uint32_t stepsThisRun = 0;             // Total emitted pulses since original START
int32_t positionSteps = 0;             // Signed position from original START
bool positionStopActive = false;       // Normal planned stop should land on an aligned position
bool aligningToPositionStop = false;   // Current ramp-down is the planned aligned stop

uint32_t runStartMs = 0;               // When run began
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
uint32_t selectedCanDiameterFromSwitch() {
  if (digitalRead(PIN_CAN_SIZE_SLIM) == LOW) return CAN_DIAMETER_SLIM_UM;
  if (digitalRead(PIN_CAN_SIZE_OPTION_3) == LOW) return CAN_DIAMETER_OPTION_3_UM;
  return CAN_DIAMETER_STANDARD_UM;
}

uint32_t stepsPerCanRev(uint32_t diameterUm) {
  uint32_t circumferenceUm = (uint32_t)(((uint64_t)diameterUm * PI_MICRO + 500000ULL) / 1000000ULL);
  return (uint32_t)(((uint64_t)circumferenceUm * STEPS_PER_MOTOR_REV +
                     (BELT_TRAVEL_PER_MOTOR_REV_UM / 2)) /
                    BELT_TRAVEL_PER_MOTOR_REV_UM);
}

uint32_t rampStepEstimate(uint16_t fromRate) {
  return (uint32_t)((((uint64_t)fromRate + MIN_STEPS_PER_SEC) * RAMP_MS + 1000ULL) / 2000ULL);
}

uint32_t plannedStepEstimate(uint16_t cruiseRate, uint32_t runtimeMs) {
  uint32_t cruiseMs = (runtimeMs > (2UL * RAMP_MS)) ? (runtimeMs - (2UL * RAMP_MS)) : 0;
  uint64_t total = (uint64_t)rampStepEstimate(cruiseRate) * 2ULL;
  total += ((uint64_t)cruiseRate * cruiseMs + 500ULL) / 1000ULL;
  if (total > 0xFFFFFFFFULL) return 0xFFFFFFFFUL;
  return (uint32_t)total;
}

uint32_t alignedTargetSteps(uint16_t cruiseRate, uint32_t runtimeMs, uint32_t stepsPerRev) {
  if (stepsPerRev == 0) return 0;

  uint32_t plannedSteps = plannedStepEstimate(cruiseRate, runtimeMs);
  uint32_t rotations = (plannedSteps + stepsPerRev - 1) / stepsPerRev;
  if (rotations < 1) rotations = 1;

  uint64_t target = (uint64_t)rotations * stepsPerRev;
  if (target > 0xFFFFFFFFULL) return 0xFFFFFFFFUL;
  return (uint32_t)target;
}

uint32_t stepsToNextAlignedPosition() {
  if (selectedStepsPerCanRev == 0) return 0;

  int32_t rev = (int32_t)selectedStepsPerCanRev;
  int32_t pos = positionSteps % rev;
  if (pos < 0) pos += rev;

  if (pos == 0) return 0;
  return nextRunForward ? (uint32_t)(rev - pos) : (uint32_t)pos;
}

uint32_t remainingAlignedStopSteps() {
  if (!positionStopActive) return 0;
  if (selectedStepsPerCanRev == 0) return 0;

  uint32_t toAligned = stepsToNextAlignedPosition();
  uint32_t minRemaining = (stepsThisRun < targetRunSteps) ? (targetRunSteps - stepsThisRun) : 0;
  if (toAligned >= minRemaining) return toAligned;

  uint32_t extra = minRemaining - toAligned;
  uint32_t extraRotations = (extra + selectedStepsPerCanRev - 1) / selectedStepsPerCanRev;
  uint64_t remaining = (uint64_t)toAligned + ((uint64_t)extraRotations * selectedStepsPerCanRev);
  if (remaining > 0xFFFFFFFFULL) return 0xFFFFFFFFUL;
  return (uint32_t)remaining;
}

static inline void stepOnce() {
  digitalWrite(X_STEP, HIGH);
  delayMicroseconds(STEP_PULSE_US);
  digitalWrite(X_STEP, LOW);
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
  selectedCanDiameterUm = selectedCanDiameterFromSwitch();
  selectedStepsPerCanRev = stepsPerCanRev(selectedCanDiameterUm);
  targetRunSteps = alignedTargetSteps(targetStepsPerSec, plannedRunMs, selectedStepsPerCanRev);
  stepsThisRun = 0;
  positionSteps = 0;
  positionStopActive = true;
  aligningToPositionStop = false;

  // Initialize speed filter with starting speed
  initSpeedFilter(targetStepsPerSec);

  digitalWrite(X_DIR, nextRunForward ? HIGH : LOW);

  enableDrivers();

  runStartMs = millis();
  phaseStartMs = runStartMs;
  lastMicros = micros();
  lastSpeedReadMs = runStartMs;
  accumUnits = 0;

  state = RAMP_UP;
}

void resumeRunAfterReverse() {
  digitalWrite(X_DIR, nextRunForward ? HIGH : LOW);

  enableDrivers();

  uint32_t nowMs = millis();
  phaseStartMs = nowMs;
  lastMicros = micros();
  lastSpeedReadMs = nowMs;
  accumUnits = 0;

  state = RAMP_UP;
}

void beginRampDownFromCurrent(bool alignToTarget = false, bool preservePositionStop = false) {
  if (state == IDLE) return;

  uint32_t nowMs = millis();
  uint16_t current = instantaneousRateNow(nowMs);

  // Make sure we don't start below MIN (keeps ramp math sensible)
  if (current < MIN_STEPS_PER_SEC) current = MIN_STEPS_PER_SEC;

  rampDownStartRate = current;
  state = RAMP_DOWN;
  phaseStartMs = nowMs;
  aligningToPositionStop = alignToTarget;
  if (!alignToTarget && !preservePositionStop) positionStopActive = false;
}

void endRun(bool preservePositionStop = false) {
  disableDrivers();   // comment out if you want hold torque
  state = IDLE;
  if (!preservePositionStop) positionStopActive = false;
  aligningToPositionStop = false;
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

      if (positionStopActive && remainingAlignedStopSteps() <= rampStepEstimate(targetStepsPerSec)) {
        beginRampDownFromCurrent(true);
        return targetStepsPerSec;
      }

      return targetStepsPerSec;
    }

    case RAMP_DOWN: {
      uint32_t t = nowMs - phaseStartMs;
      if (aligningToPositionStop && remainingAlignedStopSteps() == 0) {
        endRun();
        return 0;
      }
      if (aligningToPositionStop && t >= RAMP_MS) {
        return MIN_STEPS_PER_SEC;
      }
      if (t >= RAMP_MS) {
        endRun(reverseRestartPending);
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
  pinMode(EN_PIN, OUTPUT);

  pinMode(PIN_START, INPUT_PULLUP);
  pinMode(PIN_REVERSE, INPUT_PULLUP);
  pinMode(PIN_CAN_SIZE_SLIM, INPUT_PULLUP);
  pinMode(PIN_CAN_SIZE_OPTION_3, INPUT_PULLUP);

  disableDrivers();

  btnStart.begin(PIN_START);
  btnReverse.begin(PIN_REVERSE);
}

void loop() {
  // Handle delayed restart after reverse-while-running workflow
  if (reverseWaitActive) {
    if (btnStart.pressedEvent()) {
      reverseWaitActive = false;
      reverseRestartPending = false;
      endRun();
      return;
    }

    if (millis() - reverseWaitStartMs >= REVERSE_RESTART_DELAY_MS) {
      reverseWaitActive = false;
      resumeRunAfterReverse();
    } else {
      return;
    }
  }

  // Reverse behavior:
  // - if idle: toggle direction for next run
  // - if running: ramp down, toggle, pause, then resume from the original position reference
  if (btnReverse.pressedEvent()) {
    if (state == IDLE) {
      nextRunForward = !nextRunForward;
    } else if (!reverseRestartPending) {
      reverseRestartPending = true;
      beginRampDownFromCurrent(false, true);
    }
  }

  // START toggles: start if idle; cancel (graceful) if running
  if (btnStart.pressedEvent()) {
    if (state == IDLE) beginRun();
    else {
      reverseRestartPending = false;
      reverseWaitActive = false;
      beginRampDownFromCurrent();
    }
  }

  if (state == IDLE) return;

  // Get commanded rate for this moment (may advance state)
  uint16_t rate = commandedRate();
  if (state == IDLE || rate == 0) {
    if (state == IDLE && reverseRestartPending) {
      reverseRestartPending = false;
      nextRunForward = !nextRunForward;
      reverseWaitActive = true;
      reverseWaitStartMs = millis();
    }
    return;
  }

  // DDS stepping using elapsed micros
  uint32_t nowUs = micros();
  uint32_t dtUs = (uint32_t)(nowUs - lastMicros);
  lastMicros = nowUs;

  accumUnits += (uint64_t)rate * (uint64_t)dtUs;

  if (accumUnits >= 1000000ULL) {
    if (positionStopActive && !reverseRestartPending && remainingAlignedStopSteps() == 0) {
      endRun();
      accumUnits = 0;
      return;
    }
    accumUnits -= 1000000ULL;
    stepOnce();
    stepsThisRun++;
    positionSteps += nextRunForward ? 1 : -1;
    if (positionStopActive && !reverseRestartPending && remainingAlignedStopSteps() == 0) {
      endRun();
      accumUnits = 0;
    }
  }
}

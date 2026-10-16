# Can-Roller

Arduino-driven device to roll up to 12 drink cans in unison for filming.

## Active file

Edit **`can-roller-v5.ino`** — that is the current, actively developed version. Earlier versions (v1–v4) are historical snapshots in the same repo.

## Build / flash

No build system (no PlatformIO, Makefile, or CI). Open `.ino` files directly in **Arduino IDE** or flash via `arduino-cli`. Sketch uses only the standard `Arduino.h` library — no external dependencies.

## Hardware

| Component | Detail |
|---|---|
| Board | Elegoo Uno R3 |
| Shield | CNC Shield v3 |
| Drivers | 2× DRV8825, Vref = 0.7V (→ 0.875A per phase), 1/8 microstepping (MS1=ON, MS2=ON, MS3=OFF → 1600 steps/rev) |
| Motors | 2× NEMA 17 (HANPOSE 17HS4401-S, 1.7A rated) |
| Slave link | Jumper A→X on CNC shield (A_STEP/A_DIR electrically tied to X_STEP/X_DIR; code drives X only) |
| Inputs | START (A0, INPUT_PULLUP), REVERSE (A1, INPUT_PULLUP), SPEED pot (A2), RUNTIME pot (A3) |

## Key design patterns

- **State machine**: `IDLE → RAMP_UP → CRUISE → RAMP_DOWN` (RunState enum in `can-roller-v5.ino:61`)
- **DDS stepping**: Phase-accumulator in `loop()` for smooth, low-jitter pulses (`can-roller-v5.ino:416-425`)
- **Button debouncing**: `DebouncedButton` struct with 30ms debounce window (`can-roller-v5.ino:101-130`); buttons are active-low (INPUT_PULLUP, pressed = LOW)
- **Speed filtering**: 8-sample moving average + rate limiter (3000 steps/s² max change) (`can-roller-v5.ino:44-49`)
- **Ramp profile**: 500ms ramp up / 500ms ramp down, with runtime adjusted so total = planned duration + ramp-down

## v5-specific behavior

Pressing REVERSE during a run: ramp down, toggle direction, wait 130ms (`REVERSE_RESTART_DELAY_MS`), then ramp back up. REVERSE while idle toggles direction for next run only.

Code drives only `X_STEP` and `X_DIR`. The CNC shield jumper carries those signals to A, so `A_STEP` and `A_DIR` are left unconfigured.

## Documentation

`README.md` contains full hardware wiring, pulley speed calculations, DRV8825 current setting, and potentiometer RC filter design. The schematic image `pot-filter.png` shows the RC low-pass filter circuit.

## License

GPL v3.

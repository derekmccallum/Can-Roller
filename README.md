# Can-Roller

Arduino driven device to roll up to 12 drink cans in unison for filming purposes

## Hardware

- 1 x Arduino CNC shield v3 with DRV8825 motor drivers
- 1 x Elegoo Uno R3
- 2 x HANPOSE 17HS4401-S 40mm Nema 17 Stepper Motor 42 Motor 42BYGH 1.7A 40N.cm 4-lead Motor
- 2 x GT2 2mm Pitch 6mm Width Closed Loop Synchronous Timing Belt 750 teeth 1500mm
- 2 x Machifit GT2 Timing Pulley 20 Teeth Synchronous Wheel Inner Diameter 5mm for 6mm Width Belt
- 4 x GT2 Idler Pulley 20 Teeth

## Control Input Features

| Feature | Ver | IO Pin | Description | Wiring |
|---------|----|--------|-------------|--------|
| START button | v1 | A0 | If idle: start run (planned 5s total). If running: cancel and ramp down smoothly from current speed, then stop | Buttons wired to GND, use INPUT_PULLUP (pressed = LOW) |
| REVERSE button | v1 | A1 | Toggles direction for NEXT run only | Buttons wired to GND, use INPUT_PULLUP (pressed = LOW) |
| SPEED pot | v1 | A2 | Read once at start, sets cruise step rate (no mid-run updates) | 5V → end, A2 → wiper, GND → other end |
| RUNTIME pot | v2 | A3 | Read once at start, sets run time (no mid-run updates) | 5V → end, A3 → wiper, GND → other end |

## File Versions
| File - Version | Description |
|----------------|------------------------------------------------------|
| can-roller-v1.ini | UNO + CNC Shield V3 + (2x) DRV8825<br>- START button (A0):<br>&nbsp;&nbsp;&nbsp;&nbsp;* if idle: start run (according to set runtime)<br>&nbsp;&nbsp;&nbsp;&nbsp;* if running: cancel and ramp down smoothly from CURRENT speed, then stop<br>- REVERSE button (A1): toggles direction for NEXT run only<br>- SPEED pot (A2): read once at start, sets cruise steps/sec (no mid-run updates)<br>- Soft start/stop: 0.5s ramp up / 0.5s ramp down<br>- DDS/phase-accumulator stepping (smooth, low jitter)<br>Buttons wired to GND, using INPUT_PULLUP (pressed = LOW) |
| can-roller-v1.ini | UNO + CNC Shield V3 + (2x) DRV8825<br>- START button (A0):<br>&nbsp;&nbsp;&nbsp;&nbsp;* if idle: start run (planned duration set by RUNTIME pot)<br>&nbsp;&nbsp;&nbsp;&nbsp;* if running: cancel and ramp down smoothly from CURRENT speed, then stop<br>- REVERSE button (A1): toggles direction for NEXT run only<br>- SPEED pot (A2): read once at start, sets cruise steps/sec (no mid-run updates)<br>- RUNTIME pot (A3): read once at start, sets run duration 3-10 seconds<br>- Soft start/stop: 0.5s ramp up / 0.5s ramp down<br>- DDS/phase-accumulator stepping (smooth, low jitter)<br><br>Buttons wired to GND, using INPUT_PULLUP (pressed = LOW) |
<img width="1038" alt="CNC Shield v3" src="https://europe1.discourse-cdn.com/arduino/original/4X/c/f/9/cf988326f7e0baaa42da84e9b4440201e97f966a.jpeg" />

## Power & step config

12V: 1/8 micro-stepping
Set jumpers MS1 ON, MS2 ON, MS3 OFF → 1600 steps/rev

	• DRV8825 @ 1/8 microstepping ⇒ 1600 steps/rev (200 × 8)
	• Can diameter ≈ 66 mm ⇒ circumference C ≈ 207.35 mm
	• GT2 pitch = 2 mm ⇒ belt travel per motor rev = 2 × teeth (mm)
	• Ideal no-slip rolling (and cans are constrained from translating)

Motor speed from step rate (independent of pulley teeth):
	• At 1000 steps/s: motor = 1000/1600 = 0.625 rps
	• At 9500 steps/s: motor = 9500/1600 = 5.9375 rps

Belt speed = motor_rps × (2×teeth)
Can speed (rev/s) = belt_speed / 207.35
 
A slave to X
	• Place a jumper on A → X

This electrically connects:
	• A_STEP → X_STEP
	• A_DIR  → X_DIR

So:
	• Both motors receive the same STEP and DIR signals
	• Arduino code only needs to drive X
	• A direction reversed owing to mounting


## Pulley Specs

20T pulley
Belt per rev: 40 mm

	• 1000 steps/s
		○ Belt speed: 0.625 × 40 = 25.0 mm/s
		○ Can speed: 25.0 / 207.35 = 0.1206 rps = 7.23 RPM
		○ Time per turn: 8.29 s/rev
	• 9500 steps/s
		○ Belt speed: 5.9375 × 40 = 237.5 mm/s
		○ Can speed: 237.5 / 207.35 = 1.1454 rps = 68.73 RPM
		○ Time per turn: 0.873 s/rev

24T pulley
Belt per rev: 48 mm

	• 1000 steps/s
		○ Belt speed: 0.625 × 48 = 30.0 mm/s
		○ Can speed: 30.0 / 207.35 = 0.1447 rps = 8.68 RPM
		○ Time per turn: 6.91 s/rev
	• 9500 steps/s
		○ Belt speed: 5.9375 × 48 = 285.0 mm/s
		○ Can speed: 285.0 / 207.35 = 1.3745 rps = 82.47 RPM
		○ Time per turn: 0.728 s/rev


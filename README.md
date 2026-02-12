# Can-Roller

Arduino driven device to roll up to 12 drink cans in unison for filming purposes

## Hardware

- 1 x Arduino CNC shield v3 with A4988/DRV8825 motor drivers
- 1 x Elegoo Uno R3
- 2 x HANPOSE 17HS4401-S 40mm Nema 17 Stepper Motor 42 Motor 42BYGH 1.7A 40N.cm 4-lead Motor
- 2 x GT2 2mm Pitch 6mm Width Closed Loop Synchronous Timing Belt 750 teeth 1500mm
- 2 x Machifit GT2 Timing Pulley 20 Teeth Synchronous Wheel Inner Diameter 5mm for 6mm Width Belt
- 4 x GT2 Idler Pulley 20 Teeth

## Control Input Features

| Feature | IO Pin | Description | Wiring |
|---------|--------|-------------|--------|
| START button | A0 | If idle: start run (planned 5s total)<br>If running: cancel and ramp down smoothly from current speed, then stop | Buttons wired to GND, use INPUT_PULLUP (pressed = LOW) |
| REVERSE button | A1 | Toggles direction for NEXT run only | Buttons wired to GND, use INPUT_PULLUP (pressed = LOW) |
| SPEED pot | A2 | Read once at start, sets cruise step rate (no mid-run updates) | 5V → end<br>A2 → wiper<br>GND → other end |
| RUNTIME pot | A3 | Read once at start, sets run time (no mid-run updates) | 5V → end<br>A3 → wiper<br>GND → other end |

![Can-Roller Hardware](https://github.com/user-attachments/assets/e6c2bad5-db18-4510-8bad-494698126975)

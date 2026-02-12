# Can-Roller
Arduino driven device to roll up to 12 drink cans in unison for filming purposes

HARDWARE
1 x Auduino CNC shield v3 with A4988/DRV8825 motor drivers

1 x Elegoo Uno R3

2 x HANPOSE 17HS4401-S 40mm Nema 17 Stepper Motor 42 Motor 42BYGH 1.7A 40N.cm 4-lead Motor

2 x GT2 2mm Pitch 6mm Width Closed Loop Synchronous Timing Belt 750 teeth 1500mm

2 x Machifit GT2 Timing Pulley 20 Teeth Synchronous Wheel Inner Diameter 5mm for 6mm Width Belt 

4 x GT2 Idler Pulley 20 Teeth 


CONTROL INPUT FEATURES
Feature	IO Pin	Description	Wiring
START button	A0	      * if idle: start run (planned 5s total)	Buttons wired to GND, use INPUT_PULLUP (pressed = LOW)
		      * if running: cancel and ramp down smoothly from CURRENT speed, then stop
REVERSE button	A1	toggles direction for NEXT run only	Buttons wired to GND, use INPUT_PULLUP (pressed = LOW)
SPEED pot	A2	read once at start, sets cruise step rate (no mid-run updates)
	Pot: 
			5V -> end
			A2 -> wiper
			GND -> other end
RUNTIME pot	A3	read once at start, sets run time (no mid-run updates)	Pot: 
			5V -> end
			A3 -> wiper
			GND -> other end
			
<img width="1038" height="450" alt="image" src="https://github.com/user-attachments/assets/e6c2bad5-db18-4510-8bad-494698126975" />

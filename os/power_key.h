// The power button and a clean shutdown, on the device.
#pragma once

// Finds the input devices that report the power key. Returns how many.
int power_key_open(void);

// 1 once per press. Never blocks.
int power_key_pressed(void);

// Saves pending writes to the card, then asks the system to power off.
// force skips the orderly shutdown, for when that has hung.
void power_off(int force);

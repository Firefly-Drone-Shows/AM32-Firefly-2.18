# Firefly Hard-Coded ESC Settings

This document decodes the 192-byte `fireflySettings[]` array in `Src/main.c` for the Firefly fork of AM32, target `FIREFLY_G20_F051`. These settings are loaded into `eepromBuffer` at every boot via `setFireflySettings()`, overriding whatever is stored in flash EEPROM.

Byte offsets correspond to the `EEprom_t` struct defined in `Inc/eeprom.h`. **Note:** the 2.20 EEPROM layout differs from 2.18 — the `firmware_name` field that lived at bytes 5–16 is gone, replaced by new settings (max_ramp, minimum_duty_cycle, current PID, active brake power). Firmware name now comes from the `FIRMWARE_NAME` define in `targets.h`.

## Important behavior note

The hard-coded settings are loaded into **RAM only** — they are never written to the EEPROM flash sector. As a result, the AM32 web configurator (am32.ca) will display whatever stale or uninitialized values are physically in the flash sector, **not** the values shown below. The ESC itself runs from RAM and behaves according to these hard-coded values regardless of what the configurator displays.

## Header / EEPROM identity

| Byte | Setting | Hex | Value |
|------|---------|-----|-------|
| 0 | reserved_0 | 0x01 | — |
| 1 | eeprom_version | 0x03 | Layout v3 (matches AM32 2.20) |
| 2 | reserved_1 | 0x0B | — |
| 3–4 | firmware version | macros | 2.20 (uses `VERSION_MAJOR` / `VERSION_MINOR`) |

## New 2.20 settings (bytes 5–16)

| Byte | Setting | Hex | Meaning |
|------|---------|-----|---------|
| 5 | max_ramp | 0x32 | **5.0 % duty per ms** ramp rate |
| 6 | minimum_duty_cycle | 0x07 | **3.5 %** minimum duty cycle |
| 7 | disable_stick_calibration | 0x00 | Stick calibration enabled (default) |
| 8 | absolute_voltage_cutoff | 0x00 | Off (low-voltage cutoff disabled) |
| 9 | current_P | 0x64 | 100 (stock default) |
| 10 | current_I | 0x00 | 0 (stock default) |
| 11 | current_D | 0x64 | 100 (stock default) |
| 12 | active_brake_power | 0x00 | Off |
| 13–16 | reserved_eeprom_3 | 0x00 ×4 | — |

## Motor direction & mode

| Byte | Setting | Hex | Meaning |
|------|---------|-----|---------|
| 17 | dir_reversed | 0x00 | Normal direction (not reversed) |
| 18 | bi_direction | 0x00 | Unidirectional (not 3D mode) |
| 19 | use_sine_start | 0x00 | Sinusoidal startup OFF |
| 20 | comp_pwm | 0x01 | Complementary PWM ON |
| 21 | variable_pwm | 0x00 | Fixed PWM frequency (not variable) |
| 22 | stuck_rotor_protection | 0x00 | OFF |
| 23 | advance_level | 0x1E | **18.75°** timing advance (new format: `(degrees / 0.9375) + 10` = 30) |

## PWM & power

| Byte | Setting | Hex | Meaning |
|------|---------|-----|---------|
| 24 | pwm_frequency | 0x18 | **24 kHz** |
| 25 | startup_power | 0x5A | **90 %** |
| 26 | motor_kv | 0x18 | **980 KV** (formula: 24 × 40 + 20) |
| 27 | motor_poles | 0x0E | **14 poles** (7 pole pairs) |

## Braking & protections

| Byte | Setting | Hex | Meaning |
|------|---------|-----|---------|
| 28 | brake_on_stop | 0x00 | OFF |
| 29 | stall_protection | 0x00 | OFF |
| 30 | beep_volume | 0x0A | **10** |
| 31 | telemetry_on_interval | 0x00 | OFF (no 30 ms KISS telemetry) |

## Servo input calibration

| Byte | Setting | Hex | Meaning |
|------|---------|-----|---------|
| 32 | servo.low_threshold | 0x91 | **1040 µs** (low end of stick) |
| 33 | servo.high_threshold | 0x69 | **1960 µs** (high end of stick) |
| 34 | servo.neutral | 0x7E | **1500 µs** (center / neutral) |
| 35 | servo.dead_band | 0x32 | **50 µs** dead band around neutral |

Transformations (from `main.c`):
- `servo_low_threshold  = low_threshold  * 2 + 750`
- `servo_high_threshold = high_threshold * 2 + 1750`
- `servo_neutral        = neutral + 1374`

## Battery cutoff

| Byte | Setting | Hex | Meaning |
|------|---------|-----|---------|
| 36 | low_voltage_cut_off | 0x00 | OFF (low-voltage cutoff disabled) |
| 37 | low_cell_volt_cutoff | 0x32 | 3.0 V/cell (formula: 50 + 250) — moot since cutoff is off |

## Throttle behavior

| Byte | Setting | Hex | Meaning |
|------|---------|-----|---------|
| 38 | rc_car_reverse | 0x00 | OFF (not RC car bidirectional) |
| 39 | use_hall_sensors | 0x00 | OFF |
| 40 | sine_mode_changeover | 0x05 | 5 % throttle (default min) |
| 41 | drag_brake_strength | 0x0A | 10 (default max) |
| 42 | driving_brake_strength | 0x03 | **Level 3** (lower = weaker brake; sets dead_time_override = DEAD_TIME + 120) |

## Limits

| Byte | Setting | Hex | Meaning |
|------|---------|-----|---------|
| 43 | limits.temperature | 0xFF | Disabled (255 = no temp limit) |
| 44 | limits.current | 0x00 | Disabled (0 = no current limit) |
| 45 | sine_mode_power | 0x05 | 5 (default) |
| 46 | input_type | 0x01 | **DSHOT_IN** (1) — DShot protocol forced |
| 47 | auto_advance | 0x00 | OFF |

## Startup tune (bytes 48–175)

All 128 bytes set to **0xFF** (`ERASED_FLASH_BYTE`). This means no custom BlueJay tune is stored, so `playStartupTune()` plays the default Firefly 3-tone ascending startup sound.

## CAN / DroneCAN (bytes 176–191)

All 16 bytes set to **0xFF** (erased-flash sentinel). CAN is unconfigured — this ESC does not use DroneCAN.

## Summary

DShot input, 14-pole 980 KV motor, fixed 24 kHz PWM with complementary output, **18.75° fixed timing advance** (auto-advance off), 90 % startup power, 5.0 % duty/ms ramp rate, 3.5 % minimum duty cycle, **driving brake level 3** (relatively weak — gives more dead time during deceleration), max drag brake, no battery cutoff, no temperature/current limits, normal direction, no sine startup, no stall protection. Standard 1000–2000 µs servo range with 1500 µs center is configured but unused at runtime since DShot is forced. Default 3-tone Firefly startup tune.

# RTOS-based Button-Driven Menu Navigation Firmware

Firmware for an ESP32-based embedded device with 3 physical buttons, a 16-LED bar driven via 74HC595 shift register, and a button-navigated menu UI. Built on FreeRTOS tasks, queues, and a mutex for hardware access.

## Features

- Debounced GPIO input scanned every 5 ms
- Single, double, triple, and long-press detection with low-latency resolution
- Menu system: Main Menu → Brightness / Mode / Info / Reset
- Manual and Auto LED pattern modes (Auto cycles patterns every 2 seconds)
- Brightness control (0–10) applied via PWM on the shift register's OE pin
- Power-off and restart handling (BTN3 Long to power off, BTN1 Long to restart)
- Reset to defaults (Brightness 5, Manual mode, Pattern 1)

## Hardware

| Component | Pin |
|---|---|
| Button 1 | GPIO 21 |
| Button 2 | GPIO 47 |
| Button 3 | GPIO 48 |
| Shift register DS (data) | GPIO 4 |
| Shift register OE (output enable) | GPIO 5 |
| Shift register STCP (storage clock) | GPIO 6 |
| Shift register SHCP (shift clock) | GPIO 7 |
| Shift register MR (master reset) | GPIO 15 |

Buttons are wired with `INPUT_PULLUP` (active LOW). Brightness is implemented via PWM on the OE pin (active LOW), so higher brightness = lower PWM duty on OE.

Circuit diagram: https://wokwi.com/projects/452559063452352513

## Architecture

Three FreeRTOS tasks communicate via queues and a mutex:

```
ButtonTask  ──► btn_evt_queue ──► MenuTask ──► display_queue ──► DisplayTask
(core 0)                          (core 1)                        (core 1, owns shiftreg_mutex)
```

- **ButtonTask**: Reads and debounces GPIO inputs every 5 ms, detects single/double/triple/long presses, and pushes `btn_event_t` events to `btn_evt_queue`.
- **MenuTask**: Consumes button events, maintains menu state, and pushes the resulting LED pattern (`uint16_t`) to `display_queue`.
- **DisplayTask**: Consumes patterns and writes them to the shift register, guarded by `shiftreg_mutex`.
- **AutoTimerCallback**: A FreeRTOS software timer that advances the pattern every 2 seconds while in Auto mode.

## Press Timing Rules

| Gesture | Condition |
|---|---|
| Single press | Press and release within 300 ms, no further tap |
| Double press | Two taps resolved within 500 ms |
| Triple press | Three taps resolved within 700 ms (resolves immediately on 3rd release) |
| Long press | Hold longer than 1500 ms |

## Menu Map

```
Main Menu
├── Brightness   (BTN2: +, BTN3: -, BTN1: back to Main)
├── Mode
│   ├── Manual    (BTN1: cycle pattern, BTN2: save, BTN3: cancel)
│   └── Auto      (auto-cycles every 2s, BTN1: exit)
├── Info          (BTN1: next screen, BTN3: return)
└── Reset         (BTN2 Double: confirm, BTN3: cancel)
```

Main menu navigation: BTN1 = next item, BTN3 = previous item, BTN2 = enter, BTN3 Long = power off.

Power off: all LEDs off, ignores all input except BTN1 Long (restart).

## Build & Flash

1. Open `project.ino` in the Arduino IDE (or PlatformIO) with the ESP32 board package installed (core 3.x — uses `ledcAttach`/`ledcWrite`).
2. Select your ESP32 board and port.
3. Compile and upload.

To enable debug logging over Serial (115200 baud), set `DEBUG_PRINTS` to `1` at the top of `project.ino`.

## Defaults

| Setting | Value |
|---|---|
| Brightness | 5 |
| Mode | Manual |
| Pattern | Pattern 1 |

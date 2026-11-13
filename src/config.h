// Tunables for display, touch and timing. Touch calibration values are the
// common CYD defaults — refine them with the serial output once hardware is
// in hand (tap the corners, read the raw values, adjust min/max).
#pragma once

// --- Touch (XPT2046 on its own SPI bus) ---
#define TOUCH_CLK_PIN 25
#define TOUCH_MOSI_PIN 32
#define TOUCH_MISO_PIN 39
#define TOUCH_CS_PIN 33
#define TOUCH_IRQ_PIN 36

// Raw ADC range mapped to screen pixels (after XPT2046 setRotation)
#define TOUCH_RAW_X_MIN 200
#define TOUCH_RAW_X_MAX 3700
#define TOUCH_RAW_Y_MIN 240
#define TOUCH_RAW_Y_MAX 3800

// Resistive panels are noisy: a press must be seen on consecutive reads, and
// accepted presses are rate-limited.
#define TOUCH_DEBOUNCE_MS 400

// --- Screen sleep ---
// Backlight switches off after this long without a touch, so the box isn't a
// night light in the van. Any tap wakes it; that tap never presses a button.
#define SCREEN_SLEEP_MS 60000

// --- Timing ---
#define UI_REFRESH_MS 1000          // numeric field refresh cadence
#define STALE_TELEMETRY_MS 10000    // show "NO SIGNAL" past this age
#define TOGGLE_CONFIRM_TIMEOUT_MS 4000  // pending -> failed without telemetry confirm
#define WATCHDOG_TIMEOUT_S 15

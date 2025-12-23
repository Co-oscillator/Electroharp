#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// --- Pin Definitions ---
// Display Pins are defined in platformio.ini via build_flags
// Touch Pins
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

// Audio Output
#define AUDIO_PIN 26 // DAC2

// Input Pins
#define LDR_PIN 34

// --- Audio Settings ---
#define SAMPLE_RATE 44100
#define MAX_VOICES 8
#define SAW_MAX 255

// --- UI Settings ---
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define STRING_COUNT 37   // 3 Octaves (C3 to C6)
#define STRING_HEIGHT 180 // Top 3/4 roughly
#define BUTTON_HEIGHT 60  // Bottom section

// Colors
#define COLOR_BG TFT_BLACK
#define COLOR_STRING_WHITE TFT_WHITE
#define COLOR_STRING_BLACK TFT_DARKGREY
#define COLOR_BTN_INACTIVE TFT_BLUE
#define COLOR_BTN_ACTIVE TFT_GREEN
#define COLOR_TEXT TFT_WHITE

#endif

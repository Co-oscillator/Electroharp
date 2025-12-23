#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// --- Pin Definitions ---
// Display Pins (ST7796 via PlatformIO build_flags)
// Touch Pins (Shared SPI on 3.5" CYD)
#define XPT2046_IRQ 36
#define XPT2046_MOSI 13
#define XPT2046_MISO 12
#define XPT2046_CLK 14
#define XPT2046_CS 33

// Audio Output
#define AUDIO_PIN 26          // DAC2
#define SPEAKER_ENABLE_PIN -1 // Not used/Integrated on 3.5"

// Power / Backlight Control
#define PIN_POWER_ENABLE 4 // Shared with 2.8 revision for speaker amp
#define PIN_BL 27          // Backlight on 3.5
#define PIN_EXT_POWER 22   // Keep 22 high just in case (Common)

// Input Pins
#define LDR_PIN 34

// --- Audio Settings ---
#define SAMPLE_RATE 44100 // High Fidelity
#define MAX_VOICES 24     // High Polyphony (Rev 21)
#define SAW_MAX 255

// --- UI Settings ---
#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 320
#define STRING_COUNT 109  // 9 Octaves + 1 (C1 to C10)
#define STRING_HEIGHT 220 // Scaled for 320px height
#define BUTTON_HEIGHT 80  // Scaled for 320px height

// --- Sound Profile ---
#define CONFIG_VERSION 0xCAFEBABE // Settings Version for Forced Wipe

struct SoundProfile {
  float gainSaw;
  float gainSquare;
  float gainSine;
  float gainTri;
  float masterGain;
  float releaseTime; // Default Global Release
};

extern SoundProfile *currentProfile;

// --- Editor Settings ---
enum LfoType { LFO_SINE, LFO_SQUARE, LFO_RAMP, LFO_NOISE };
enum LfoTarget {
  TARGET_NONE,
  TARGET_FOLD,
  TARGET_FILTER,
  TARGET_RES,
  TARGET_PITCH,
  TARGET_ATTACK,
  TARGET_RELEASE,
  TARGET_LFO_DEPTH
};

enum ArpMode {
  ARP_OFF,
  ARP_UP,
  ARP_DOWN,
  ARP_UPDOWN,
  ARP_WALK,
  ARP_RANDOM,
  ARP_SPARKLE,
  ARP_SPARKLE2
};

struct SynthParameters {
  float filterCutoff;  // 100 - 4000
  float filterRes;     // 0.01 - 0.90
  float waveFold;      // 0.25 - 0.75
  float delayFeedback; // 0.05 - 0.70
  float attackTime;    // 0.001 - 0.150
  float releaseTime;   // 0.100 - 3.000
  float lfoRate;       // 0.125 - 10.0
  float lfoDepth;      // 0.0 - 1.0 (New)
  float driveAmount;   // 0.05 - 0.60
  float tremRate;      // 0.25 - 5.0
  LfoType lfoType;
  LfoTarget lfoTarget;
  int octaveRange; // Range in octaves (1-8)
};

extern SynthParameters activeParams;
extern volatile int activeSampleRate;

// Colors
#define COLOR_BG TFT_BLACK
#define COLOR_STRING_WHITE 0xFFF0 // Pale Cream Yellow (Desaturated)
#define COLOR_STRING_BLACK 0x9E7D // Pale Sky Blue
#define COLOR_BTN_INACTIVE TFT_BLUE
#define COLOR_BTN_ACTIVE TFT_GREEN
#define COLOR_TEXT TFT_WHITE

#endif

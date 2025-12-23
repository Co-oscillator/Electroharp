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
#define SPEAKER_ENABLE_PIN 21

// Input Pins
#define LDR_PIN 34

// --- Audio Settings ---
#define SAMPLE_RATE 22050
#define MAX_VOICES 24
#define SAW_MAX 255

// --- UI Settings ---
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define STRING_COUNT 97   // 8 Octaves + 1 (C2 to C10)
#define STRING_HEIGHT 140 // strings drawn from 60 to 140
#define BUTTON_HEIGHT 60  // Bottom section

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

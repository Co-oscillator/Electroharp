#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// --- Pin Definitions ---
// Display Pins are defined in platformio.ini via build_flags
// Touch Calibration (Approximate for 3.5" HOSYOND)
// These need to be fine-tuned.
// Raw Range ~ 3680 (Left/Top) to ~460 (Right/Bottom)
// Inverted Axis Logic handled in map()
#define TS_MINX 460
#define TS_MAXX 3680
#define TS_MINY 350
#define TS_MAXY 3770
// Touch Pins
#define XPT2046_IRQ 36
#define XPT2046_MOSI 13
#define XPT2046_MISO 12
#define XPT2046_CLK 14
#define XPT2046_CS 33
// CYD 3.5" RGB LED Pins (Sunton: Red/PA=21, Green=16, Blue=17)
#define LED_R 21
#define LED_G 16
#define LED_B 17
#define LED_INVERTED false // Active HIGH for PA and LED on this variant

// Audio Output
#define AUDIO_PIN 26 // DAC2

// Input Pins
#define LDR_PIN 34

// --- Audio Settings ---
#define DELAY_DOWNSAMPLE                                                       \
  2 // 1=Full, 2=Half, 4=Quarter. 2 reduces aliasing/whine.
// Dynamic Sample Rate (32k Speaker / 44.1k BT)
extern int currentSampleRate;
#define MAX_VOICES 16
#define SAW_MAX 255
#define CONFIG_VERSION 0xCAFEBABE // Settings Version for Forced Wipe

// --- UI Settings ---
#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 320
#define STRING_COUNT 49 // 4 Octaves (C2 to C6)
#define STRING_HEIGHT                                                          \
  160 // Reduced to give more space to Volume/Transpose/TopBar
#define BUTTON_HEIGHT 80 // Bottom section (was 60)

// --- Sound Profile ---
struct SoundProfile {
  float gainSaw;
  float gainSquare;
  float gainSine;
  float gainTri;
  float masterGain;
  float releaseTime; // Default Global Release
};

extern SoundProfile *currentProfile;

struct LEDConfig {
  int pinR;
  int pinG;
  int pinB;
  bool inverted;
  bool enabled;
};

extern LEDConfig globalLedConfig;

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
  LfoTarget ldrTarget; // Light Sensor Target (New)
};

extern SynthParameters activeParams;

// Colors
#define COLOR_BG TFT_BLACK
#define COLOR_STRING_WHITE TFT_WHITE
#define COLOR_STRING_BLACK TFT_DARKGREY
#define COLOR_BTN_INACTIVE TFT_BLUE
#define COLOR_BTN_ACTIVE TFT_GREEN
#define COLOR_TEXT TFT_WHITE

#endif

#ifndef SYNTH_VOICE_H
#define SYNTH_VOICE_H

#include "Config.h"
#include <Arduino.h>

enum Waveform { WAVE_SAW, WAVE_SQUARE, WAVE_SINE, WAVE_TRIANGLE };
enum EnvState { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

class SynthVoice {
public:
  bool active = false;
  bool held = false;
  bool isLatchedArp = false; // New flag
  bool isSparkle = false;    // New flag for Sparkle Mode
  int noteIndex = -1;

  float frequency;
  float phase;
  float phaseIncrement;
  float pulseWidth = 0.5f;
  float mixGain = 1.0f; // Input Gain per-voice

  Waveform waveform = WAVE_SAW; // Default

  // ADSR State
  float envelope = 0.0f;
  EnvState envState = ENV_IDLE;

  // ADSR Settings (Stored)
  float attackTime = 0.01f;
  float decayTime = 0.1f;
  float sustainLvl = 0.7f;
  float releaseTime = 0.3f;

  // Calculated Rates
  float attackRate;
  float decayRate;
  float releaseRate;

  // Setters
  void setWaveform(Waveform w) { waveform = w; }
  void setPulseWidth(float pw) { pulseWidth = pw; }
  void setADSR(float a, float d, float s, float r) {
    attackTime = a;
    decayTime = d;
    sustainLvl = s;
    releaseTime = r;
    // Pre-calc rates not needed here if calculated in trigger,
    // but good for optimization if changed rarely.
    // For now, calc in trigger to be safe or calc here?
    // Let's calc in trigger to ensure sample rate is respected at runtime ?
    // No, sample rate is constant. Calc here is efficient.
  }

  // Simplifed Trigger
  void trigger(float freq, int noteIdx);

  // Full Trigger
  void trigger(float freq, int noteIdx, Waveform wave, float pw, float attack,
               float decay, float sustain, float release);

  void release();

  // Generate one sample
  float IRAM_ATTR getSample(float pitchMod, float pwMod);

  // Static Resources
  static float sineLUT[256];
  static void initLUT();
};

#endif
